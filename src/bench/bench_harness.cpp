// bench_harness.cpp — benchmark 框架的實作（純標準 C++17）

#include <vfs_bench.h>
#include <vfs_platform.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace vfsbench {
namespace {

std::vector<Pattern>& Registry() {
	static std::vector<Pattern> r;
	return r;
}

// 指標的唯一鍵：pattern/variant/metric
std::string KeyOf(const Result& r, const Metric& m) {
	return r.Label() + "." + m.name;
}

} // namespace

// ---------------------------------------------------------------- Result

void Result::Add(const std::string& name, double value, const std::string& unit,
                 bool higher_is_better) {
	Metric m;
	m.name = name;
	m.value = value;
	m.unit = unit;
	m.higher_is_better = higher_is_better;
	m.valid = true;
	metrics.push_back(m);
}

void Result::AddInfo(const std::string& name, double value, const std::string& unit,
                     bool higher_is_better) {
	Add(name, value, unit, higher_is_better);
	metrics.back().gated = false;
}

void Result::AddInvalid(const std::string& name, const std::string& unit) {
	Metric m;
	m.name = name;
	m.unit = unit;
	m.valid = false;
	metrics.push_back(m);
}

std::string Result::Label() const {
	return variant.empty() ? pattern : (pattern + "/" + variant);
}

// ---------------------------------------------------------------- Samples

void Samples::EnsureSorted() {
	if (!sorted_) { std::sort(v_.begin(), v_.end()); sorted_ = true; }
}

double Samples::Min() { if (v_.empty()) return 0.0; EnsureSorted(); return v_.front(); }
double Samples::Max() { if (v_.empty()) return 0.0; EnsureSorted(); return v_.back(); }

double Samples::Sum() const {
	double s = 0.0;
	for (double x : v_) s += x;
	return s;
}

double Samples::Mean() const {
	return v_.empty() ? 0.0 : Sum() / static_cast<double>(v_.size());
}

double Samples::Percentile(double pct) {
	if (v_.empty()) return 0.0;
	EnsureSorted();
	if (pct <= 0.0) return v_.front();
	if (pct >= 100.0) return v_.back();
	// 線性插值，避免樣本數少時分位數跳動
	const double pos = (pct / 100.0) * static_cast<double>(v_.size() - 1);
	const size_t lo = static_cast<size_t>(pos);
	const size_t hi = (lo + 1 < v_.size()) ? lo + 1 : lo;
	const double frac = pos - static_cast<double>(lo);
	return v_[lo] + (v_[hi] - v_[lo]) * frac;
}

// ---------------------------------------------------------------- 快取控制

void EvictOsCache(size_t bytes) {
	if (bytes == 0) return;

	// 寫一個 ballast 檔再整個讀回來。OS 的檔案快取容量有限，塞進這麼多不相干的
	// 資料就會把先前封包的內容擠出去。這是標準 C++ 範圍內唯一可行的做法
	// —— 不保證徹底，但足以消掉「剛寫完的封包還完整躺在 RAM 裡」這種最嚴重的污染。
	const std::string path = vfsplat::PathJoin(vfsplat::ScratchDir(), "vfs_cache_ballast.tmp");

	const size_t kChunk = 4u << 20;             // 4MB 一批
	std::vector<char> buf(kChunk, 0x5A);

	std::FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp) return;
	for (size_t written = 0; written < bytes; written += kChunk) {
		const size_t n = (bytes - written < kChunk) ? (bytes - written) : kChunk;
		if (std::fwrite(buf.data(), 1, n, fp) != n) break;
	}
	std::fflush(fp);
	std::fclose(fp);

	// 再讀一次，確保這些頁真的進了快取（只寫的話可能還壓在延遲寫回佇列裡）
	fp = std::fopen(path.c_str(), "rb");
	if (fp) {
		volatile unsigned long long sink = 0;   // 防止最佳化把讀取整個拿掉
		for (;;) {
			const size_t n = std::fread(buf.data(), 1, kChunk, fp);
			if (!n) break;
			sink += static_cast<unsigned char>(buf[0]) + static_cast<unsigned char>(buf[n - 1]);
		}
		(void)sink;
		std::fclose(fp);
	}
	vfsplat::FileDelete(path.c_str());
}

// ---------------------------------------------------------------- Timer

