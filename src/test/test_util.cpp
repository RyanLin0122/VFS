// test_util.cpp — 測試 / benchmark 共用 VFS 輔助層的實作（純標準 C++17）

#include <vfs_testutil.h>
#include <vfs_platform.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace vfsutil {
namespace {

// 暫存封包名稱要在「同一次執行的不同測試之間」與「同時跑的多個行程之間」都唯一。
// 行程 ID 不是標準 C++ 拿得到的東西，改用啟動時刻的奈秒數當種子 —— 一樣夠散。
std::string UniqueBase(const char* tag) {
	static const unsigned long long run_id =
		static_cast<unsigned long long>(
			std::chrono::steady_clock::now().time_since_epoch().count()) ^
		static_cast<unsigned long long>(
			std::chrono::system_clock::now().time_since_epoch().count());
	static std::atomic<unsigned> counter{0};

	char name[128];
	std::snprintf(name, sizeof(name), "vfs_%s_%08llx_%04u",
		tag ? tag : "t",
		static_cast<unsigned long long>(run_id & 0xFFFFFFFFULL),
		counter.fetch_add(1));
	return vfsplat::PathJoin(vfsplat::ScratchDir(), name);
}

} // namespace

// ---------------------------------------------------------------- PRNG

unsigned long long Rng::Next() {
	// xorshift64*
	s_ ^= s_ >> 12;
	s_ ^= s_ << 25;
	s_ ^= s_ >> 27;
	return s_ * 0x2545F4914F6CDD1DULL;
}

long long Rng::Range(long long lo, long long hi) {
	if (hi <= lo) return lo;
	const unsigned long long span = static_cast<unsigned long long>(hi - lo) + 1ULL;
	return lo + static_cast<long long>(Next() % span);
}

double Rng::Unit() {
	return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0);
}

// ---------------------------------------------------------------- 資料產生

std::vector<char> Blob(size_t size, unsigned seed) {
	std::vector<char> out(size);
	Rng rng(0xC0FFEEULL ^ (static_cast<unsigned long long>(seed) << 17) ^ size);
	size_t i = 0;
	while (i + 8 <= size) {
		const unsigned long long v = rng.Next();
		std::memcpy(out.data() + i, &v, 8);
		i += 8;
	}
	if (i < size) {
		const unsigned long long v = rng.Next();
		std::memcpy(out.data() + i, &v, size - i);
	}
	return out;
}

std::vector<char> Compressible(size_t size, unsigned seed) {
	std::vector<char> out(size);
	char pattern[16];
	Rng rng(0xBEEFULL ^ seed);
	for (int i = 0; i < 16; ++i) pattern[i] = static_cast<char>('a' + (rng.Next() % 26));
	for (size_t i = 0; i < size; ++i)
		out[i] = (i % 997 == 0) ? '\n' : pattern[i % 16];
	return out;
}

// ---------------------------------------------------------------- VFS 讀寫

int FileSize(VfsHandle* h, const std::string& key) {
	if (!h || !h->dt_handle || !h->nt_handle) return -1;
	const int tn = vfs_dt_filename_lookup(h->dt_handle, key.c_str());
	if (tn < 0) return -1;
	const int nt = vfs_dt_filename_get_nt_index(h->dt_handle, tn);
	if (nt < 0) return -1;
	return vfs_nt_node_get_size(h->nt_handle, nt);
}

bool ReadAll(VfsHandle* h, const std::string& key, std::vector<char>* out) {
	out->clear();
	const int size = FileSize(h, key);
	if (size < 0) { vfs_errno = VFS_ERR_FILE_NOT_FOUND; return false; }
	if (size == 0) return true;

	const int fd = vfs_file_open(h, key.c_str(), 2 /* read-write */);
	if (fd < 0) return false;

	out->resize(static_cast<size_t>(size));
	int done = 0;
	while (done < size) {
		const int got = vfs_file_read(h, fd, out->data() + done, size - done);
		if (got <= 0) break;
		done += got;
	}
	vfs_file_close(h, fd);
	if (done != size) { out->resize(static_cast<size_t>(done)); return false; }
	return true;
}

bool WriteAll(VfsHandle* h, const std::string& key, const std::vector<char>& data) {
	if (vfs_file_exists(h, key.c_str())) {
		if (vfs_file_unlink(h, key.c_str()) != 0) return false;
	}
	const int fd = vfs_file_create(h, key.c_str());
	if (fd < 0) return false;

	bool ok = true;
	int done = 0;
	const int total = static_cast<int>(data.size());
	while (done < total) {
		const int put = vfs_file_write(h, fd, data.data() + done, total - done);
		if (put <= 0) { ok = false; break; }
		done += put;
	}
	vfs_file_close(h, fd);
	return ok;
}

