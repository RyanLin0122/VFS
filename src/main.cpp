// vfstool — VFS 封包虛擬檔案系統的命令列工具
//
// VFS 把大量遊戲資源塞進兩個實體檔：
//   <name>.paki  — 索引（IIO 多通道：DT TrieNode / DT KeyNode / NT / FAT）
//   <name>.pak   — 資料區塊
//   <name>.lock  — 跨行程鎖
// 本工具把 vfs.cpp 的 Layer 7 API（vfs_file_* / vfs_glob）包成 console 介面。

#include <vfs.h>

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// 測試與 benchmark 的入口（src/test/test_main.cpp）。
// 刻意只用前置宣告，不讓 vfs_test.h / vfs_bench.h 的相依滲進這個純 CLI 檔。
int VfsTestMain(int argc, char** argv);
int VfsBenchMain(int argc, char** argv);

namespace {

// ---------------------------------------------------------------- 共用小工具

// 封包內的鍵一律小寫 + '/' 分隔。跨平台時檔名大小寫敏感度不一致是災難的來源，
// 統一在工具端正規化掉。
std::string NormalizeKey(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    // 去掉開頭的 "./" 與 '/'
    size_t start = 0;
    while (start + 1 < out.size() && out[start] == '.' && out[start + 1] == '/') start += 2;
    while (start < out.size() && out[start] == '/') ++start;
    return out.substr(start);
}

void Fail(const char* what) {
    vfs_perror(nullptr, what);
}

// 直接查 NT 取得檔案大小（不佔用 fd）。vfs.h 缺 public 的 size API，走內部結構，
// 之後補上 public 的 size API 就能拿掉這個權宜做法。
int VfsFileSize(VfsHandle* h, const char* key) {
    int tn = vfs_dt_filename_lookup(h->dt_handle, key);
    if (tn < 0) return -1;
    const int nt = vfs_dt_filename_get_nt_index(h->dt_handle, tn);
    if (nt < 0) return -1;
    return vfs_nt_node_get_size(h->nt_handle, nt);
}

// 依序試「原字串」→「正規化字串」，回傳實際存在的那個鍵
bool ResolveKey(VfsHandle* h, const std::string& want, std::string* found) {
    if (vfs_file_exists(h, want.c_str())) { *found = want; return true; }
    std::string norm = NormalizeKey(want);
    if (norm != want && vfs_file_exists(h, norm.c_str())) { *found = norm; return true; }
    return false;
}

bool ReadHostFile(const char* path, std::vector<char>* out) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;
    _fseeki64(fp, 0, SEEK_END);
    long long size = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    if (size < 0 || size > 0x7FFFFFFF) { fclose(fp); return false; }
    out->resize(static_cast<size_t>(size));
    size_t got = size ? fread(out->data(), 1, static_cast<size_t>(size), fp) : 0;
    fclose(fp);
    return got == static_cast<size_t>(size);
}

bool WriteHostFile(const char* path, const std::vector<char>& data) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), fp) == data.size();
    fclose(fp);
    return ok;
}

// 遞迴建立目錄（給 extract 用）
void EnsureHostDir(const std::string& dir) {
    if (dir.empty()) return;
    std::string cur;
    for (size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '\\' || dir[i] == '/') {
            if (!cur.empty() && cur.back() != ':') CreateDirectoryA(cur.c_str(), nullptr);
            if (i == dir.size()) break;
        }
        cur.push_back(dir[i]);
    }
}

