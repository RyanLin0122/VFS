# VFS — 遊戲資源封包虛擬檔案系統

把大量遊戲資源打包成兩個實體檔，提供 POSIX 風格的檔案 API，附帶單元測試與效能量測框架。

| 檔案 | 內容 |
|---|---|
| `<name>.paki` | 索引。IIO 多通道格式，magic `"AIO2"` |
| `<name>.pak` | 資料區塊（512 bytes 一塊） |
| `<name>.lock` | 跨行程鎖（`lock_check` / `lock_leave`） |

`.paki` 通道配置固定：Ch0 = DT TrieNode（20 bytes）、Ch1 = DT KeyNode、Ch2 = NT、Ch3 = FAT。

舊的 `"AIOD"` 格式（TrieNode 16 bytes，上限 32767 個檔）不支援，要從原始檔案用 `addtree` 重新打包。

## 建置

Visual Studio 2022，x64（Debug / Release）。

```bash
MSBuild.exe VFS.vcxproj -p:Configuration=Release -p:Platform=x64 -nologo -verbosity:minimal -m:1
```

找不到 MSBuild 時：

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" \
    -latest -products '*' -find "MSBuild\**\Bin\MSBuild.exe"
```

輸出在 `bin\x64\<Configuration>\vfs.exe`。

- 需要 `/utf-8`：原始碼是 UTF-8，多數沒有 BOM。
- 不能定義 `NOMINMAX`：`vfs.cpp` 用了 windows.h 的 `min` 巨集。

## 用法

```
vfs <command> [args]

  create     <archive>                       建立空封包
  info       <archive>                       檔案數、大小、區塊使用率
  list       <archive> [pattern]             列出符合 glob 的檔案（預設 *）
  ll         <archive> [pattern]             同 list，多顯示每個檔案大小
  add        <archive> <hostfile> [vfspath]  把磁碟檔案寫入封包
  addtree    <archive> <hostdir> [prefix]    遞迴打包整個目錄
  extract    <archive> <vfspath> [hostfile]  取出單一檔案
  extractall <archive> <destdir> [pattern]   批次取出到目錄
  delete     <archive> <vfspath>             從封包刪除檔案
  cat        <archive> <vfspath> [maxbytes]  hex dump（預設 256 bytes）
  selftest   [archive]                       舊版內建自我測試（保留相容）

  test       [suite...] [-l] [-v]            單元測試
  bench      [pattern...] [--json|--compare] 效能量測
```

`archive` 是不含副檔名的基底名稱：`assets` 對應 `assets.paki` + `assets.pak`。

封包內的鍵一律小寫、以 `/` 分隔。`add` / `addtree` 會自動正規化；查詢類指令先試原字串，找不到再試正規化後的。

## 可調參數

全域變數，須在 `vfs_start` 之前設定：

| 變數 | 預設 | 說明 |
|---|---|---|
| `vfs_iio_CACHE_PAGES` | 64 | IIO 每個通道的 LRU 快取頁數（四個通道合計約 4MB） |
| `vfs_data_CACHE_BYTES` | 65536 | `.pak` 滑動視窗大小 |

`vfs_stat_*` 是診斷計數器（FAT 讀寫次數、`next_free` 掃描步數、`.pak` 視窗滑動次數等），配合 `vfs bench -v` 使用。

## 測試

```bash
vfs test              # 全部
vfs test --list       # 列出 suite
vfs test file dt      # 指定 suite
```

暫存封包放在系統暫存目錄的 `vfstest/`，可用 `VFS_TEST_TMPDIR` 改位置（大規模測試與 bench 需要幾 GB 空間）。

| suite | 涵蓋 |
|---|---|
| `file` | 讀寫往返、覆寫、lseek、append、unlink / link、fd 管理、glob、檔名、大量檔案、交錯操作 |
| `iio` | 通道配置、讀寫往返、快取淘汰、通道成長、錯誤參數 |
| `data` | 區塊往返、滑動視窗邊界、contiguous 等價性、EOF 補零 |
| `fat` | 鏈建立／延伸／縮減／銷毀、空間回收、get_nth |
| `nt` | 節點配置、欄位往返、refcount、持久化 |
| `dt` | trie 插入／查詢／刪除、長檔名、glob 樣式比對、刪除的副作用 |
| `compat` | `.paki` 磁碟 magic 是 `"AIO2"` |
| `regress` | 回歸案例、增量更新的變動量約束 |

### 寫新測試

```cpp
VFS_TEST(file, my_case) {
    vfsutil::TempArchive a("mycase");   // RAII，解構時自動刪掉 .pak/.paki/.lock
    REQUIRE(a.Ok());

    const std::vector<char> data = vfsutil::Blob(4096, 42);  // 決定性內容
    CHECK(vfsutil::WriteAll(a.h(), "some/key.bin", data));

    std::vector<char> back;
    CHECK(vfsutil::ReadAll(a.h(), "some/key.bin", &back));
    CHECK_BLOB_EQ(back, data);          // 失敗時會印出第一個差異的 offset
}
```

`CHECK_*` 失敗後繼續，`REQUIRE_*` 失敗就中止該案例。測試會自動註冊。

## Benchmark

```bash
vfs bench                                   # 全部 pattern
vfs bench --list                            # 列出 pattern
vfs bench randread --scale 20               # 單一 pattern、接近正式封包的規模
vfs bench seqread --cold 16384              # 量測前把封包擠出 OS 快取（16GB ballast）