Timer::Timer() : start_(vfsplat::MonotonicSeconds()) {}
void   Timer::Reset() { start_ = vfsplat::MonotonicSeconds(); }
double Timer::Elapsed() const { return vfsplat::MonotonicSeconds() - start_; }

// ---------------------------------------------------------------- 註冊與執行

void RegisterPattern(const Pattern& p) { Registry().push_back(p); }

void ListPatterns() {
	std::printf("可用的 benchmark pattern：\n");
	for (const Pattern& p : Registry())
		std::printf("  %-12s %s\n", p.name, p.description);
}

std::vector<Result> RunPatterns(const std::vector<std::string>& filter, const Options& opt) {
	std::vector<Result> all;
	for (const Pattern& p : Registry()) {
		if (!filter.empty() &&
		    std::find(filter.begin(), filter.end(), p.name) == filter.end()) continue;

		std::printf("跑 %s … ", p.name);
		std::fflush(stdout);
		const Timer t;

		if (opt.repeats <= 1) {
			EvictOsCache(static_cast<size_t>(opt.cold_mb) << 20);
			p.fn(opt, &all);
		} else {
			// 重複多次，每個指標取「最好的一次」而不是中位數。
			//
			// 中位數看起來比較「公正」，實際上會把背景干擾一起平均進來：
			// 這些 pattern 每輪要寫上百 MB，OS 的延遲寫回、其他行程、熱節流
			// 都只會讓數字變差，不會讓它變好。實測用中位數時，同一份程式碼
			// 連跑兩次會出現 -8% 與 -33% 兩種結果，門檻根本沒法用。
			// 取最佳值 = 取干擾最少的那一輪，這是 benchmark 的標準做法。
			std::vector<std::vector<Result>> rounds;
			for (int i = 0; i < opt.repeats; ++i) {
				// 每一輪都從同樣的起點開始，否則第 2 輪會沾到第 1 輪留在 OS 快取裡的
				// 東西，取最佳值時就固定會選到「最熱」的那一輪。
				EvictOsCache(static_cast<size_t>(opt.cold_mb) << 20);
				std::vector<Result> one;
				p.fn(opt, &one);
				rounds.push_back(std::move(one));
			}
			if (!rounds.empty()) {
				std::vector<Result> merged = rounds.front();
				for (size_t ri = 0; ri < merged.size(); ++ri) {
					for (size_t mi = 0; mi < merged[ri].metrics.size(); ++mi) {
						std::vector<double> vals;
						for (const std::vector<Result>& round : rounds) {
							if (ri < round.size() && mi < round[ri].metrics.size() &&
							    round[ri].metrics[mi].valid)
								vals.push_back(round[ri].metrics[mi].value);
						}
						if (!vals.empty()) {
							std::sort(vals.begin(), vals.end());
							// 越大越好 → 取最大；越小越好 → 取最小
							merged[ri].metrics[mi].value =
								merged[ri].metrics[mi].higher_is_better ? vals.back() : vals.front();
						}
					}
				}
				for (Result& r : merged) all.push_back(std::move(r));
			}
		}
		std::printf("%.2f s\n", t.Elapsed());
	}
	return all;
}

void PrintResults(const std::vector<Result>& results) {
	std::string current;
	for (const Result& r : results) {
		if (current != r.Label()) {
			current = r.Label();
			std::printf("\n[%s]\n", current.c_str());
		}
		for (const Metric& m : r.metrics) {
			if (!m.valid) {
				std::printf("  %-28s %12s %s\n", m.name.c_str(), "n/a", m.unit.c_str());
				continue;
			}
			// 依數量級選小數位，讓表格對得整齊又不失精度
			const double a = std::fabs(m.value);
			const char* fmt = (a >= 1000.0) ? "  %-28s %12.0f %s\n"
			                : (a >= 10.0)   ? "  %-28s %12.2f %s\n"
			                                : "  %-28s %12.4f %s\n";
			std::printf(fmt, m.name.c_str(), m.value, m.unit.c_str());
		}
	}
}

// ---------------------------------------------------------------- JSON 輸出

namespace {

void JsonEscape(const std::string& s, std::string* out) {
	for (char c : s) {
		switch (c) {
			case '"':  *out += "\\\""; break;
			case '\\': *out += "\\\\"; break;
			case '\n': *out += "\\n";  break;
			case '\r': *out += "\\r";  break;
			case '\t': *out += "\\t";  break;
			default:   *out += c;      break;
		}
	}
}

} // namespace