long long HostFileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return -1;
    return (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

std::string HumanSize(long long bytes) {
    char buf[64];
    const char* unit[] = { "B", "KB", "MB", "GB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 3) { v /= 1024.0; ++u; }
    if (u == 0) sprintf_s(buf, "%lld B", bytes);
    else        sprintf_s(buf, "%.2f %s", v, unit[u]);
    return buf;
}

// ---------------------------------------------------------------- VFS 讀寫

// 讀整個 VFS 檔（vfs_file_read 可能只回部份，要迴圈推進）
bool VfsRead(VfsHandle* h, const std::string& key, std::vector<char>* out) {
    int size = VfsFileSize(h, key.c_str());
    if (size < 0) { vfs_errno = VFS_ERR_FILE_NOT_FOUND; return false; }

    int fd = vfs_file_open(h, key.c_str(), 2 /* read-write */);
    if (fd < 0) return false;

    out->resize(static_cast<size_t>(size));
    int done = 0;
    while (done < size) {
        int got = vfs_file_read(h, fd, out->data() + done, size - done);
        if (got <= 0) break;
        done += got;
    }
    vfs_file_close(h, fd);
    if (done != size) { out->resize(static_cast<size_t>(done)); return false; }
    return true;
}

// 寫入 VFS（已存在就先 unlink 達成覆寫）
bool VfsWrite(VfsHandle* h, const std::string& key, const std::vector<char>& data) {
    if (vfs_file_exists(h, key.c_str())) {
        if (vfs_file_unlink(h, key.c_str()) != 0) return false;
    }
    int fd = vfs_file_create(h, key.c_str());
    if (fd < 0) return false;

    bool ok = true;
    int done = 0;
    const int total = static_cast<int>(data.size());
    while (done < total) {
        int put = vfs_file_write(h, fd, data.data() + done, total - done);
        if (put <= 0) { ok = false; break; }
        done += put;
    }
    vfs_file_close(h, fd);
    return ok;
}

// ---------------------------------------------------------------- 開關檔

// 讀寫模式開啟；archive 不存在時會建立
VfsHandle* OpenRW(const char* archive) {
    VfsHandle* h = vfs_start(archive, 3);
    if (!h) Fail("vfs_start");
    return h;
}

// 唯讀用途：先確認 .paki/.pak 都在，避免打錯字時被 vfs_start 靜靜建出空封包
VfsHandle* OpenExisting(const char* archive) {
    if (!vfs_exists(archive)) {
        fprintf(stderr, "找不到封包：%s.paki / %s.pak\n", archive, archive);
        return nullptr;
    }
    return OpenRW(archive);
}

// ---------------------------------------------------------------- 各指令

int CmdCreate(const char* archive) {
    if (vfs_exists(archive)) {
        fprintf(stderr, "封包已存在：%s\n", archive);
        return 1;
    }
    VfsHandle* h = OpenRW(archive);
    if (!h) return 1;
    vfs_end(h, 0);
    printf("已建立 %s.paki / %s.pak\n", archive, archive);
    return 0;
}

int CmdList(const char* archive, const char* pattern, bool longFormat) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    std::string pat = pattern ? pattern : "*";
    VfsGlobResults g = {};
    int rc = vfs_glob(h, pat.c_str(), 0 /* 排序 */, nullptr, &g);
    if (rc == 1) {                       // GLOB_NOMATCH → 用正規化過的 pattern 再試一次
        std::string norm = NormalizeKey(pat);
        if (norm != pat) rc = vfs_glob(h, norm.c_str(), 0, nullptr, &g);
    }
    if (rc != 0) {
        printf("(無符合 \"%s\" 的檔案)\n", pat.c_str());
        vfs_glob_free(&g);
        vfs_end(h, 0);
        return rc == 1 ? 0 : 1;
    }

    long long total = 0;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        const char* key = g.gl_pathv[g.gl_offs + i];
        if (!key) continue;
        if (longFormat) {
            int size = VfsFileSize(h, key);
            total += (size > 0 ? size : 0);
            printf("%12d  %s\n", size, key);
        } else {
            printf("%s\n", key);
        }
    }
    if (longFormat) printf("---\n%zu 個檔案，合計 %s\n", g.gl_pathc, HumanSize(total).c_str());

    vfs_glob_free(&g);
    vfs_end(h, 0);
    return 0;
}

