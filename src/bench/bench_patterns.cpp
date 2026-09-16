// bench_patterns.cpp — 六個測速 pattern
//
//   pack      打包速度（整體 / 分大小級距 / 大量小檔）
//   seqread   循序讀（整檔讀出，模擬載入關卡）
//   randread  隨機讀（模擬遊戲執行期零星取資源）—— 含「碎片化」對照組
//   write     寫入（新增 / 原地覆寫 / 增長），含空間放大
//   open      開啟封包與查名字的成本（跟檔案數的關係）
//   patch     增量更新造成的 .pak/.paki 變動比例 —— 直接對應 CDN 流量
//
// 每個 pattern 都用同一套合成語料，並把「越大越好 / 越小越好」標進指標，
// 讓 --compare 知道哪個方向算退步。

#include <vfs_bench.h>
#include <vfs_testutil.h>
#include <vfs_platform.h>

#include "bench_corpus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using vfsbench::Corpus;
using vfsbench::CorpusItem;
using vfsbench::Options;
using vfsbench::Result;
using vfsbench::Samples;
using vfsbench::SizeClassName;
using vfsbench::Timer;
using vfsutil::TempArchive;

const double kMB = 1024.0 * 1024.0;

// 把語料寫進封包；回傳成功的檔案數，bytes_out 給實際寫入的位元組數
int FillArchive(VfsHandle* h, const Corpus& c, long long* bytes_out) {
	int ok = 0;
	long long bytes = 0;
	for (const CorpusItem& it : c.items) {
		const std::vector<char> data = it.Bytes();
		if (vfsutil::WriteAll(h, it.key, data)) {
			++ok;
			bytes += static_cast<long long>(data.size());
		}
	}
	if (bytes_out) *bytes_out = bytes;
	return ok;
}

// 把封包打散：刪掉一半的檔案再用不同大小寫回去。
// 之後配置到的區塊會落進前面留下的洞裡，FAT 鏈不再連續 ——
// 這才是玩家玩了幾個月、更新過幾輪之後的真實狀態。
//
// 每個 fragmented 變體都會回報 files_available —— 這個過程做了大量的
// unlink + 重寫，正是最容易把資料弄丟的操作。缺陷 4 修好之前，這裡會掉一半的
// 檔案（472 / 944），而吞吐量的數字看起來反而更漂亮。把完整性跟速度一起量，
// 才不會拿一個壞掉流程的數字當基準。
void Fragment(VfsHandle* h, const Corpus& c, unsigned long long seed) {
	vfsutil::Rng rng(seed ^ 0xF4A9ULL);
	for (size_t i = 0; i < c.items.size(); i += 2) {
		vfs_file_unlink(h, c.items[i].key.c_str());
	}
	for (size_t i = 0; i < c.items.size(); i += 2) {
		const CorpusItem& it = c.items[i];
		// 大小改變才會真的打散配置；避開 % 512 == 1 的已知 bug
		size_t sz = static_cast<size_t>(
			static_cast<double>(it.size) * (0.6 + rng.Unit() * 0.8));
		if (sz < 64) sz = 64;
		if (sz % 512 == 1) ++sz;
		vfsutil::WriteAll(h, it.key, vfsutil::Blob(sz, it.seed ^ 0x9999u));
	}
}

} // namespace

// ================================================================ pack

