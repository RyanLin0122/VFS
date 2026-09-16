// vfs_platform.h — 測試 / benchmark 框架的可攜層
//
// 目標平台：Windows / Linux / Steam Deck / macOS / Android / iOS / Switch / PS5。
//
// 規則：只用標準 C 與 C++17 標準函式庫。沒有 windows.h、沒有 unistd.h、
// 沒有 dirent.h、沒有任何第三方相依。整個框架裡唯一的平台分支是
// FileSeekAbs / FileTell 裡的那個 #if（標準 fseek 的 offset 是 long，
// 在 Windows 上只有 32-bit，撐不住 3GB 封包），其餘全部走標準庫。
//
// 移植到主機平台時要看的只有兩個地方：
//   1. ScratchDir()   —— 主機的可寫區通常要走 SDK 的掛載點，用環境變數
//                        VFS_TEST_TMPDIR 覆寫即可，不用改程式。
//   2. ProcessIoBytes() —— 取不到就回 false，bench 會標成 n/a，不會假裝有數字。
#ifndef VFS_PLATFORM_H
#define VFS_PLATFORM_H

#include <chrono>
#include <cstdio>
#include <string>

namespace vfsplat {

// ---------------------------------------------------------------- 時間
//
// steady_clock 保證單調且各平台都有，不需要任何平台程式碼。

inline double MonotonicSeconds() {
	using clock = std::chrono::steady_clock;
	static const clock::time_point origin = clock::now();
	return std::chrono::duration<double>(clock::now() - origin).count();
}

// ---------------------------------------------------------------- 檔案
//
// 命名避開 Win32 巨集（CopyFile / DeleteFile / MoveFile 都是 A/W 巨集）。
// 萬一日後有人在某個 TU 裡 include 了 windows.h，撞名會在連結期才爆炸。

// 位元組數；不存在或讀不到回 -1
long long FileSizeOf(const char* path);

// 刪除檔案。本來就不存在也算成功。
bool FileDelete(const char* path);

// 複製檔案（覆寫目的地）。用 fread/fwrite 實作，不依賴任何 OS API。
bool FileCopy(const char* from, const char* to);

// 64-bit 定位。這是整個框架唯一需要平台分支的地方。
int       FileSeekAbs(std::FILE* fp, long long offset);
long long FileTell(std::FILE* fp);

// ---------------------------------------------------------------- 目錄

// 建立目錄（含中間層）。已存在算成功。
bool DirCreate(const char* path);

// ---------------------------------------------------------------- 路徑

char PathSep();
std::string PathJoin(const std::string& a, const std::string& b);

// 框架用的可寫暫存目錄。
// 依序嘗試：環境變數 VFS_TEST_TMPDIR → 系統暫存目錄 → 目前工作目錄。
// 主機平台上直接設 VFS_TEST_TMPDIR 指到 SDK 的可寫掛載點即可。
const std::string& ScratchDir();

// ---------------------------------------------------------------- 量測

// 這個行程到目前為止的實體 I/O 位元組數，用來算寫放大。
// 取不到就回 false —— bench 會把該欄標成 n/a，而不是填一個假數字。
bool ProcessIoBytes(unsigned long long* read_bytes, unsigned long long* write_bytes);

} // namespace vfsplat

#endif // VFS_PLATFORM_H