int CmdInfo(const char* archive) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    VfsGlobResults g = {};
    long long logical = 0;
    size_t count = 0;
    if (vfs_glob(h, "*", 4 /* NOSORT */, nullptr, &g) == 0) {
        count = g.gl_pathc;
        for (size_t i = 0; i < g.gl_pathc; ++i) {
            const char* key = g.gl_pathv[g.gl_offs + i];
            if (!key) continue;
            int size = VfsFileSize(h, key);
            if (size > 0) logical += size;
        }
    }
    vfs_glob_free(&g);

    std::string paki = std::string(archive) + ".paki";
    std::string pak = std::string(archive) + ".pak";
    long long pakiSize = HostFileSize(paki);
    long long pakSize = HostFileSize(pak);

    printf("封包         : %s\n", archive);
    printf("檔案數       : %zu\n", count);
    printf("內容合計     : %s\n", HumanSize(logical).c_str());
    printf("%-12s : %s\n", ".paki", pakiSize < 0 ? "(缺)" : HumanSize(pakiSize).c_str());
    printf("%-12s : %s\n", ".pak", pakSize < 0 ? "(缺)" : HumanSize(pakSize).c_str());
    if (pakSize > 0 && logical > 0) {
        printf("區塊使用率   : %.1f%%（block size = %d）\n",
               100.0 * static_cast<double>(logical) / static_cast<double>(pakSize),
               vfs_iio_BLOCK_SIZEv);
    }

    vfs_end(h, 0);
    return 0;
}

int CmdAdd(const char* archive, const char* hostFile, const char* vfsPath) {
    std::vector<char> data;
    if (!ReadHostFile(hostFile, &data)) {
        fprintf(stderr, "讀不到來源檔：%s\n", hostFile);
        return 1;
    }
    VfsHandle* h = OpenRW(archive);
    if (!h) return 1;

    std::string key = NormalizeKey(vfsPath ? vfsPath : hostFile);
    bool ok = VfsWrite(h, key, data);
    if (!ok) Fail("vfs write");
    else printf("已寫入 %s (%s)\n", key.c_str(), HumanSize(static_cast<long long>(data.size())).c_str());

    vfs_end(h, 0);
    return ok ? 0 : 1;
}

// 遞迴走訪磁碟目錄，把檔案逐一打包
int PackDir(VfsHandle* h, const std::string& dir, const std::string& root,
            const std::string& prefix, int* packed, long long* bytes) {
    WIN32_FIND_DATAA fd;
    std::string spec = dir + "\\*";
    HANDLE hFind = FindFirstFileA(spec.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int errors = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        std::string full = dir + "\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            errors += PackDir(h, full, root, prefix, packed, bytes);
            continue;
        }

        std::vector<char> data;
        if (!ReadHostFile(full.c_str(), &data)) {
            fprintf(stderr, "  略過（讀取失敗）：%s\n", full.c_str());
            ++errors;
            continue;
        }
        std::string rel = full.substr(root.size());
        std::string key = NormalizeKey(prefix.empty() ? rel : prefix + "/" + rel);
        if (!VfsWrite(h, key, data)) {
            fprintf(stderr, "  略過（寫入失敗）：%s\n", key.c_str());
            ++errors;
            continue;
        }
        ++(*packed);
        *bytes += static_cast<long long>(data.size());
        printf("  + %s (%zu)\n", key.c_str(), data.size());
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return errors;
}

int CmdAddTree(const char* archive, const char* hostDir, const char* prefix) {
    DWORD attr = GetFileAttributesA(hostDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "不是目錄：%s\n", hostDir);
        return 1;
    }
    VfsHandle* h = OpenRW(archive);
    if (!h) return 1;

    std::string root = hostDir;
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    const std::string base = root;
    root.push_back('\\');   // rel = full.substr(root.size()) 靠這個尾斜線

    int packed = 0;
    long long bytes = 0;
    int errors = PackDir(h, base, root,
                         prefix ? NormalizeKey(prefix) : std::string(), &packed, &bytes);

    vfs_end(h, 0);
    printf("---\n打包 %d 個檔案，%s%s\n", packed, HumanSize(bytes).c_str(),
           errors ? "（有錯誤，見上）" : "");
    return errors ? 1 : 0;
}

