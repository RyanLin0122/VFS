// test_main.cpp — `vfs test` 與 `vfs bench` 兩個子命令的入口
//
// 放在獨立 TU 而不是 main.cpp，是為了讓 main.cpp 維持「純 CLI 工具」的定位；
// 測試與 benchmark 的相依（vfs_test / vfs_bench / vfs_platform）不會滲進去。

#include <vfs_test.h>
#include <vfs.h>
#include <vfs_bench.h>
#include <vfs_platform.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool IsFlag(const char* arg, const char* name) {
	return std::strcmp(arg, name) == 0;
}

} // namespace

// ---------------------------------------------------------------- vfs test

int VfsTestMain(int argc, char** argv) {
	std::vector<std::string> suites;
	bool verbose = false;

	for (int i = 0; i < argc; ++i) {
		const char* a = argv[i];
		if (IsFlag(a, "-v") || IsFlag(a, "--verbose")) { verbose = true; continue; }
		if (IsFlag(a, "-l") || IsFlag(a, "--list")) { vfstest::ListSuites(); return 0; }
		if (IsFlag(a, "-h") || IsFlag(a, "--help")) {
			std::printf(
				"用法：vfs test [suite...] [選項]\n"
				"\n"
				"  不指定 suite 就全部跑。可用的 suite 用 --list 看。\n"
				"\n"
				"選項：\n"
				"  -l, --list     列出所有 suite\n"
				"  -v, --verbose  連已知失敗（xfail）的細節也印出來\n"
				"\n"
				"結果標記：\n"
				"  [ ok  ]  通過\n"
				"  [FAIL ]  失敗 —— 退出碼非 0\n"
				"  [xfail]  已知缺陷，如預期失敗 —— 不影響退出碼\n"
				"  [XPASS]  已知缺陷卻通過了 —— 退出碼非 0，該把 XFAIL 標記拿掉了\n"
				"\n"
				"暫存封包放在 %s（可用環境變數 VFS_TEST_TMPDIR 指定）\n",
				vfsplat::ScratchDir().c_str());
			return 0;
		}
		if (a[0] == '-') {
			std::fprintf(stderr, "未知選項：%s\n", a);
			return 2;
		}
		suites.push_back(a);
	}

	std::printf("暫存目錄：%s\n", vfsplat::ScratchDir().c_str());
	return vfstest::RunAll(suites, verbose);
}

// ---------------------------------------------------------------- vfs bench