bool WriteJson(const std::string& path, const std::vector<Result>& results,
               const Options& opt) {
	std::string s;
	char num[64];

	s += "{\n";
	s += "  \"format\": 1,\n";
	std::snprintf(num, sizeof(num), "%.6g", opt.scale);
	s += std::string("  \"scale\": ") + num + ",\n";
	std::snprintf(num, sizeof(num), "%llu", opt.seed);
	s += std::string("  \"seed\": ") + num + ",\n";
	s += "  \"results\": [\n";

	for (size_t i = 0; i < results.size(); ++i) {
		const Result& r = results[i];
		s += "    {\n      \"pattern\": \"";
		JsonEscape(r.pattern, &s);
		s += "\",\n      \"variant\": \"";
		JsonEscape(r.variant, &s);
		s += "\",\n      \"metrics\": [\n";
		for (size_t j = 0; j < r.metrics.size(); ++j) {
			const Metric& m = r.metrics[j];
			s += "        { \"name\": \"";
			JsonEscape(m.name, &s);
			s += "\", \"value\": ";
			if (m.valid) {
				std::snprintf(num, sizeof(num), "%.10g", m.value);
				s += num;
			} else {
				s += "null";
			}
			s += ", \"unit\": \"";
			JsonEscape(m.unit, &s);
			s += "\", \"higher_is_better\": ";
			s += m.higher_is_better ? "true" : "false";
			s += ", \"gated\": ";
			s += m.gated ? "true" : "false";
			s += " }";
			if (j + 1 < r.metrics.size()) s += ",";
			s += "\n";
		}
		s += "      ]\n    }";
		if (i + 1 < results.size()) s += ",";
		s += "\n";
	}
	s += "  ]\n}\n";

	std::FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp) return false;
	const bool ok = std::fwrite(s.data(), 1, s.size(), fp) == s.size();
	return (std::fclose(fp) == 0) && ok;
}

// ---------------------------------------------------------------- JSON 讀取
//
// 只認得上面 WriteJson 產出的格式。刻意寫得寬鬆（找 key 而不是嚴格解析），
// 因為它唯一的用途就是讀回自己寫出去的 baseline。

namespace {

// 從 pos 起找 "key": 。回傳「值」的位置，key_pos_out 收「鍵」的位置。
// 兩者都要：要推進到下一筆 result 時必須用鍵的位置，用值的位置會整筆跳過。
size_t FindValue(const std::string& s, const std::string& key, size_t pos,
                 size_t* key_pos_out = nullptr) {
	const std::string pat = "\"" + key + "\"";
	const size_t k = s.find(pat, pos);
	if (k == std::string::npos) return std::string::npos;
	size_t c = s.find(':', k + pat.size());
	if (c == std::string::npos) return std::string::npos;
	++c;
	while (c < s.size() && (s[c] == ' ' || s[c] == '\t' || s[c] == '\n' || s[c] == '\r')) ++c;
	if (key_pos_out) *key_pos_out = k;
	return c;
}

bool ParseString(const std::string& s, size_t pos, std::string* out) {
	if (pos >= s.size() || s[pos] != '"') return false;
	++pos;
	out->clear();
	while (pos < s.size() && s[pos] != '"') {
		if (s[pos] == '\\' && pos + 1 < s.size()) {
			++pos;
			switch (s[pos]) {
				case 'n': *out += '\n'; break;
				case 'r': *out += '\r'; break;
				case 't': *out += '\t'; break;
				default:  *out += s[pos]; break;
			}
		} else {
			*out += s[pos];
		}
		++pos;
	}
	return true;
}

} // namespace