std::vector<std::string> ListKeys(VfsHandle* h, const char* pattern) {
	std::vector<std::string> keys;
	VfsGlobResults g = {};
	if (vfs_glob(h, pattern, 0, nullptr, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; ++i)
			if (g.gl_pathv[i]) keys.push_back(g.gl_pathv[i]);
	}
	vfs_glob_free(&g);
	std::sort(keys.begin(), keys.end());
	return keys;
}

// ---------------------------------------------------------------- TempArchive

void RemoveArchive(const std::string& base) {
	vfsplat::FileDelete((base + ".paki").c_str());
	vfsplat::FileDelete((base + ".pak").c_str());
	vfsplat::FileDelete((base + ".lock").c_str());
}

TempArchive::TempArchive(const char* tag) : base_(UniqueBase(tag)) {
	RemoveArchive(base_);          // 前一輪若崩潰留下殘骸，先清掉
	h_ = vfs_start(base_.c_str(), 3);
}

TempArchive::~TempArchive() {
	Close();
	RemoveArchive(base_);
	for (const std::string& tag : snapshots_) {
		vfsplat::FileDelete((base_ + ".snap." + tag + ".pak").c_str());
		vfsplat::FileDelete((base_ + ".snap." + tag + ".paki").c_str());
	}
}

void TempArchive::Close() {
	if (h_) { vfs_end(h_, 0); h_ = nullptr; }
}

VfsHandle* TempArchive::Reopen(int access_mode) {
	Close();
	h_ = vfs_start(base_.c_str(), access_mode);
	return h_;
}

long long TempArchive::PakBytes() const {
	return vfsplat::FileSizeOf((base_ + ".pak").c_str());
}

long long TempArchive::PakiBytes() const {
	return vfsplat::FileSizeOf((base_ + ".paki").c_str());
}

bool TempArchive::Snapshot(const std::string& tag) {
	// 快照前必須先關檔把快取刷出去，否則存到的是磁碟上的舊內容
	const bool was_open = (h_ != nullptr);
	Close();
	const bool ok =
		vfsplat::FileCopy((base_ + ".pak").c_str(),  (base_ + ".snap." + tag + ".pak").c_str()) &&
		vfsplat::FileCopy((base_ + ".paki").c_str(), (base_ + ".snap." + tag + ".paki").c_str());
	if (ok && std::find(snapshots_.begin(), snapshots_.end(), tag) == snapshots_.end())
		snapshots_.push_back(tag);
	if (was_open) Reopen();
	return ok;
}

long long TempArchive::DiffVsSnapshot(const std::string& tag, bool paki,
                                      long long* total_out, long long* grew_out) const {
	const char* ext = paki ? ".paki" : ".pak";
	long long total = 0, grew = 0;
	const long long diff = DiffFileBlocks(base_ + ".snap." + tag + ext, base_ + ext,
	                                      4096, &total, &grew);
	if (total_out) *total_out = total;
	if (grew_out)  *grew_out = grew;
	return diff;
}

long long DiffFileBlocks(const std::string& a, const std::string& b,
                         size_t block_size, long long* total_out, long long* grew_out) {
	if (total_out) *total_out = 0;
	if (grew_out)  *grew_out = 0;

	std::FILE* fa = std::fopen(a.c_str(), "rb");
	if (!fa) return -1;
	std::FILE* fb = std::fopen(b.c_str(), "rb");
	if (!fb) { std::fclose(fa); return -1; }

	std::vector<char> ba(block_size), bb(block_size);
	long long diff = 0, total = 0, grew = 0;
	for (;;) {
		const size_t na = std::fread(ba.data(), 1, block_size, fa);
		const size_t nb = std::fread(bb.data(), 1, block_size, fb);
		if (na == 0 && nb == 0) break;

		const size_t common = (na < nb) ? na : nb;
		++total;
		if (common && std::memcmp(ba.data(), bb.data(), common) != 0) ++diff;
		else if (na != nb && common == 0) ++diff;

		if (na != nb) {
			grew += static_cast<long long>(na > nb ? na - nb : nb - na);
			if (na == 0 || nb == 0) {                 // 一邊已到底，剩下的全算成長
				std::FILE* rest = (na == 0) ? fb : fa;
				for (;;) {
					const size_t n = std::fread(ba.data(), 1, block_size, rest);
					if (!n) break;
					grew += static_cast<long long>(n);
					++total;
				}
				break;
			}
		}
	}
	std::fclose(fa);
	std::fclose(fb);
	if (total_out) *total_out = total;
	if (grew_out)  *grew_out = grew;
	return diff;
}

} // namespace vfsutil