VFS_BENCH(pack, "打包速度：整體、分大小級距、以及大量小檔") {
	const Corpus c = vfsbench::MakeCorpus(opt.scale, opt.seed);

	// --- 整體 ---
	{
		TempArchive a("bench_pack");
		if (!a.Ok()) return;

		long long bytes = 0;
		const Timer t;
		const int ok = FillArchive(a.h(), c, &bytes);
		a.Close();                              // 關檔才算完整刷出，計時要含這段
		const double sec = t.Elapsed();

		Result r;
		r.pattern = "pack";
		r.variant = "all";
		r.Add("throughput_mb_s", (static_cast<double>(bytes) / kMB) / sec, "MB/s", true);
		r.Add("files_per_sec", static_cast<double>(ok) / sec, "files/s", true);
		r.AddInfo("elapsed_s", sec, "s", false);              // 跟 throughput 同源，避免重複計一次
		r.Add("files_written", static_cast<double>(ok), "files", true);
		r.Add("files_failed", static_cast<double>(c.items.size() - ok), "files", false);
		// 空間放大：實體佔用 ÷ 邏輯內容。512B 區塊的內部碎片與索引開銷都在裡面。
		const long long on_disk = a.PakBytes() + a.PakiBytes();
		r.Add("space_amplification", static_cast<double>(on_disk) / static_cast<double>(bytes),
		      "x", false);
		r.Add("pak_mb", static_cast<double>(a.PakBytes()) / kMB, "MB", false);
		r.Add("paki_mb", static_cast<double>(a.PakiBytes()) / kMB, "MB", false);
		out->push_back(r);
	}

	// --- 分大小級距 ---
	for (int cls = 0; cls < vfsbench::kSizeClassCount; ++cls) {
		Corpus sub;
		for (const CorpusItem& it : c.items)
			if (it.size_class == cls) { sub.items.push_back(it); sub.total_bytes += static_cast<long long>(it.size); }
		if (sub.items.empty()) continue;

		TempArchive a("bench_pack_cls");
		if (!a.Ok()) continue;

		long long bytes = 0;
		const Timer t;
		const int ok = FillArchive(a.h(), sub, &bytes);
		a.Close();
		const double sec = t.Elapsed();

		Result r;
		r.pattern = "pack";
		r.variant = SizeClassName(cls);
		r.Add("throughput_mb_s", (static_cast<double>(bytes) / kMB) / sec, "MB/s", true);
		r.Add("files_per_sec", static_cast<double>(ok) / sec, "files/s", true);
		r.AddInfo("mean_file_kb", (static_cast<double>(bytes) / 1024.0) / static_cast<double>(ok), "KB", true);
		out->push_back(r);
	}

	// --- 大量小檔（索引成本主導，跟資料量無關）---
	{
		const Corpus small = vfsbench::MakeSmallFileCorpus(opt.scale, opt.seed);
		TempArchive a("bench_pack_many");
		if (!a.Ok()) return;

		long long bytes = 0;
		const Timer t;
		const int ok = FillArchive(a.h(), small, &bytes);
		a.Close();
		const double sec = t.Elapsed();

		Result r;
		r.pattern = "pack";
		r.variant = "many_small";
		r.Add("files_per_sec", static_cast<double>(ok) / sec, "files/s", true);
		r.Add("throughput_mb_s", (static_cast<double>(bytes) / kMB) / sec, "MB/s", true);
		r.Add("us_per_file", sec * 1e6 / static_cast<double>(ok), "us", false);
		r.Add("index_bytes_per_file",
		      static_cast<double>(a.PakiBytes()) / static_cast<double>(ok), "B", false);
		out->push_back(r);
	}
}

// ================================================================ seqread

