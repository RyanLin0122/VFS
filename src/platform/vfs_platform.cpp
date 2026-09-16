// vfs_platform.cpp — vfs_platform.h 的實作：純標準 C / C++17
//
// 這個檔案不 include 任何 OS 標頭。唯一的平台分支在 FileSeekAbs / FileTell，
// 因為標準 fseek 的 offset 型別是 long：Linux/macOS 的 64-bit build 上 long
// 是 64-bit 沒問題，但 Windows 的 long 永遠是 32-bit，會在 2GB 處截斷。
// _fseeki64（MSVC）與 fseeko（POSIX）都由 <cstdio> 提供，不需要 OS 標頭。

#include <vfs_platform.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

namespace vfsplat {

// ---------------------------------------------------------------- 檔案

long long FileSizeOf(const char* path) {
	std::error_code ec;
	const std::filesystem::path p(path);
	if (!std::filesystem::is_regular_file(p, ec) || ec) return -1;
	const auto n = std::filesystem::file_size(p, ec);
	if (ec) return -1;
	return static_cast<long long>(n);
}

bool FileDelete(const char* path) {
	std::error_code ec;
	std::filesystem::remove(std::filesystem::path(path), ec);
	return !ec;                       // 檔案本來就不存在時 remove 回 false 但不設 ec
}

bool FileCopy(const char* from, const char* to) {
	// 刻意用 stdio 而非 filesystem::copy_file：後者在部分主機 SDK 上沒實作，
	// 而且我們要的就是「一個位元組一個位元組照搬」的語意。
	std::FILE* fi = std::fopen(from, "rb");
	if (!fi) return false;
	std::FILE* fo = std::fopen(to, "wb");
	if (!fo) { std::fclose(fi); return false; }

	std::vector<char> buf(1 << 20);
	bool ok = true;
	for (;;) {
		const size_t n = std::fread(buf.data(), 1, buf.size(), fi);
		if (n == 0) break;
		if (std::fwrite(buf.data(), 1, n, fo) != n) { ok = false; break; }
	}
	if (std::ferror(fi)) ok = false;
	std::fclose(fi);
	if (std::fclose(fo) != 0) ok = false;
	if (!ok) FileDelete(to);
	return ok;
}

int FileSeekAbs(std::FILE* fp, long long offset) {
#if defined(_WIN32)
	return _fseeki64(fp, offset, SEEK_SET);
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__) || defined(_POSIX_VERSION)
	return fseeko(fp, static_cast<off_t>(offset), SEEK_SET);
#else
	// 主機平台的 fallback。long 若是 64-bit 就沒事；若不是，超過 2GB 會失敗，
	// 這裡讓它明確失敗而不是安靜地定位到錯誤位置。
	if (offset > static_cast<long long>(LONG_MAX)) return -1;
	return std::fseek(fp, static_cast<long>(offset), SEEK_SET);
#endif
}

long long FileTell(std::FILE* fp) {
#if defined(_WIN32)
	return _ftelli64(fp);
#elif defined(__APPLE__) || defined(__linux__) || defined(__unix__) || defined(_POSIX_VERSION)
	return static_cast<long long>(ftello(fp));
#else
	return static_cast<long long>(std::ftell(fp));
#endif
}

// ---------------------------------------------------------------- 目錄

bool DirCreate(const char* path) {
	std::error_code ec;
	const std::filesystem::path p(path);
	if (std::filesystem::exists(p, ec)) return std::filesystem::is_directory(p, ec);
	std::filesystem::create_directories(p, ec);
	return !ec;
}

// ---------------------------------------------------------------- 路徑

char PathSep() {
	return static_cast<char>(std::filesystem::path::preferred_separator);
}

std::string PathJoin(const std::string& a, const std::string& b) {
	if (a.empty()) return b;
	if (b.empty()) return a;
	const char last = a[a.size() - 1];
	if (last == '/' || last == '\\') return a + b;
	return a + PathSep() + b;
}

const std::string& ScratchDir() {
	static const std::string dir = []() -> std::string {
		if (const char* env = std::getenv("VFS_TEST_TMPDIR")) {
			if (env[0] && DirCreate(env)) return env;
		}
		std::error_code ec;
		const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec);
		std::string d = ec ? std::string(".") : tmp.string();
		d = PathJoin(d, "vfstest");
		if (!DirCreate(d.c_str())) d = ".";     // 主機平台可能整個唯讀，退回工作目錄
		return d;
	}();
	return dir;
}

// ---------------------------------------------------------------- 量測

bool ProcessIoBytes(unsigned long long* read_bytes, unsigned long long* write_bytes) {
#if defined(__linux__)
	// /proc/self/io 用普通 fopen 就讀得到，不需要任何 OS 標頭。
	// Steam Deck（Arch + Linux kernel）走的就是這條。
	std::FILE* fp = std::fopen("/proc/self/io", "rb");
	if (!fp) return false;
	char line[256];
	bool got_r = false, got_w = false;
	while (std::fgets(line, sizeof(line), fp)) {
		unsigned long long v = 0;
		if (!got_r && std::sscanf(line, "read_bytes: %llu", &v) == 1) {
			if (read_bytes) *read_bytes = v;
			got_r = true;
		} else if (!got_w && std::sscanf(line, "write_bytes: %llu", &v) == 1) {
			if (write_bytes) *write_bytes = v;
			got_w = true;
		}
	}
	std::fclose(fp);
	return got_r && got_w;
#else
	// 其他平台目前沒有不靠 OS API 就能拿到的行程 I/O 統計。
	// bench 會改用「封包實體大小成長 ÷ 邏輯寫入量」這個可攜的替代指標。
	(void)read_bytes; (void)write_bytes;
	return false;
#endif
}

} // namespace vfsplat
