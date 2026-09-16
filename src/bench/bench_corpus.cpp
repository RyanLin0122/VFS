// bench_corpus.cpp — 合成語料的產生

#include "bench_corpus.h"

#include <vfs_testutil.h>

#include <cstdio>

namespace vfsbench {
namespace {

struct GroupSpec {
	const char* dir;
	int         count_at_scale_1;
	size_t      min_bytes;
	size_t      max_bytes;
	int         size_class;
};

// 仿真實遊戲資源的分佈。檔名長度與目錄深度也刻意做出差異，
// 因為 Patricia trie 的成本跟鍵的長度與共用前綴有關。
const GroupSpec kGroups[] = {
	{ "script/ai",            300,      256,    8 * 1024, kSmall  },
	{ "config",               150,      128,    4 * 1024, kSmall  },
	{ "ui/icon",              200,    1 * 1024,  16 * 1024, kSmall },
	{ "model/character",      120,   16 * 1024, 256 * 1024, kMedium },
	{ "texture/environment",  100,   64 * 1024, 512 * 1024, kMedium },
	{ "sound/sfx",             60,  128 * 1024,  1024 * 1024, kMedium },
	{ "map",                    8, 2 * 1024 * 1024, 8 * 1024 * 1024, kLarge },
	{ "sound/bgm",              6, 3 * 1024 * 1024, 6 * 1024 * 1024, kLarge },
};

const char* kExt[] = { ".dat", ".bin", ".dds", ".ca", ".ogg", ".cfg" };

// 避免產生 size % 512 == 1 的檔案。
// 那是已知 bug（regress/write_size_mod512_eq_1），會讓 bench 的寫入路徑失敗，
// 量出來的數字失去意義。bug 修好後這個調整留著也無害。
size_t AvoidKnownBadSize(size_t n) {
	return (n % 512 == 1) ? n + 1 : n;
}

} // namespace

const char* SizeClassName(int c) {
	switch (c) {
		case kSmall:  return "small";
		case kMedium: return "medium";
		case kLarge:  return "large";
		default:      return "?";
	}
}

std::vector<char> CorpusItem::Bytes() const {
	return vfsutil::Blob(size, seed);
}

long long Corpus::BytesInClass(int c) const {
	long long n = 0;
	for (const CorpusItem& it : items)
		if (it.size_class == c) n += static_cast<long long>(it.size);
	return n;
}

int Corpus::CountInClass(int c) const {
	int n = 0;
	for (const CorpusItem& it : items)
		if (it.size_class == c) ++n;
	return n;
}

Corpus MakeCorpus(double scale, unsigned long long seed) {
	Corpus c;
	vfsutil::Rng rng(seed);
	unsigned item_seed = 1;

	for (const GroupSpec& g : kGroups) {
		int count = static_cast<int>(g.count_at_scale_1 * scale + 0.5);
		if (count < 1) count = 1;

		for (int i = 0; i < count; ++i) {
			CorpusItem it;
			char key[160];
			std::snprintf(key, sizeof(key), "%s/%s_%05d%s",
				g.dir, g.dir[0] ? g.dir : "f", i,
				kExt[rng.Next() % (sizeof(kExt) / sizeof(kExt[0]))]);
			// 把 '/' 以外的分隔統一，鍵一律小寫 '/' 分隔（與打包工具一致）
			for (char* p = key; *p; ++p)
				if (*p == '\\') *p = '/';

			it.key = key;
			it.size = AvoidKnownBadSize(
				static_cast<size_t>(rng.Range(static_cast<long long>(g.min_bytes),
				                              static_cast<long long>(g.max_bytes))));
			it.seed = item_seed++;
			it.size_class = g.size_class;

			c.total_bytes += static_cast<long long>(it.size);
			c.items.push_back(std::move(it));
		}
	}
	return c;
}

Corpus MakeSmallFileCorpus(double scale, unsigned long long seed) {
	Corpus c;
	vfsutil::Rng rng(seed ^ 0xA5A5A5A5ULL);
	unsigned item_seed = 1;

	int count = static_cast<int>(3000 * scale + 0.5);
	if (count < 1) count = 1;

	for (int i = 0; i < count; ++i) {
		CorpusItem it;
		char key[160];
		std::snprintf(key, sizeof(key), "script/gen/s_%06d.lua", i);
		it.key = key;
		it.size = AvoidKnownBadSize(static_cast<size_t>(rng.Range(128, 6 * 1024)));
		it.seed = item_seed++;
		it.size_class = kSmall;
		c.total_bytes += static_cast<long long>(it.size);
		c.items.push_back(std::move(it));
	}
	return c;
}

} // namespace vfsbench