vfs bench --repeats 3 --json bench-baseline.json      # 建立基準
vfs bench --repeats 3 --compare bench-baseline.json   # 比對，退出碼非 0 = 有退步
```

| pattern | 量什麼 |
|---|---|
| `pack` | 打包速度：整體、分大小級距、大量小檔、空間放大 |
| `seqread` | 循序讀，連續封包 vs 碎片化封包 |
| `randread` | 隨機讀，IOPS 與 p50/p95/p99 延遲 |
| `write` | 新增 / 原地覆寫 / 增長，含磁碟成長比 |
| `open` | `vfs_start` 延遲、查名字延遲、glob 全表，500/2000/8000 檔各量一次 |
| `patch` | 增量更新造成的 `.pak`/`.paki` 變動比例 |

語料由固定種子在記憶體中合成（`src/bench/bench_corpus.cpp`），每次結果一致。

- 預設規模（`--scale 1`，約 157MB）整包在 OS 快取內，量的是 VFS 的 CPU 成本。要量磁碟 I/O 用 `--scale 20` 以上，或 `--cold <MB>`（ballast 給實體記憶體的 1~2 倍）。
- `--repeats` 取最佳值，不取中位數。
- 標 `(參考)` 的指標不列入退步判定。
- 比對時兩邊的 `--repeats` / `--scale` / `--cold` 要一致。預設容差 25%，用 `--tolerance` 調整。

`bench-baseline.json` 是目前的基準，`bench-original.json` 是舊版實作的基準。

### 寫新 pattern

```cpp
VFS_BENCH(mypattern, "一句話說明") {
    vfsutil::TempArchive a("bench_mine");
    if (!a.Ok()) return;

    const vfsbench::Timer t;
    /* ... 受測的操作 ... */
    const double sec = t.Elapsed();

    vfsbench::Result r;
    r.pattern = "mypattern";
    r.variant = "";                                  // 可空
    r.Add("throughput_mb_s", mb / sec, "MB/s", true); // true = 越大越好
    r.AddInfo("elapsed_s", sec, "s", false);          // Info = 不納入門檻
    out->push_back(r);
}
```

## 可攜性

測試與 benchmark 框架只用標準 C 與 C++17，沒有 OS 標頭與第三方相依。

| 檔案 | 角色 |
|---|---|
| `inc/vfs_platform.h` | 可攜層介面 |
| `src/platform/vfs_platform.cpp` | 實作，只用 `<cstdio>` / `<chrono>` / `<filesystem>` |

移植到主機平台時：

- `ScratchDir()`：可寫區設 `VFS_TEST_TMPDIR` 即可。
- `ProcessIoBytes()`：取不到就回 `false`，bench 顯示 `n/a`。

`src/vfs.cpp` 還有三處 Win32 相依，移植時要補 `#else` 分支：

| 位置 | 用到什麼 | POSIX 對應 |
|---|---|---|
| `get_page_size()` | `GetSystemInfo` | `sysconf(_SC_PAGESIZE)` |
| `auto_truncate()` | `SetEndOfFile` | `ftruncate` |
| 四處 `min(...)` | windows.h 的 `min` 巨集 | 自己的 helper 或 `std::min` |