bool ReadJson(const std::string& path, std::vector<Result>* results) {
	std::FILE* fp = std::fopen(path.c_str(), "rb");
	if (!fp) return false;
	std::string s;
	char buf[65536];
	for (;;) {
		const size_t n = std::fread(buf, 1, sizeof(buf), fp);
		if (!n) break;
		s.append(buf, n);
	}
	std::fclose(fp);

	results->clear();
	size_t pos = 0;
	for (;;) {
		size_t pattern_key = 0;
		const size_t pp = FindValue(s, "pattern", pos, &pattern_key);
		if (pp == std::string::npos) break;

		Result r;
		if (!ParseString(s, pp, &r.pattern)) break;
		const size_t vp = FindValue(s, "variant", pp);
		if (vp != std::string::npos) ParseString(s, vp, &r.variant);

		// metrics 陣列的範圍：從這個 result 的 "metrics" 到下一個 "pattern"
		const size_t mstart = FindValue(s, "metrics", pp);
		size_t next_key = std::string::npos;
		FindValue(s, "pattern", pp, &next_key);
		const size_t mend = (next_key == std::string::npos) ? s.size() : next_key;

		size_t mp = mstart;
		while (mp != std::string::npos && mp < mend) {
			size_t name_key = 0;
			const size_t np = FindValue(s, "name", mp, &name_key);
			if (np == std::string::npos || name_key >= mend) break;

			Metric m;
			if (!ParseString(s, np, &m.name)) break;

			const size_t valp = FindValue(s, "value", np);
			if (valp != std::string::npos && valp < mend) {
				if (s.compare(valp, 4, "null") == 0) {
					m.valid = false;
				} else {
					m.valid = true;
					m.value = std::strtod(s.c_str() + valp, nullptr);
				}
			}
			const size_t up = FindValue(s, "unit", np);
			if (up != std::string::npos && up < mend) ParseString(s, up, &m.unit);

			const size_t hp = FindValue(s, "higher_is_better", np);
			if (hp != std::string::npos && hp < mend)
				m.higher_is_better = (s.compare(hp, 4, "true") == 0);

			const size_t gp = FindValue(s, "gated", np);
			if (gp != std::string::npos && gp < mend)
				m.gated = (s.compare(gp, 4, "true") == 0);

			r.metrics.push_back(m);
			mp = np;              // 從值之後接著找下一個 metric
		}
		results->push_back(r);
		if (next_key == std::string::npos) break;
		pos = next_key;          // 用鍵的位置推進，才不會跳過下一筆 result
	}
	return !results->empty();
}

// ---------------------------------------------------------------- baseline 比對

int CompareToBaseline(const std::vector<Result>& current,
                      const std::vector<Result>& baseline,
                      double tolerance_pct) {
	std::map<std::string, Metric> base;
	for (const Result& r : baseline)
		for (const Metric& m : r.metrics)
			base[KeyOf(r, m)] = m;

	std::printf("\n與 baseline 比對（容差 %.1f%%）\n", tolerance_pct);
	std::printf("%-44s %12s %12s %10s\n", "指標", "baseline", "目前", "變化");
	std::printf("--------------------------------------------------------------------------------\n");

	int regressions = 0, compared = 0, missing = 0;
	std::vector<std::string> regressed;

	for (const Result& r : current) {
		for (const Metric& m : r.metrics) {
			const std::string key = KeyOf(r, m);
			const auto it = base.find(key);
			if (it == base.end()) { ++missing; continue; }
			if (!m.valid || !it->second.valid) continue;

			const double b = it->second.value;
			const double c = m.value;
			++compared;

			double change_pct = 0.0;
			if (b != 0.0) change_pct = 100.0 * (c - b) / std::fabs(b);

			// higher_is_better 時，變化為負才是退步；反之亦然
			const double regress_pct = m.higher_is_better ? -change_pct : change_pct;
			// 兩邊都要同意才納入門檻：舊 baseline 沒有 gated 欄位時預設 true，
			// 新指標標了 info 就不會突然把既有的 CI 弄紅。
			const bool gated = m.gated && it->second.gated;
			const bool bad = gated && regress_pct > tolerance_pct;
			if (bad) { ++regressions; regressed.push_back(key); }

			std::printf("%-44s %12.4g %12.4g %+9.1f%% %s\n",
				key.c_str(), b, c, change_pct,
				bad ? " <-- 退步" : (gated ? "" : " (參考)"));
		}
	}

	std::printf("--------------------------------------------------------------------------------\n");
	std::printf("比對 %d 個指標", compared);
	if (missing) std::printf("，baseline 沒有的新指標 %d 個", missing);
	std::printf("\n");

	if (regressions) {
		std::printf("\n超出容差的退步：\n");
		for (const std::string& k : regressed) std::printf("  - %s\n", k.c_str());
		return 1;
	}
	std::printf("沒有超出容差的退步。\n");
	return 0;
}

} // namespace vfsbench