int CmdExtract(const char* archive, const char* vfsPath, const char* hostFile) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    std::string key;
    if (!ResolveKey(h, vfsPath, &key)) {
        fprintf(stderr, "封包內找不到：%s\n", vfsPath);
        vfs_end(h, 0);
        return 1;
    }

    std::vector<char> data;
    bool ok = VfsRead(h, key, &data);
    vfs_end(h, 0);
    if (!ok) { Fail("vfs read"); return 1; }

    std::string dest = hostFile ? hostFile : key.substr(key.find_last_of('/') + 1);
    size_t slash = dest.find_last_of("\\/");
    if (slash != std::string::npos) EnsureHostDir(dest.substr(0, slash));
    if (!WriteHostFile(dest.c_str(), data)) {
        fprintf(stderr, "寫不出檔案：%s\n", dest.c_str());
        return 1;
    }
    printf("%s -> %s (%s)\n", key.c_str(), dest.c_str(),
           HumanSize(static_cast<long long>(data.size())).c_str());
    return 0;
}

int CmdExtractAll(const char* archive, const char* destDir, const char* pattern) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    std::string pat = pattern ? pattern : "*";
    VfsGlobResults g = {};
    int rc = vfs_glob(h, pat.c_str(), 0, nullptr, &g);
    if (rc != 0) {
        printf("(無符合 \"%s\" 的檔案)\n", pat.c_str());
        vfs_glob_free(&g);
        vfs_end(h, 0);
        return rc == 1 ? 0 : 1;
    }

    std::string root = destDir;
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    EnsureHostDir(root);

    int done = 0, errors = 0;
    long long bytes = 0;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        const char* key = g.gl_pathv[g.gl_offs + i];
        if (!key) continue;

        std::vector<char> data;
        if (!VfsRead(h, key, &data)) { fprintf(stderr, "  失敗：%s\n", key); ++errors; continue; }

        std::string dest = root + "\\" + key;
        for (char& c : dest) if (c == '/') c = '\\';
        size_t slash = dest.find_last_of('\\');
        if (slash != std::string::npos) EnsureHostDir(dest.substr(0, slash));
        if (!WriteHostFile(dest.c_str(), data)) { fprintf(stderr, "  寫入失敗：%s\n", dest.c_str()); ++errors; continue; }

        ++done;
        bytes += static_cast<long long>(data.size());
        printf("  %s\n", key);
    }
    vfs_glob_free(&g);
    vfs_end(h, 0);
    printf("---\n取出 %d 個檔案，%s%s\n", done, HumanSize(bytes).c_str(),
           errors ? "（有錯誤，見上）" : "");
    return errors ? 1 : 0;
}

int CmdDelete(const char* archive, const char* vfsPath) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    std::string key;
    if (!ResolveKey(h, vfsPath, &key)) {
        fprintf(stderr, "封包內找不到：%s\n", vfsPath);
        vfs_end(h, 0);
        return 1;
    }
    bool ok = (vfs_file_unlink(h, key.c_str()) == 0);
    if (!ok) Fail("vfs_file_unlink");
    else printf("已刪除 %s\n", key.c_str());

    vfs_end(h, 0);
    return ok ? 0 : 1;
}

int CmdCat(const char* archive, const char* vfsPath, int maxBytes) {
    VfsHandle* h = OpenExisting(archive);
    if (!h) return 1;

    std::string key;
    if (!ResolveKey(h, vfsPath, &key)) {
        fprintf(stderr, "封包內找不到：%s\n", vfsPath);
        vfs_end(h, 0);
        return 1;
    }
    std::vector<char> data;
    bool ok = VfsRead(h, key, &data);
    vfs_end(h, 0);
    if (!ok) { Fail("vfs read"); return 1; }

    printf("%s — %s\n", key.c_str(), HumanSize(static_cast<long long>(data.size())).c_str());
    const size_t cap = static_cast<size_t>(maxBytes < 0 ? 0 : maxBytes);
    size_t show = data.size() < cap ? data.size() : cap;
    for (size_t off = 0; off < show; off += 16) {
        printf("%08zx  ", off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < show) printf("%02x ", static_cast<unsigned char>(data[off + i]));
            else                printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" |");
        for (size_t i = 0; i < 16 && off + i < show; ++i) {
            unsigned char c = static_cast<unsigned char>(data[off + i]);
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        printf("|\n");
    }
    if (show < data.size()) printf("... 還有 %zu bytes\n", data.size() - show);
    return 0;
}

// ---------------------------------------------------------------- selftest

#define VFS_CHECK(cond, msg)                              \
    do {                                                  \
        if (cond) { printf("  [ OK ] %s\n", msg); }       \
        else { printf("  [FAIL] %s\n", msg); ++failed; }  \
    } while (0)