int VfsBenchMain(int argc, char** argv) {
	std::vector<std::string> patterns;
	vfsbench::Options opt;
	std::string json_out;
	std::string compare_with;
	// 預設放寬到 25%：吞吐量類指標本來就有 10~20% 的機器雜訊，門檻太緊只會製造
	// 假警報。真正該盯緊的是計數與比例類指標（space_amplification / *_changed_pct /
	// files_failed），它們是決定性的，一動就是真的有東西變了。
	double tolerance = 25.0;

	for (int i = 0; i < argc; ++i) {
		const char* a = argv[i];
		const bool has_next = (i + 1 < argc);

		if (IsFlag(a, "-l") || IsFlag(a, "--list")) { vfsbench::ListPatterns(); return 0; }
		if (IsFlag(a, "-v") || IsFlag(a, "--verbose")) { opt.verbose = true; continue; }
		if (IsFlag(a, "--scale") && has_next)    { opt.scale = std::atof(argv[++i]); continue; }
		if (IsFlag(a, "--seed") && has_next)     { opt.seed = std::strtoull(argv[++i], nullptr, 0); continue; }
		if (IsFlag(a, "--repeats") && has_next)  { opt.repeats = std::atoi(argv[++i]); continue; }
		if (IsFlag(a, "--cold") && has_next)     { opt.cold_mb = std::atoi(argv[++i]); continue; }
		if (IsFlag(a, "--json") && has_next)     { json_out = argv[++i]; continue; }
		if (IsFlag(a, "--compare") && has_next)  { compare_with = argv[++i]; continue; }
		if (IsFlag(a, "--tolerance") && has_next){ tolerance = std::atof(argv[++i]); continue; }
		if (IsFlag(a, "-h") || IsFlag(a, "--help")) {
			std::printf(
				"用法：vfs bench [pattern...] [選項]\n"
				"\n"
				"  不指定 pattern 就全部跑。可用的 pattern 用 --list 看。\n"
				"\n"
				"選項：\n"
				"  --scale <f>      語料規模倍率（預設 1.0 約 60MB／900 檔）\n"
				"                   --scale 20 約 1.2GB，接近正式封包\n"
				"  --seed <n>       語料亂數種子（預設 0x5EED1234）\n"
				"  --repeats <n>    每個 pattern 重複 n 次取最佳值，降噪用（建議 3）\n"
				"  --cold <MB>      每一輪開始前寫讀一個 MB 大小的 ballast 檔，\n"
				"                   把封包擠出 OS 的檔案快取（預設 0 = 不做）\n"
				"                   要擠乾淨得給到實體記憶體的 1~2 倍，會慢很多；\n"
				"                   只在要量磁碟受限的行為時才用\n"
				"  --json <path>    把結果寫成 JSON\n"
				"  --compare <path> 跟 baseline JSON 比對，有退步就回非 0\n"
				"  --tolerance <p>  比對容差百分比（預設 25）\n"
				"\n"
				"比對前兩邊的 --repeats / --scale / --cold 要一致，否則比到的是雜訊。\n"
				"吞吐量類指標有 10~20%% 的機器雜訊，--repeats 3 取最佳值會穩很多；\n"
				"space_amplification、*_changed_pct、files_failed 這類比例與計數是\n"
				"決定性的，一有變化就是真的有東西改了。\n"
				"\n"
				"建立 baseline：\n"
				"  vfs bench --repeats 3 --json baseline.json\n"
				"重構後檢查有沒有退步：\n"
				"  vfs bench --repeats 3 --compare baseline.json\n"
				"\n"
				"暫存封包放在 %s（可用環境變數 VFS_TEST_TMPDIR 指定）\n",
				vfsplat::ScratchDir().c_str());
			return 0;
		}
		if (a[0] == '-') {
			std::fprintf(stderr, "未知選項：%s\n", a);
			return 2;
		}
		patterns.push_back(a);
	}

	// 診斷用旋鈕：用環境變數蓋掉快取設定，不必重新編譯就能做 A/B
	if (const char* e = std::getenv("VFS_DATA_CACHE")) { const int v = std::atoi(e); if (v > 0) vfs_data_CACHE_BYTES = v; }
	if (const char* e = std::getenv("VFS_IIO_PAGES"))  { const int v = std::atoi(e); if (v > 0) vfs_iio_CACHE_PAGES = v; }

	if (opt.scale <= 0.0) opt.scale = 1.0;
	if (opt.repeats < 1) opt.repeats = 1;

	std::printf("暫存目錄：%s\n", vfsplat::ScratchDir().c_str());
	std::printf("規模 %.2gx，種子 0x%llX，重複 %d 次\n",
		opt.scale, opt.seed, opt.repeats);
	if (opt.scale < 4.0) {
		// 說清楚這一輪量的是什麼，免得有人拿 2500 MB/s 去推估玩家的實際載入時間
		std::printf(
			"注意：這個規模下整個封包塞得進 OS 的檔案快取，量到的是 VFS 本身的\n"
			"      CPU 成本（FAT 鏈走訪、memcpy、查表），不是磁碟 I/O。這正是做\n"
			"      regression 想要的 —— 穩定、可重現。要量磁碟受限的行為請用\n"
			"      --scale 20 以上（約 1GB 以上，超過快取才會真的打到硬碟）。\n");
	}
	std::printf("\n");

	const std::vector<vfsbench::Result> results = vfsbench::RunPatterns(patterns, opt);
	vfsbench::PrintResults(results);

	if (!json_out.empty()) {
		if (vfsbench::WriteJson(json_out, results, opt))
			std::printf("\n結果已寫入 %s\n", json_out.c_str());
		else
			std::fprintf(stderr, "\n寫入 %s 失敗\n", json_out.c_str());
	}

	if (!compare_with.empty()) {
		std::vector<vfsbench::Result> baseline;
		if (!vfsbench::ReadJson(compare_with, &baseline)) {
			std::fprintf(stderr, "\n讀不到 baseline：%s\n", compare_with.c_str());
			return 2;
		}
		return vfsbench::CompareToBaseline(results, baseline, tolerance);
	}
	return 0;
}
