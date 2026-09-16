// vfs_bench.h — benchmark 框架
//
// 設計重點是「能拿來做 regression」，不是「印個漂亮數字」：
//   - 每個 pattern 產出具名指標（metric），而不是一坨自由格式的文字
//   - 結果可存成 JSON，之後用 --compare 跟 baseline 比，超出容差就非零退出
//   - 語料由固定種子產生，跨平台、跨次數都一樣，數字才可比
//   - 每個指標都標明「越大越好」還是「越小越好」，比對時才知道哪邊是退步
//
// 可攜性：只用標準 C++17 與 vfs_platform.h。JSON 讀寫是自己寫的小實作，
// 只認得本框架自己產出的格式 —— 不引進任何第三方相依。
#ifndef VFS_BENCH_H
#define VFS_BENCH_H

#include <string>
#include <vector>

namespace vfsbench {

// ---------------------------------------------------------------- 指標

struct Metric {
	std::string name;             // 例如 "throughput_mb_s"
	double      value = 0.0;
	std::string unit;             // 例如 "MB/s"、"ms"、"%"
	bool        higher_is_better = true;
	bool        valid = true;     // false = 這個平台量不到，輸出成 null
	// false = 只印出來參考，不納入 --compare 的退步判定。
	// 給 lat_max_ms 這種單一樣本統計用 —— 它本來就會抖，拿來當門檻只會製造假警報。
	bool        gated = true;
};

struct Result {
	std::string         pattern;  // 例如 "pack"
	std::string         variant;  // 例如 "small_files"；可空
	std::vector<Metric> metrics;

	void Add(const std::string& name, double value, const std::string& unit,
	         bool higher_is_better);
	// 同 Add，但不納入退步判定（噪音大或純粹參考用的指標）
	void AddInfo(const std::string& name, double value, const std::string& unit,
	             bool higher_is_better);
	void AddInvalid(const std::string& name, const std::string& unit);
	std::string Label() const;    // "pack/small_files" 或 "pack"
};

// ---------------------------------------------------------------- 取樣
//
// 延遲類指標要看分佈而不是平均：隨機讀的痛點正是尾端延遲。

class Samples {
public:
	void Add(double v) { v_.push_back(v); }
	void Reserve(size_t n) { v_.reserve(n); }
	size_t Count() const { return v_.size(); }
	bool Empty() const { return v_.empty(); }

	double Min();
	double Max();
	double Mean() const;
	double Percentile(double pct);          // pct 為 0~100
	double Sum() const;

private:
	void EnsureSorted();
	std::vector<double> v_;
	bool sorted_ = false;
};

// ---------------------------------------------------------------- 計時

class Timer {
public:
	Timer();
	void   Reset();
	double Elapsed() const;                  // 秒
private:
	double start_ = 0.0;
};

// ---------------------------------------------------------------- pattern 註冊
//
// 跟測試同樣的自我註冊做法：新增一個 pattern 不需要改 dispatcher。

struct Options {
	// 語料規模。預設值刻意壓小，讓 `vfs bench` 幾秒內跑完；
	// 要跑接近正式封包的規模用 --scale（例如 --scale 20 約 1GB）。
	double scale = 1.0;
	unsigned long long seed = 0x5EED1234;
	int repeats = 1;                         // 重複次數，取最佳值以降噪
	// 每一輪開始前要擠掉多少 MB 的 OS 檔案快取。0 = 不做。
	// 見 EvictOsCache 的說明：標準 C++ 沒有丟棄檔案快取的 API，只能用壓力法。
	int cold_mb = 0;
	bool verbose = false;
};

// ---------------------------------------------------------------- 快取控制
//
// 量測前要清的快取有兩層，性質完全不同：
//
// 1. VFS 自己的快取（IIO 的 LRU 頁、.pak 的 64KB 滑動視窗）
//    關檔就會全部丟掉，是決定性的、可攜的。每個 pattern 在開始量之前都必須
//    先 Reopen 一次，否則量到的是「剛寫完還熱在手上」的狀態。
//    vfs_iio_CACHE_PAGES 提高到 64 之後，這一層的影響比以前大得多。
//
// 2. OS 的檔案快取
//    標準 C++ 沒有「丟棄某個檔案的快取」這種 API（Windows 要 FILE_FLAG_NO_BUFFERING、
//    Linux 要 posix_fadvise，都不可攜）。唯一能做的是配置壓力：寫一個夠大的
//    ballast 檔再讀回來，把想量的封包擠出快取。盡力而為，不保證徹底。

// 寫一個 bytes 大小的暫存檔再整個讀回來，藉此把先前的檔案內容擠出 OS 快取。
// 完成後會刪掉該檔。bytes 為 0 時直接返回。
void EvictOsCache(size_t bytes);

using PatternFn = void (*)(const Options&, std::vector<Result>*);

struct Pattern {
	const char* name;
	const char* description;
	PatternFn   fn;
};

void RegisterPattern(const Pattern& p);
struct PatternRegistrar { explicit PatternRegistrar(const Pattern& p) { RegisterPattern(p); } };

#define VFS_BENCH(name_, desc_)                                                    \
	static void vfsbench_fn_##name_(const ::vfsbench::Options&,                    \
	                                std::vector<::vfsbench::Result>*);             \
	static ::vfsbench::PatternRegistrar vfsbench_reg_##name_(                      \
		::vfsbench::Pattern{ #name_, desc_, &vfsbench_fn_##name_ });               \
	static void vfsbench_fn_##name_(const ::vfsbench::Options& opt,                \
	                                std::vector<::vfsbench::Result>* out)

// ---------------------------------------------------------------- 執行

void ListPatterns();

// filter 為空代表全跑。回傳跑出來的結果。
std::vector<Result> RunPatterns(const std::vector<std::string>& filter, const Options& opt);

void PrintResults(const std::vector<Result>& results);

// ---------------------------------------------------------------- JSON

bool WriteJson(const std::string& path, const std::vector<Result>& results,
               const Options& opt);
bool ReadJson(const std::string& path, std::vector<Result>* results);

// 與 baseline 比對。tolerance_pct 是允許的退步幅度（例如 10 表示退步 10% 以內算過）。
// 回傳 0 = 沒有超出容差的退步。會把比對表印出來。
int CompareToBaseline(const std::vector<Result>& current,
                      const std::vector<Result>& baseline,
                      double tolerance_pct);

} // namespace vfsbench

#endif // VFS_BENCH_H
