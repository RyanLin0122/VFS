// bench_corpus.h — benchmark 用的合成語料
//
// 語料完全由固定種子在記憶體中產生，不碰主機檔案系統：
//   - 跨平台、跨次數完全一致，數字才可比
//   - 不受主機磁碟上原始檔案的讀取速度污染，量到的是 VFS 本身
//   - 主機平台（Switch/PS5）上也能跑，不需要事先佈署一坨測試檔
//
// 大小分佈仿真實遊戲資源：大量小檔（腳本/設定）+ 中量貼圖模型 + 少量大檔（地圖/音軌）。
#ifndef VFS_BENCH_CORPUS_H
#define VFS_BENCH_CORPUS_H

#include <string>
#include <vector>

namespace vfsbench {

// 語料中的一個項目。內容不預先產生（3GB 放不進記憶體），
// 要用的時候才依 seed 生成 —— Bytes() 是決定性的。
struct CorpusItem {
	std::string key;
	size_t      size = 0;
	unsigned    seed = 0;
	int         size_class = 0;    // 0=小 1=中 2=大

	std::vector<char> Bytes() const;
};

// 大小級距，報告時分開統計
enum SizeClass { kSmall = 0, kMedium = 1, kLarge = 2, kSizeClassCount = 3 };
const char* SizeClassName(int c);

struct Corpus {
	std::vector<CorpusItem> items;
	long long total_bytes = 0;

	long long BytesInClass(int c) const;
	int       CountInClass(int c) const;
};

// scale = 1.0 約 60MB / 900 個檔，幾秒內跑得完。
// scale = 20 約 1.2GB，接近正式封包的規模。
Corpus MakeCorpus(double scale, unsigned long long seed);

// 只含小檔的語料，用來單獨量「大量小檔」的成本
Corpus MakeSmallFileCorpus(double scale, unsigned long long seed);

} // namespace vfsbench

#endif // VFS_BENCH_CORPUS_H