VFS_BENCH(seqread, "循序讀：整檔讀出，模擬載入關卡") {
	const Corpus c = vfsbench::MakeCorpus(opt.scale, opt.seed);

	TempArchive a("bench_seqread");
	if (!a.Ok()) return;
	long long bytes = 0;
	FillArchive(a.h(), c, &bytes);
	// Reopen 清掉 VFS 自己的快取（IIO 的 LRU 頁、.pak 的滑動視窗）。
	// 這一步是決定性的，一定要做 —— 否則量到的是剛寫完還熱在手上的狀態。
	if (!a.Reopen()) return;
	// 再把 OS 的檔案快取也擠掉（只有指定 --cold 時才會真的動作）。
	// 要放在「填完語料之後」，因為封包就是剛剛才寫出去的，
	// 在輪次開頭清是沒用的。
	vfsbench::EvictOsCache(static_cast<size_t>(opt.cold_mb) << 20);

	// --- 連續封包（剛打包完的狀態）---
	{
		std::vector<char> buf;
		Samples per_file_mb_s;
		long long read_bytes = 0;
		int failed = 0;

		const Timer t;
		for (const CorpusItem& it : c.items) {
			const Timer ft;
			if (!vfsutil::ReadAll(a.h(), it.key, &buf)) { ++failed; continue; }
			const double fsec = ft.Elapsed();
			read_bytes += static_cast<long long>(buf.size());
			if (fsec > 0.0)
				per_file_mb_s.Add((static_cast<double>(buf.size()) / kMB) / fsec);
		}
		const double sec = t.Elapsed();

		Result r;
		r.pattern = "seqread";
		r.variant = "contiguous";
		r.Add("throughput_mb_s", (static_cast<double>(read_bytes) / kMB) / sec, "MB/s", true);
		r.Add("files_per_sec", static_cast<double>(c.items.size()) / sec, "files/s", true);
		r.Add("read_failures", static_cast<double>(failed), "files", false);
		r.Add("per_file_mb_s_p50", per_file_mb_s.Percentile(50), "MB/s", true);
		r.AddInfo("per_file_mb_s_p05", per_file_mb_s.Percentile(5), "MB/s", true);
		out->push_back(r);
	}

	// --- 碎片化封包（更新過幾輪之後的真實狀態）---
	{
		Fragment(a.h(), c, opt.seed);
		if (!a.Reopen()) return;
		vfsbench::EvictOsCache(static_cast<size_t>(opt.cold_mb) << 20);

		std::vector<char> buf;
		long long read_bytes = 0;
		const std::vector<std::string> keys = vfsutil::ListKeys(a.h());

		const Timer t;
		for (const std::string& k : keys) {
			if (vfsutil::ReadAll(a.h(), k, &buf)) read_bytes += static_cast<long long>(buf.size());
		}
		const double sec = t.Elapsed();

		Result r;
		r.pattern = "seqread";
		r.variant = "fragmented";
		r.Add("throughput_mb_s", (static_cast<double>(read_bytes) / kMB) / sec, "MB/s", true);
		r.Add("files_per_sec", static_cast<double>(keys.size()) / sec, "files/s", true);
		r.Add("files_available", static_cast<double>(keys.size()), "files", true);
		r.Add("files_expected", static_cast<double>(c.items.size()), "files", true);
		out->push_back(r);
	}
}

// ================================================================ randread

VFS_BENCH(randread, "隨機讀：遊戲執行期零星取資源，看尾端延遲") {
	const Corpus c = vfsbench::MakeCorpus(opt.scale, opt.seed);

	// 隨機讀最能打爆 .pak 的 64KB 單一滑動視窗與 IIO 的 8 頁 LRU，
	// 所以連續／碎片化兩種狀態都要量，兩者的差距就是碎片化的代價。
	for (int frag = 0; frag < 2; ++frag) {
		TempArchive a("bench_randread");
		if (!a.Ok()) return;
		long long bytes = 0;
		FillArchive(a.h(), c, &bytes);
		if (frag) Fragment(a.h(), c, opt.seed);
		if (!a.Reopen()) return;                 // 清掉 VFS 自己的快取
		vfsbench::EvictOsCache(static_cast<size_t>(opt.cold_mb) << 20);   // 再擠掉 OS 的

		const std::vector<std::string> keys = vfsutil::ListKeys(a.h());
		if (keys.empty()) return;

		// 每次讀一小段（4KB），位置隨機 —— 模擬「要一張圖的某個 mip」這種存取
		const int kOps = static_cast<int>(3000 * (opt.scale > 1.0 ? 1.0 : opt.scale)) + 500;
		const int kChunk = 4096;

		vfsutil::Rng rng(opt.seed ^ 0xBEEF1234ULL ^ static_cast<unsigned>(frag));
		std::vector<char> buf(static_cast<size_t>(kChunk));
		Samples lat_ms;
		lat_ms.Reserve(static_cast<size_t>(kOps));
		long long got_bytes = 0;
		int failures = 0;

		const Timer total;
		for (int i = 0; i < kOps; ++i) {
			const std::string& key = keys[static_cast<size_t>(rng.Range(0, static_cast<long long>(keys.size()) - 1))];
			const int size = vfsutil::FileSize(a.h(), key);
			if (size <= 0) { ++failures; continue; }
			const int want = (size < kChunk) ? size : kChunk;
			const int off = (size > want) ? static_cast<int>(rng.Range(0, size - want)) : 0;

			const Timer op;
			const int fd = vfs_file_open(a.h(), key.c_str(), 2);
			if (fd < 0) { ++failures; continue; }
			vfs_file_lseek(a.h(), fd, off, 0);
			const int got = vfs_file_read(a.h(), fd, buf.data(), want);
			vfs_file_close(a.h(), fd);
			lat_ms.Add(op.Elapsed() * 1000.0);

			if (got != want) ++failures;
			else got_bytes += got;
		}
		const double sec = total.Elapsed();

		Result r;
		r.pattern = "randread";
		r.variant = frag ? "fragmented" : "contiguous";
		r.Add("iops", static_cast<double>(kOps) / sec, "ops/s", true);
		r.Add("throughput_mb_s", (static_cast<double>(got_bytes) / kMB) / sec, "MB/s", true);
		r.Add("lat_p50_ms", lat_ms.Percentile(50), "ms", false);
		r.Add("lat_p95_ms", lat_ms.Percentile(95), "ms", false);
		r.Add("lat_p99_ms", lat_ms.Percentile(99), "ms", false);
		r.AddInfo("lat_max_ms", lat_ms.Max(), "ms", false);   // 單一樣本，只供參考
		r.Add("failures", static_cast<double>(failures), "ops", false);
		r.Add("files_available", static_cast<double>(keys.size()), "files", true);
		out->push_back(r);
	}
}