int CmdSelfTest(const char* archiveIn) {
    std::string archive = archiveIn ? archiveIn : "vfs_selftest";
    int failed = 0;

    // 收拾上一輪殘留
    remove((archive + ".paki").c_str());
    remove((archive + ".pak").c_str());
    remove((archive + ".lock").c_str());

    printf("== selftest: %s ==\n", archive.c_str());

    // 1) 建立
    VfsHandle* h = vfs_start(archive.c_str(), 3);
    VFS_CHECK(h != nullptr, "vfs_start 建立新封包");
    if (!h) return 1;

    // 2) 寫入三個檔案，含跨多區塊的大檔
    std::vector<char> small(13);
    memcpy(small.data(), "hello, vfs!!\n", 13);

    std::vector<char> big(static_cast<size_t>(vfs_iio_BLOCK_SIZEv) * 3 + 777);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);

    std::vector<char> empty;

    VFS_CHECK(VfsWrite(h, "text/hello.txt", small), "寫入小檔");
    VFS_CHECK(VfsWrite(h, "bin/large.dat", big), "寫入跨區塊大檔");
    VFS_CHECK(VfsWrite(h, "text/empty.bin", empty), "寫入空檔");

    // 3) 存在性
    VFS_CHECK(vfs_file_exists(h, "text/hello.txt") != 0, "vfs_file_exists 命中");
    VFS_CHECK(vfs_file_exists(h, "text/nope.txt") == 0, "vfs_file_exists 未命中");

    // 4) 大小
    VFS_CHECK(VfsFileSize(h, "text/hello.txt") == 13, "小檔大小正確");
    VFS_CHECK(VfsFileSize(h, "bin/large.dat") == static_cast<int>(big.size()), "大檔大小正確");

    // 5) 讀回比對
    std::vector<char> back;
    VFS_CHECK(VfsRead(h, "text/hello.txt", &back) && back == small, "小檔內容一致");
    VFS_CHECK(VfsRead(h, "bin/large.dat", &back) && back == big, "大檔內容一致");

    // 6) glob
    VfsGlobResults g = {};
    VFS_CHECK(vfs_glob(h, "text/*", 0, nullptr, &g) == 0 && g.gl_pathc == 2, "glob \"text/*\" 命中 2 筆");
    vfs_glob_free(&g);
    VFS_CHECK(vfs_glob(h, "*", 0, nullptr, &g) == 0 && g.gl_pathc == 3, "glob \"*\" 命中 3 筆");
    vfs_glob_free(&g);
    VFS_CHECK(vfs_glob(h, "zzz/*", 0, nullptr, &g) == 1, "glob 無匹配回 GLOB_NOMATCH");
    vfs_glob_free(&g);

    // 7) 覆寫
    std::vector<char> small2(5);
    memcpy(small2.data(), "again", 5);
    VFS_CHECK(VfsWrite(h, "text/hello.txt", small2), "覆寫既有檔案");
    VFS_CHECK(VfsRead(h, "text/hello.txt", &back) && back == small2, "覆寫後內容正確");

    // 8) 刪除
    VFS_CHECK(vfs_file_unlink(h, "text/empty.bin") == 0, "vfs_file_unlink");
    VFS_CHECK(vfs_file_exists(h, "text/empty.bin") == 0, "刪除後查不到");

    // 9) 關閉再開，驗證持久化
    vfs_end(h, 0);
    h = vfs_start(archive.c_str(), 3);
    VFS_CHECK(h != nullptr, "重新開啟既有封包");
    if (h) {
        VFS_CHECK(VfsRead(h, "bin/large.dat", &back) && back == big, "重開後大檔仍一致");
        VFS_CHECK(VfsRead(h, "text/hello.txt", &back) && back == small2, "重開後覆寫結果仍在");
        VFS_CHECK(vfs_file_exists(h, "text/empty.bin") == 0, "重開後刪除仍生效");
        vfs_end(h, 0);
    }

    printf("== %s ==\n", failed ? "有失敗項目" : "全部通過");
    return failed ? 1 : 0;
}

#undef VFS_CHECK

