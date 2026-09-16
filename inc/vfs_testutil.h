// vfs_testutil.h — 測試 / benchmark 共用的 VFS 輔助層
//
// vfs.h 的 Layer 7 是 POSIX 風格的 raw 介面（部份讀寫、fd 要自己收），
// 直接拿來寫測試會有一半篇幅在處理迴圈。這裡包一層「一次搞定」的語意，
// 讓測試本體只描述要驗什麼。
//
// 可攜性：只用標準 C++17 與 vfs_platform.h，沒有任何 OS 標頭。
#ifndef VFS_TESTUTIL_H
#define VFS_TESTUTIL_H

#include <vfs.h>

#include <string>
#include <vector>

namespace vfsutil {

// ---------------------------------------------------------------- 資料產生

// 決定性的偽隨機 blob：同樣 (size, seed) 永遠得到同樣內容。
// 失敗案例才能重現，也才能做 byte-level 的可重現性驗證。
std::vector<char> Blob(size_t size, unsigned seed);

// 高度可壓縮的 blob（重複樣式），模擬文字／設定檔這類資源
std::vector<char> Compressible(size_t size, unsigned seed);

// xorshift64*。測試與 bench 共用同一顆 PRNG，跨平台結果才一致
// （std 的 distribution 各家實作不同，不能用）。
class Rng {
public:
	explicit Rng(unsigned long long seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
	unsigned long long Next();
	long long Range(long long lo, long long hi);   // 含兩端
	double    Unit();                              // [0,1)
private:
	unsigned long long s_;
};

// ---------------------------------------------------------------- VFS 讀寫

// 一次讀完整個 VFS 檔（內部處理部份讀取）
bool ReadAll(VfsHandle* h, const std::string& key, std::vector<char>* out);
// 一次寫完（已存在就先 unlink 達成覆寫）
bool WriteAll(VfsHandle* h, const std::string& key, const std::vector<char>& data);
// 不佔 fd 取得檔案大小；不存在回 -1
int  FileSize(VfsHandle* h, const std::string& key);
// glob 成所有鍵名（已排序，方便比對）
std::vector<std::string> ListKeys(VfsHandle* h, const char* pattern = "*");

// ---------------------------------------------------------------- 暫存封包

// RAII：建立唯一命名的暫存封包，解構時關閉並刪掉 .pak / .paki / .lock
// 與所有 snapshot。測試之間不會撞檔名，失敗時也不留垃圾。
class TempArchive {
public:
	// tag 只是為了讓殘留檔案（若程式崩潰）看得出來是誰留下的
	explicit TempArchive(const char* tag);
	~TempArchive();

	TempArchive(const TempArchive&) = delete;
	TempArchive& operator=(const TempArchive&) = delete;

	VfsHandle* h() const { return h_; }
	const std::string& base() const { return base_; }
	bool Ok() const { return h_ != nullptr; }

	// 關閉再開，用來驗證持久化。失敗回 nullptr。
	VfsHandle* Reopen(int access_mode = 3);
	void Close();                            // 只關不刪

	long long PakBytes() const;              // .pak 實體大小（會先 flush）
	long long PakiBytes() const;             // .paki 實體大小

	// 把目前的 .pak / .paki 複製一份留存，供之後做 byte-level 比對。
	// 內部會先關檔確保快取已刷出，再重新開啟。
	bool Snapshot(const std::string& tag);

	// 與某個 snapshot 比對，回傳有差異的 4096-byte 區塊數。
	// total_out 給總區塊數，grew_out 給長度差。找不到 snapshot 回 -1。
	long long DiffVsSnapshot(const std::string& tag, bool paki,
	                         long long* total_out = nullptr,
	                         long long* grew_out = nullptr) const;

private:
	std::string              base_;
	VfsHandle*               h_ = nullptr;
	std::vector<std::string> snapshots_;      // 自己記著，不必去掃目錄
};

// 刪掉某個封包的全部實體檔（含 .lock）
void RemoveArchive(const std::string& base);

// 比對兩個實體檔：回傳有差異的 block_size 區塊數，total_out 給總區塊數，
// grew_out 給長度差的位元組數。任一檔打不開回 -1。
long long DiffFileBlocks(const std::string& a, const std::string& b,
                         size_t block_size,
                         long long* total_out, long long* grew_out);

} // namespace vfsutil

#endif // VFS_TESTUTIL_H