// ================================================================ write

VFS_BENCH(write, "寫入：新增 / 原地覆寫 / 增長，含空間放大") {
	const Corpus c = vfsbench::MakeCorpus(opt.scale, opt.seed);

	TempArchive a("bench_write");
	if (!a.Ok()) return;

	// --- 新增（空封包起步）---
	// 加 -v 會印出底層的操作次數。這一層的效能問題幾乎都是「某個迴圈做了比預期
	// 多三個數量級的次數」，看次數比看時間準得多 —— FAT 配置器的瓶頸就是這樣抓到的。
	auto dump_counters = [&opt](const char* what, long long bytes) {
		if (!opt.verbose) return;
		std::printf("\n  [診斷] %s（邏輯 %.1f MB）\n", what, bytes / (1024.0 * 1024.0));
		std::printf("    FAT 讀 %-12lld FAT 寫 %-12lld next_free 掃描步數 %lld\n",
			vfs_stat_fat_read, vfs_stat_fat_write, vfs_stat_fat_scan_steps);
		std::printf("    data 讀 %-11lld data 寫 %-11lld 視窗滑動 %lld\n",
			vfs_stat_data_read, vfs_stat_data_write, vfs_stat_data_slide);
	};

	long long logical = 0;
	{
		vfs_stat_reset();
		const Timer t;
		FillArchive(a.h(), c, &logical);
		a.Close();
		const double sec = t.Elapsed();
		const long long on_disk = a.PakBytes() + a.PakiBytes();

		dump_counters("create", logical);

		Result r;
		r.pattern = "write";
		r.variant = "create";
		r.Add("throughput_mb_s", (static_cast<double>(logical) / kMB) / sec, "MB/s", true);
		r.Add("space_amplification",
		      static_cast<double>(on_disk) / static_cast<double>(logical), "x", false);
		out->push_back(r);
	}

	if (!a.Reopen()) return;
	const long long disk_before = a.PakBytes() + a.PakiBytes();

	// --- 原地覆寫（大小不變）---
	{
		vfs_stat_reset();
		long long written = 0;
		const Timer t;
		for (size_t i = 0; i < c.items.size(); i += 3) {
			const CorpusItem& it = c.items[i];
			if (vfsutil::WriteAll(a.h(), it.key, vfsutil::Blob(it.size, it.seed ^ 0x55u)))
				written += static_cast<long long>(it.size);
		}
		a.Close();
		const double sec = t.Elapsed();
		const long long disk_after = a.PakBytes() + a.PakiBytes();

		dump_counters("overwrite", written);

		Result r;
		r.pattern = "write";
		r.variant = "overwrite_same_size";
		r.Add("throughput_mb_s", (static_cast<double>(written) / kMB) / sec, "MB/s", true);
		// 同大小覆寫理應不佔新空間；這個值要接近 0
		r.Add("disk_growth_ratio",
		      static_cast<double>(disk_after - disk_before) / static_cast<double>(written),
		      "x", false);
		out->push_back(r);
		if (!a.Reopen()) return;
	}

	// --- 增長（每個檔變大 1.5 倍）---
	//
	// 用自己的封包從頭來過，不沿用前面 create / overwrite 兩階段的狀態。
	// 那兩階段已經往磁碟丟了兩百多 MB，OS 的寫回佇列還積著，接著量到的數字
	// 會被那個積壓主導 —— 實測同一份程式碼會在 302 / 415 / 476 MB/s 之間跳，
	// 門檻根本沒法用。每個變體各自獨立量，數字才對得起「變體」這個名字。
	{
		a.Close();
		TempArchive g("bench_write_grow");
		if (!g.Ok()) return;
		long long fill_bytes = 0;
		FillArchive(g.h(), c, &fill_bytes);
		g.Close();
		if (!g.Reopen()) return;

		const long long before = g.PakBytes() + g.PakiBytes();
		vfs_stat_reset();
		long long written = 0;
		const Timer t;
		for (size_t i = 0; i < c.items.size(); i += 5) {
			const CorpusItem& it = c.items[i];
			size_t sz = it.size + it.size / 2;
			if (sz % 512 == 1) ++sz;
			if (vfsutil::WriteAll(g.h(), it.key, vfsutil::Blob(sz, it.seed ^ 0xAAu)))
				written += static_cast<long long>(sz);
		}
		g.Close();
		const double sec = t.Elapsed();
		const long long after = g.PakBytes() + g.PakiBytes();
		dump_counters("grow", written);

		// 覆寫／增長之後，整個封包的檔案必須一個不少、內容一個不錯。
		//
		// 這個檢查是有來歷的：缺陷 4（刪除會連帶毀掉別的鍵）修好之前，
		// WriteAll 的 unlink 會讓其他鍵消失，於是後續的「覆寫」變成「建立新檔」
		// —— 循序追加，數字很漂亮，但資料在掉（原版實測只剩 190 / 944）。
		// 把完整性一起量出來，就不會再有人拿一個壞掉流程的吞吐量當基準。
		if (!g.Reopen()) return;
		int intact = 0;
		std::vector<char> back;
		for (const CorpusItem& it : c.items) {
			if (vfsutil::ReadAll(g.h(), it.key, &back)) ++intact;
		}

		Result r;
		r.pattern = "write";
		r.variant = "grow";
		r.Add("throughput_mb_s", (static_cast<double>(written) / kMB) / sec, "MB/s", true);
		r.Add("disk_growth_ratio",
		      static_cast<double>(after - before) / static_cast<double>(written), "x", false);
		r.Add("files_intact", static_cast<double>(intact), "files", true);
		r.Add("files_expected", static_cast<double>(c.items.size()), "files", true);
		out->push_back(r);
	}
}