// ---------------------------------------------------------------- usage

void Usage(const char* exe) {
    printf(
        "VFS 封包工具 — 操作 .pak / .paki 虛擬檔案系統\n"
        "\n"
        "用法：%s <command> [args]\n"
        "\n"
        "  create     <archive>                       建立空封包（產生 .paki + .pak）\n"
        "  info       <archive>                       顯示檔案數、大小、區塊使用率\n"
        "  list       <archive> [pattern]             列出符合 glob 的檔案（預設 *）\n"
        "  ll         <archive> [pattern]             同 list，另外顯示每個檔案大小\n"
        "  add        <archive> <hostfile> [vfspath]  把磁碟檔案寫入封包\n"
        "  addtree    <archive> <hostdir> [prefix]    遞迴打包整個目錄\n"
        "  extract    <archive> <vfspath> [hostfile]  取出單一檔案\n"
        "  extractall <archive> <destdir> [pattern]   批次取出到目錄\n"
        "  delete     <archive> <vfspath>             從封包刪除檔案\n"
        "  cat        <archive> <vfspath> [maxbytes]  hex dump 檔案內容（預設 256）\n"
        "  selftest   [archive]                       跑內建自我測試（舊版，保留相容）\n"
        "\n"
        "  test       [suite...] [-l] [-v]            單元測試；test --help 看細節\n"
        "  bench      [pattern...] [--json|--compare] 效能量測；bench --help 看細節\n"
        "\n"
        "archive 是不含副檔名的基底名稱，例如 \"assets\" 對應 assets.paki + assets.pak。\n"
        "封包內的鍵一律小寫、以 '/' 分隔；pattern 支援 * ? [] 萬用字元。\n"
        "\n"
        "範例：\n"
        "  %s list assets \"character/*.dds\"\n"
        "  %s extract assets map/town.dat .\\town.dat\n"
        "  %s addtree assets .\\build\\assets\n",
        exe, exe, exe, exe);
}

}  // namespace

int main(int argc, char** argv) {
    const char* exe = "vfs";
    if (argc > 0 && argv[0]) {
        const char* slash = strrchr(argv[0], '\\');
        exe = slash ? slash + 1 : argv[0];
    }

    if (argc < 2) { Usage(exe); return 1; }

    // 區塊大小照系統分頁走
    vfs_iio_BLOCK_SIZEv = static_cast<int>(get_page_size());

    const std::string cmd = argv[1];
    const char* a1 = argc > 2 ? argv[2] : nullptr;
    const char* a2 = argc > 3 ? argv[3] : nullptr;
    const char* a3 = argc > 4 ? argv[4] : nullptr;

    auto need = [&](int n) -> bool {
        if (argc < n) { fprintf(stderr, "參數不足，用 %s 看說明\n", exe); return false; }
        return true;
    };

    if (cmd == "create")     return need(3) ? CmdCreate(a1) : 1;
    if (cmd == "info")       return need(3) ? CmdInfo(a1) : 1;
    if (cmd == "list")       return need(3) ? CmdList(a1, a2, false) : 1;
    if (cmd == "ll")         return need(3) ? CmdList(a1, a2, true) : 1;
    if (cmd == "add")        return need(4) ? CmdAdd(a1, a2, a3) : 1;
    if (cmd == "addtree")    return need(4) ? CmdAddTree(a1, a2, a3) : 1;
    if (cmd == "extract")    return need(4) ? CmdExtract(a1, a2, a3) : 1;
    if (cmd == "extractall") return need(4) ? CmdExtractAll(a1, a2, a3) : 1;
    if (cmd == "delete")     return need(4) ? CmdDelete(a1, a2) : 1;
    if (cmd == "cat")        return need(4) ? CmdCat(a1, a2, a3 ? atoi(a3) : 256) : 1;
    if (cmd == "selftest")   return CmdSelfTest(a1);
    if (cmd == "test")       return VfsTestMain(argc - 2, argv + 2);
    if (cmd == "bench")      return VfsBenchMain(argc - 2, argv + 2);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") { Usage(exe); return 0; }

    fprintf(stderr, "未知指令：%s\n\n", cmd.c_str());
    Usage(exe);
    return 1;
}