// ================================================================ open

VFS_BENCH(open, "開啟封包與查名字的成本（跟檔案數的關係）") {
	// 遊戲啟動時 vfs_start 是同步的，每毫秒都算在載入時間上。
	// 查名字的成本則是 Patricia trie 的深度，跟檔案數呈對數關係。
	const int counts[] = { 500, 2000, 8000 };

	for (int n : counts) {
		const int actual = static_cast<int>(n * (opt.scale > 1.0 ? 1.0 : opt.scale));
		if (actual < 10) continue;

		TempArchive a("bench_open");
		if (!a.Ok()) return;

		std::vector<std::string> keys;
		keys.reserve(static_cast<size_t>(actual));
		for (int i = 0; i < actual; ++i) {
			char key[96];
			std::snprintf(key, sizeof(key), "assets/group%02d/item_%06d.dat", i % 32, i);
			keys.push_back(key);
			vfsutil::WriteAll(a.h(), key, vfsutil::Blob(512, static_cast<unsigned>(i)));
		}
		a.Close();

		// vfs_start 的延遲。取樣要夠多 —— 只跑 5 次的話，單次排程抖動就能
		// 讓中位數飄 30%，看起來像真的退步。
		Samples start_ms;
		for (int i = 0; i < 25; ++i) {
			const Timer t;
			if (!a.Reopen()) return;
			start_ms.Add(t.Elapsed() * 1000.0);
			a.Close();
		}
		if (!a.Reopen()) return;

		// 查名字的延遲（命中與未命中分開）。
		//
		// 單次查詢只有 1 微秒上下，直接對每一次呼叫計時的話，steady_clock 的
		// 解析度（Windows 約 100ns）與計時本身的開銷就會蓋過訊號 —— 實測會看到
		// 25% 以上的假性波動。所以改成「一批 100 次計一次時，再除以 100」：
		// 每個樣本仍是每次操作的成本，但量化誤差降到百分之一。
		// 每個樣本做 500 次查詢。批次越大，時鐘量化與單次排程抖動的佔比越小；
		// 這個 pattern 排在幾個寫入密集的 pattern 之後跑，磁碟往往還在刷，
		// 樣本太小就會被那個背景噪音蓋過去。500 × 40 次總共也只要十幾毫秒。
		const int kBatch = 500;
		const int kBatches = 40;
		vfsutil::Rng rng(opt.seed ^ static_cast<unsigned long long>(actual));
		Samples hit_us, miss_us;

		for (int b = 0; b < kBatches; ++b) {
			const Timer t;
			for (int i = 0; i < kBatch; ++i) {
				const std::string& k = keys[static_cast<size_t>(rng.Range(0, actual - 1))];
				vfs_file_exists(a.h(), k.c_str());
			}
			hit_us.Add(t.Elapsed() * 1e6 / kBatch);
		}
		for (int b = 0; b < kBatches; ++b) {
			const Timer t;
			for (int i = 0; i < kBatch; ++i) {
				char key[96];
				std::snprintf(key, sizeof(key), "assets/group%02d/missing_%06d.dat",
					(b * kBatch + i) % 32, b * kBatch + i);
				vfs_file_exists(a.h(), key);
			}
			miss_us.Add(t.Elapsed() * 1e6 / kBatch);
		}

		// glob 掃全表的成本（打包工具與資源列舉會用到）。
		// 量 5 次取最快的一次：單次只有 1 毫秒上下，一次排程抖動就能讓它翻倍。
		Samples glob_samples;
		size_t globbed = 0;
		for (int i = 0; i < 5; ++i) {
			const Timer gt;
			globbed = vfsutil::ListKeys(a.h()).size();
			glob_samples.Add(gt.Elapsed() * 1000.0);
		}
		const double glob_ms = glob_samples.Min();

		char variant[32];
		std::snprintf(variant, sizeof(variant), "%d_files", actual);

		Result r;
		r.pattern = "open";
		r.variant = variant;
		// 這幾個都是次毫秒／微秒等級的量測，取最小值（= 干擾最少的那一次）
		// 才有辦法當回歸門檻；中位數會把 OS 排程與檔案系統的抖動一起帶進來。
		// p50 仍然印出來當參考，只是不納入判定。
		r.Add("start_ms_min", start_ms.Min(), "ms", false);
		r.AddInfo("start_ms_p50", start_ms.Percentile(50), "ms", false);
		r.Add("lookup_hit_us_p50", hit_us.Percentile(50), "us", false);
		r.AddInfo("lookup_hit_us_p99", hit_us.Percentile(99), "us", false);
		r.Add("lookup_miss_us_p50", miss_us.Percentile(50), "us", false);
		r.Add("glob_all_ms", glob_ms, "ms", false);
		r.Add("glob_returned", static_cast<double>(globbed), "files", true);
		out->push_back(r);
	}
}

// ================================================================ patch

VFS_BENCH(patch, "增量更新造成的封包變動比例 —— 直接對應 CDN 流量") {
	// 這個 pattern 量的不是速度，是「玩家要下載多少」。
	// changed_pct 越低，delta patch 越小，CDN 帳單越低。
	const Corpus c = vfsbench::MakeCorpus(opt.scale, opt.seed);

	struct Scenario {
		const char* name;
		const char* desc;
		int  touch_every;      // 每 N 個檔改一個
		bool change_size;      // 是否改變大小
		int  add_new;          // 另外新增幾個檔
	};
	const Scenario kScenarios[] = {
		{ "hotfix_one",      "改一個檔、大小不變",       0, false, 0 },
		{ "patch_1pct",      "改 1% 的檔、大小不變",   100, false, 0 },
		{ "patch_1pct_grow", "改 1% 的檔、大小變動",   100, true,  0 },
		{ "add_10_files",    "新增 10 個檔",             0, false, 10 },
	};

	for (const Scenario& s : kScenarios) {
		TempArchive a("bench_patch");
		if (!a.Ok()) return;

		long long logical = 0;
		FillArchive(a.h(), c, &logical);
		if (!a.Snapshot("v1")) return;

		long long changed_logical = 0;
		int touched = 0;

		if (s.touch_every > 0) {
			for (size_t i = 0; i < c.items.size(); i += static_cast<size_t>(s.touch_every)) {
				const CorpusItem& it = c.items[i];
				size_t sz = it.size;
				if (s.change_size) {
					sz = it.size + it.size / 4 + 100;
					if (sz % 512 == 1) ++sz;
				}
				if (vfsutil::WriteAll(a.h(), it.key, vfsutil::Blob(sz, it.seed ^ 0x77u))) {
					changed_logical += static_cast<long long>(sz);
					++touched;
				}
			}
		} else if (s.add_new > 0) {
			for (int i = 0; i < s.add_new; ++i) {
				char key[96];
				std::snprintf(key, sizeof(key), "patch/new_%03d.dat", i);
				const size_t sz = 64 * 1024;
				if (vfsutil::WriteAll(a.h(), key, vfsutil::Blob(sz, static_cast<unsigned>(9000 + i)))) {
					changed_logical += static_cast<long long>(sz);
					++touched;
				}
			}
		} else if (!c.items.empty()) {
			const CorpusItem& it = c.items[c.items.size() / 2];
			if (vfsutil::WriteAll(a.h(), it.key, vfsutil::Blob(it.size, it.seed ^ 0x77u))) {
				changed_logical += static_cast<long long>(it.size);
				++touched;
			}
		}
		a.Close();

		long long pak_total = 0, pak_grew = 0, paki_total = 0, paki_grew = 0;
		const long long pak_diff  = a.DiffVsSnapshot("v1", false, &pak_total, &pak_grew);
		const long long paki_diff = a.DiffVsSnapshot("v1", true, &paki_total, &paki_grew);
		if (pak_diff < 0 || paki_diff < 0) continue;

		// 玩家實際要下載的量 ≈ 變動的區塊 + 新增的長度
		const long long delta_bytes = (pak_diff + paki_diff) * 4096 + pak_grew + paki_grew;

		Result r;
		r.pattern = "patch";
		r.variant = s.name;
		r.Add("files_touched", static_cast<double>(touched), "files", true);
		r.Add("pak_changed_pct",
		      pak_total ? 100.0 * static_cast<double>(pak_diff) / static_cast<double>(pak_total) : 0.0,
		      "%", false);
		// 這個值在 --scale 1 下量化太粗（.paki 只有約 4472 個 4KB 區塊，差一個區塊
		// 就是 0.022%），拿來當門檻只會製造假警報。真正決定 CDN 帳單的是下面的
		// delta_mb 與 download_amplification，那兩個有納入判定。
		r.AddInfo("paki_changed_pct",
		      paki_total ? 100.0 * static_cast<double>(paki_diff) / static_cast<double>(paki_total) : 0.0,
		      "%", false);
		r.Add("delta_mb", static_cast<double>(delta_bytes) / kMB, "MB", false);
		// 下載放大：玩家下載量 ÷ 實際改動的內容量。理想是接近 1。
		r.Add("download_amplification",
		      changed_logical ? static_cast<double>(delta_bytes) / static_cast<double>(changed_logical) : 0.0,
		      "x", false);
		out->push_back(r);
	}
}
