# VFS — 遊戲資源封包虛擬檔案系統

把大量遊戲資源打包成兩個實體檔，對上層提供 POSIX 風格的檔案 API，
並附帶一套單元測試與效能量測框架。

## 這是什麼

把大量遊戲資源塞進兩個實體檔，對上層提供 POSIX 風格的檔案 API：

| 檔案 | 內容 |
|---|---|
| `<name>.paki` | 索引。IIO 多通道格式，magic `"AIO2"`，見「索引格式」 |
| `<name>.pak` | 實際資料區塊（512 bytes 一塊） |
| `<name>.lock` | 跨行程鎖，由 `lock_check` / `lock_leave` 管理 |

### 分層

| 層 | 前綴 | 職責 |
|---|---|---|
| L2 | `vfs_iio_*` | Interleaved I/O — 多通道 + LRU 頁快取，資料在 `.paki` |
| L3 | `vfs_data_*` | `.pak` 區塊讀寫 + 滑動視窗快取 |
| L4 | `vfs_fat_*` | File Allocation Table，區塊鏈 |
| L5 | `vfs_nt_*` | Node Table，16 byte 的 inode（refcount / size / chain / flags） |
| L6 | `vfs_dt_*` | Directory Table，Patricia Trie 檔名索引 + glob |
| L7 | `vfs_file_*` | `open` / `read` / `write` / `lseek` / `link` / `unlink` / `glob` |

`.paki` 的通道配置是固定的：Ch0 = DT TrieNode、Ch1 = DT KeyNode、Ch2 = NT、Ch3 = FAT。

## 建置

Visual Studio 2022、**Debug | x64**（Release 也可）。

```bash
MSBuild.exe VFS.vcxproj -p:Configuration=Release -p:Platform=x64 -nologo -verbosity:minimal -m:1
```

MSBuild 路徑會漂移，找不到就用 vswhere：

```bash
"/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" \
    -latest -products '*' -find "MSBuild\**\Bin\MSBuild.exe"
```

輸出在 `bin\x64\<Configuration>\vfs.exe`，中間檔在 `obj\`。

### 專案設定的兩個重點

- **`/utf-8`**：原始碼含 UTF-8 中文註解且多數沒有 BOM，不加這個 MSVC 會用系統 codepage 誤讀。
- **不能定義 `NOMINMAX`**：`vfs.cpp` 直接用 windows.h 的 `min` 巨集（四處），
  定義了會編不過。因此 `main.cpp` 與測試／bench 也避開 `std::min`。

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

封包內的鍵一律**小寫、`/` 分隔** —— 跨平台時檔名大小寫敏感度不一致是災難的來源，
統一在工具端正規化掉。
`add` / `addtree` 會自動正規化；查詢類指令先試原字串，找不到再試正規化後的。

---

## 測試

```bash
vfs test              # 全部跑
vfs test --list       # 看有哪些 suite
vfs test file dt      # 只跑指定 suite
vfs test -v           # 連已知缺陷的失敗細節也印出來
```

暫存封包預設放在系統暫存目錄下的 `vfstest/`，用 `VFS_TEST_TMPDIR` 可以改
（**建議指到有幾 GB 空間的磁碟**，大規模的測試與 bench 會吃不少空間）。

### suite

| suite | 涵蓋 |
|---|---|
| `file` | L7：讀寫往返、覆寫、lseek、append、unlink / link、fd 管理、glob、檔名、大量檔案、交錯操作 |
| `iio` | L2：通道配置、讀寫往返、快取淘汰、通道成長、錯誤參數 |
| `data` | L3：區塊往返、滑動視窗邊界、contiguous 等價性、EOF 補零 |
| `fat` | L4：鏈建立／延伸／縮減／銷毀、空間回收、get_nth |
| `nt` | L5：節點配置、欄位往返、refcount、持久化 |
| `dt` | L6：trie 插入／查詢／刪除、長檔名、glob 樣式比對、刪除的副作用 |
| `compat` | `.paki` 磁碟 magic 必須是 `"AIO2"` |
| `regress` | 五個已修缺陷的定點回歸案例 + 增量更新的變動量約束（見下） |

### 結果標記

| 標記 | 意思 | 影響退出碼 |
|---|---|---|
| `[ ok  ]` | 通過 | 否 |
| `[FAIL ]` | 失敗 | **是** |
| `[xfail]` | 已知缺陷，如預期失敗 | 否 |
| `[XPASS]` | 已知缺陷卻通過了 | **是** |

`XPASS` 是刻意設計的：修好某個 bug 之後，對應案例會從 `xfail` 變成 `XPASS`
並讓整輪失敗，逼你回去把 `VFS_TEST_XFAIL` 改回 `VFS_TEST`。

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

`CHECK_*` 失敗後繼續跑（一個案例可以回報多個問題），`REQUIRE_*` 失敗就中止該案例。
註冊是自動的，不需要改任何 dispatcher。

---

## Benchmark

```bash
vfs bench                                   # 全部 pattern
vfs bench --list                            # 看有哪些 pattern
vfs bench randread --scale 20               # 單一 pattern、接近正式封包的規模
vfs bench seqread --cold 16384              # 量測前把封包擠出 OS 快取（16GB ballast）

# 建立 baseline
vfs bench --repeats 3 --json bench-baseline.json
# 重構後檢查有沒有退步（退出碼非 0 = 有）
vfs bench --repeats 3 --compare bench-baseline.json
```

### pattern

| pattern | 量什麼 |
|---|---|
| `pack` | 打包速度：整體、分大小級距（小／中／大）、大量小檔、空間放大 |
| `seqread` | 循序讀（模擬載入關卡），連續封包 vs 碎片化封包 |
| `randread` | 隨機讀（模擬執行期零星取資源），IOPS 與 p50/p95/p99 延遲 |
| `write` | 新增 / 原地覆寫 / 增長，含磁碟成長比 |
| `open` | `vfs_start` 延遲、查名字延遲、glob 全表，對 500/2000/8000 檔各量一次 |
| `patch` | 增量更新造成的 `.pak`/`.paki` 變動比例 —— **直接對應 CDN 流量** |

語料由固定種子在記憶體中合成（`src/bench/bench_corpus.cpp`），不碰主機上的實體檔案：
跨平台、跨次數完全一致，也不會被來源磁碟的讀取速度污染。

### 讀數字時要注意的三件事

1. **預設規模量的是 CPU 成本，不是磁碟 I/O。**
   `--scale 1` 的封包約 157MB，整個塞得進 OS 的檔案快取，所以 2500 MB/s 這種數字
   反映的是 VFS 本身的開銷（FAT 鏈走訪、memcpy、查表），不是硬碟。
   這正是做 regression 想要的 —— 穩定、可重現。

   要量磁碟受限的行為有兩條路：`--scale 20` 以上（封包大到裝不進快取），
   或 `--cold <MB>`（量測前寫讀一個 ballast 檔把封包擠出快取）。
   `--cold` 要給到實體記憶體的 1~2 倍才擠得乾淨，每輪會多花不少時間。

   關於快取，要清的其實有兩層，性質完全不同：

   | 層 | 怎麼清 | 是否決定性 |
   |---|---|---|
   | VFS 自己的快取（IIO 的 LRU 頁、`.pak` 的滑動視窗） | 關檔重開 | 是，**每個 pattern 都一定會做** |
   | OS 的檔案快取 | `--cold`（ballast 壓力法） | 否，盡力而為 |

   標準 C++ 沒有「丟棄某個檔案的快取」這種 API（Windows 要 `FILE_FLAG_NO_BUFFERING`、
   Linux 要 `posix_fadvise`，都不可攜），所以第二層只能用壓力法。
   清除的時機是「填完語料之後、開始量之前」—— 在輪次開頭清沒有用，
   因為封包就是那之後才寫出去的。

   兩者的差距非常大。同一份程式碼、同一個封包，`randread`：

   | | IOPS | p99 延遲 |
   |---|---|---|
   | warm（預設） | 56030 | 0.073 ms |
   | `--cold 4096` | 494 | 23.4 ms |

   **113 倍。** warm 那組量的是 VFS 的 CPU 成本，cold 那組才是玩家開機後
   第一次載入的真實體驗。兩個數字都有用，但不能混為一談 ——
   做 regression 用 warm（穩定可重現），評估實際載入時間用 cold。

2. **`--repeats` 取的是「最好的一次」，不是中位數。**
   中位數看起來公正，實際上會把背景干擾一起平均進來。這些 pattern 每輪要寫上百 MB，
   OS 的延遲寫回、其他行程、熱節流都只會讓數字變差，不會讓它變好。
   實測用中位數時，同一份程式碼連跑兩次會出現 −8% 與 −33% 兩種結果，門檻根本沒法用。
   取最佳值 = 取干擾最少的那一輪，這是 benchmark 的標準做法。

3. **`(參考)` 標記的指標不納入退步判定。**
   `lat_max_ms` 這種單一樣本統計本來就會抖，拿來當門檻只會製造假警報。
   真正該盯緊的是計數與比例類指標（`space_amplification`、`*_changed_pct`、
   `files_failed`），它們是決定性的，一動就是真的有東西變了。

比對前兩邊的 `--repeats` / `--scale` / `--cold` 要一致，否則比到的是雜訊。
預設容差 25%，用 `--tolerance` 調整。

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

---

## 已知缺陷

目前沒有已知的未修正缺陷。下面五項都已修好，案例保留在
`src/test/test_regress.cpp` 當回歸保護 —— 每一項都附成因說明與可重現的定點案例。

| # | 缺陷 | 修法 |
|---|---|---|
| 1 | 檔案大小 `% 512 == 1` 時寫入失敗 | `vfs_file_write` 改成傳「寫完後的大小」而非「最後一個 byte 的索引」 |
| 2 | 檔案數超過 32767 後 `nt_idx` 溢位 | TrieNode 改成 20 bytes，`nt_idx`/`b_index` 升成 int，magic 換成 `"AIO2"` |
| 3 | 新建 `.pak` 含未初始化堆積記憶體 | `cache_create` / `cache_resize` 補上 fread 讀不滿時的補零 |
| 4 | 刪除一個鍵會連帶毀掉另一個鍵 | 補上 PATRICIA 刪除該有的「鍵搬移」與 upward link 重新指向 |
| 5 | `lseek` 擴展出的空洞沒清零 | 擴展邏輯抽成 `file_grow`，會清掉真正成為空洞的區塊 |

幾個值得記住的細節：

- **缺陷 1** 的成因是呼叫端把「索引」當「大小」傳。`nblocks(size-1)` 只在
  `size % 512 == 1` 時比 `ceil(size/512)` 少一塊。`vfs_file_lseek` 與 `file_truncate`
  本來就都把參數當大小算 —— 錯的是呼叫端，不是 lseek。

- **缺陷 3** 的對照組是 `cache_slide`：它一直都有補零，`cache_create` 純粹是漏掉。
  IIO 層的 `read_absolute_block_n` 也有做，所以 `.paki` 從來沒有這個問題。

- **缺陷 4** 是 PATRICIA 刪除少了兩個步驟。這個結構裡每個節點同時是一個位元測試
  和一把鍵，N 把鍵就是 N 個節點。刪掉一把鍵之後：失去結構意義的是 `parent`，
  失去鍵的是 `current`，但 `parent` 手上那把鍵還活著。正確做法是
  **把 parent 的鍵搬進 current，再回收 parent**；而且鍵搬家之後，
  **原本指向 parent 的那條 upward link 也要改指到 current**，
  否則會留下懸空指標（`p_relink_upward`）。舊版兩步都沒做，直接把 parent 的鍵
  `fnode_free` 掉，於是那把鍵人間蒸發。受害者不是文字上的前綴，而是
  「剛好被存在那個內部節點裡的鍵」，所以從外面看像是刪一個檔就隨機少掉另一個。

  驗證靠 `dt/random_insert_delete_matches_model`：拿 `std::set` 當參考模型，
  4 組種子各跑 1200 次隨機插入／刪除，每 50 步全量對拍一次。

- **缺陷 5** 的修正刻意帶了一個 `zero_until` 參數。`vfs_file_write` 擴展完會立刻
  把整段新空間覆寫掉，先補零等於寫兩遍，打包的寫入量會整整翻倍。所以 `write`
  只要求「舊 EOF 到寫入起點」那段補零，`lseek` 才要求整段都補 —— 因為它的
  呼叫端不保證會寫。

## 索引格式

`.paki` 標頭的 magic 是 `"AIO2"` (0x324F4941)。TrieNode 20 bytes（全 `int`），檔案數上限約 21 億。

舊的 `"AIOD"` 格式（TrieNode 16 bytes，`short` nt_idx / b_index，上限 32767）已不再支援：
`read_header` 遇到它會直接失敗。舊封包要從原始檔案用 `addtree` 重新打包。

用換 magic 而不是加版本欄位，是為了讓**只認得舊格式的讀取器遇到新封包直接拒絕開啟**，
而不是照舊佈局誤讀出一堆垃圾。

磁碟上的 TrieNode 記錄與記憶體中的 `VfsTrieNode` 佈局完全相同（`static_assert` 釘住 20 bytes），
`trienode_get` / `trienode_set` 直接整筆讀寫，不做逐欄轉換。

## 可調參數

都是全域變數，必須在 `vfs_start` 之前設定：

| 變數 | 預設 | 說明 |
|---|---|---|
| `vfs_iio_CACHE_PAGES` | 64 | IIO 每個通道的 LRU 快取上限（頁數）。預設四個通道合計約 4MB |
| `vfs_data_CACHE_BYTES` | 65536 | `.pak` 資料層滑動視窗的大小 |

另外 `vfs.h` 有一組 `vfs_stat_*` 診斷計數器（FAT 讀寫次數、`next_free` 掃描步數、
`.pak` 視窗滑動次數…），配合 `vfs bench -v` 使用。這一層的效能問題幾乎都是
「某個迴圈做了比預期多三個數量級的次數」，看次數比看時間準得多 ——
FAT 配置器的瓶頸就是這樣抓到的。

`vfs_iio_CACHE_PAGES` 原本是寫死的 8（四通道合計 512KB）。那是 2000 年代的預算：
3GB 封包光 FAT 表就有 25MB，8 頁的命中率低到每走一步都要打一次磁碟。
提到 64 之後，8000 個檔的隨機查名字從 3.9µs 降到 0.53µs（**快 7.4 倍**），
隨機讀 IOPS +37%。這只花記憶體，不影響封包格式。

## 修正前後的效能對照

`bench-original.json` 是**這一輪修正之前**的程式碼跑出來的基準，留著當歷史對照。想重看差異：

```bash
vfs bench --repeats 3 --compare bench-original.json --tolerance 15
```

| 指標 | 原版 | 目前 | 變化 |
|---|---|---|---|
| **write/overwrite** | **33.1 MB/s** | **520.6** | **+1476%** |
| 查名字 @8000 檔 | 3.861 µs | 0.487 | **−87%** |
| glob 全表 @8000 檔 | 7.65 ms | 3.71 | **−52%** |
| 查名字 @2000 檔 | 0.898 µs | 0.404 | −55% |
| 查名字 @500 檔 | 0.749 µs | 0.363 | −52% |
| randread IOPS | 47,879 | 61,820 | **+29%** |
| randread p99 延遲 | 0.0808 ms | 0.0676 | −16% |
| pack/many_small 檔/s | 76,976 | 86,780 | +13% |
| seqread（碎片化） | 2,457 MB/s | 2,867 | +17% |
| seqread（連續） | 2,841 MB/s | 2,915 | +3% |
| pack/all | 629 MB/s | 632 | 持平 |
| write/create | 613 MB/s | 605 | −1% |
| **write/grow** | **543.8 MB/s** | **491** | **−10%** |
| patch delta_mb | 1.152 MB | 1.156 | +0.3% |

除了 `write/grow`，其餘不是持平就是變快。`patch delta_mb` 的 +0.3% 是
TrieNode 從 16 加大到 20 bytes 的代價 —— 換掉 32767 個檔案的上限，patch 多付 0.3~0.6% 的流量。

### write/grow 的 −90% 是怎麼回事

同一輪比對裡還有這兩個數字：

| | 原版 | 目前 |
|---|---|---|
| `write/grow.files_intact` | **190 / 944** | **944 / 944** |
| `seqread/fragmented.files_available` | **472 / 944** | **944 / 944** |
| `write/grow.disk_growth_ratio` | 1.035 | 0.346 |

原版跑完 `write/grow` 之後，944 個檔案只剩 **190 個讀得到** —— 80% 被缺陷 4
（刪除會連帶毀掉別的鍵）吃掉了。檔案被毀之後，後續的 `WriteAll` 查不到舊鍵，
於是「覆寫」變成「建立新檔」：循序追加、不用回收區塊、數字漂亮。

544 MB/s 量的是「一邊掉資料一邊追加」的速度。53.7 MB/s 才是正確語意下的真實成本
（`disk_growth_ratio` 從 1.035 掉到 0.346 也印證了：現在區塊真的被回收重用）。

## FAT 配置器：從線性掃描換成空閒點陣圖

修好缺陷 4 之後，「原地覆寫」這條路徑的真實成本才露出來：**33 MB/s，
對比循序建立的 600 MB/s —— 18 倍落差**。而原地覆寫正是增量更新的主要工作模式。

這個落差**原版就有**（原版的 overwrite 也是 33.05 MB/s），只是以前被缺陷 4 蓋住：
一半的覆寫偷偷變成了追加。

### 用計數器找出真兇

猜不如量。`vfs_stat_*` 這組計數器（見 `vfs.h`）加上 `vfs bench write -v`：

| | create | overwrite |
|---|---|---|
| 邏輯資料 | 156.8 MB | 49.2 MB |
| **`next_free` 掃描步數** | **321,643** | **93,644,169** |
| 每個區塊平均步數 | 1.0 | **928** |
| `.pak` 視窗滑動 | 2,512 | 863 |

每配置一個區塊要掃 **928 個 FAT 表項**，每一步都是一次 IIO 讀。49MB 的工作量
花掉 9400 萬次 FAT 讀取。視窗滑動次數反而更少 —— 所以瓶頸完全不在資料層。

成因：`next_free()` 是逐格線性掃描。吃完被釋放的那一段之後，掃描必須一路走過
**整張 FAT** 才找得到下一塊空地，等於每個檔案一次全表掃描，成本隨封包大小線性惡化。

### 修法

`VfsFatHandle` 加一張空閒表項點陣圖（純記憶體，不寫進封包）：

- **1 bit 對應一個表項**，1 = 空閒。3GB 封包（630 萬個區塊）約佔 786KB
- **延遲建立**：只有真的要配置區塊時才掃一次 FAT 把它建起來。
  遊戲執行期只讀不寫，完全不用付這個成本
- **一致性**：`node_set_value` 是 FAT 唯一的寫入點，點陣圖跟著它走就永遠是對的
- **找空位**：記憶體裡的 word 掃描，而不是逐格 IIO 讀
- 點陣圖建不起來（記憶體不足）時會退回原本的線性掃描，只是慢，不會壞

順帶修掉一件事：`vfs_fat_open` 原本開檔時就呼叫 `next_free(fat, 0)`，
等於**每次開封包都線性掃過整張已用的 FAT**。3GB 封包的 FAT 有 25MB，
那是一筆純粹浪費的開檔成本，而且遊戲執行期根本用不到。現在改成延遲建立。

### 結果

| | 修正前 | 修正後 |
|---|---|---|
| overwrite `next_free` 步數 | 93,644,169 | **0** |
| overwrite FAT 讀取次數 | 94,148,164 | **403,133**（−99.6%） |
| **overwrite 吞吐** | **33.1 MB/s** | **520 MB/s** |
| **grow 吞吐** | **53.7 MB/s** | **480 MB/s** |
| create 吞吐 | 594 MB/s | 605（持平） |

18 倍落差消失了：覆寫 520 vs 建立 605 MB/s，基本上是同一個量級。

## 增量更新（為什麼值得做）

`patch` pattern 量到的數字說明了一件事：**這個格式本身對 delta 很友善，
問題出在「每次都整包重打」的流程。**

| 做法 | `.pak` 變動 |
|---|---|
| 原地更新一個檔 | ~0.01% |
| 原地新增檔案 | ~0.01% + 尾端追加 |
| 整包重打（有一個檔大小變動且非對齊） | **~54%** |

整包重打時，只要有一個檔案的大小變動不是區塊對齊的，後面所有資料就整體位移，
二進位 diff 隨即失效 —— 玩家因此要重下整個 3GB。改用原地增量更新即可避開。

（前提是缺陷 3 先修好：`.pak` 帶著未初始化記憶體就不可能有位元級可重現的打包結果。）

---

## 可攜性

測試與 benchmark 框架只用**標準 C 與 C++17**，沒有任何 OS 標頭、沒有第三方相依，
目標平台是 Windows / Linux / Steam Deck / macOS / Android / iOS / Switch / PS5。

| 檔案 | 角色 |
|---|---|
| `inc/vfs_platform.h` | 可攜層介面。不含任何 OS 標頭 |
| `src/platform/vfs_platform.cpp` | 實作。全部走 `<cstdio>` / `<chrono>` / `<filesystem>` |

整個框架裡唯一的平台分支是 `FileSeekAbs` / `FileTell` 的那個 `#if`
（標準 `fseek` 的 offset 是 `long`，在 Windows 上只有 32-bit，撐不住 3GB 封包；
`_fseeki64` 與 `fseeko` 都由 `<cstdio>` 提供，不需要 OS 標頭）。

移植到主機平台時要看的只有兩處：

- `ScratchDir()` —— 主機的可寫區通常要走 SDK 的掛載點，設環境變數 `VFS_TEST_TMPDIR` 即可，不用改程式。
- `ProcessIoBytes()` —— 取不到就回 `false`，bench 會標成 `n/a` 而不是填一個假數字。

### 核心層還沒完成的部分

`inc/vfs.h` 已經不再 include `windows.h`（介面是可攜的），但 `src/vfs.cpp` 內部
還有三處 Win32 相依，移植時各補一個 `#else` 分支即可：

| 位置 | 用到什麼 | POSIX 對應 |
|---|---|---|
| `get_page_size()` | `GetSystemInfo` | `sysconf(_SC_PAGESIZE)` |
| `auto_truncate()` | `SetEndOfFile` | `ftruncate` |
| 四處 `min(...)` | windows.h 的 `min` 巨集 | 自己的 helper 或 `std::min` |

## 這一輪做了什麼

這份程式碼的起點是一份從舊專案繼承來的實作。目前與那個起點的差異：

**可攜性**

1. `vfs.h` 不再 include `windows.h`；`get_page_size` 的回傳型別改為 `unsigned long`
2. `vfs.cpp` 的 Win32 呼叫收進 `#if defined(_WIN32)`

**缺陷修正**（每一項都有可重現的回歸案例，見 `src/test/test_regress.cpp`）

3. `vfs_file_write`：擴展檔案時傳「寫完後的大小」而非「最後一個 byte 的索引」（缺陷 1）
4. `cache_create` / `cache_resize`：`fread` 讀不滿時補零（缺陷 3）
5. `p_remove_key`：補上 PATRICIA 刪除該有的鍵搬移，並新增 `p_relink_upward`
   把指向被回收節點的 upward link 改指到新位置（缺陷 4）
6. `file_grow(handle, fh, new_size, zero_until)` 取代 `vfs_file_lseek` 裡的擴展區塊；
   新配置到的空洞區塊會補零（缺陷 5）

**格式**

7. `.paki` magic 換成 `"AIO2"`（`VFS_IIO_MAGIC`），TrieNode 記錄改成 20 bytes；
   舊的 `"AIOD"` 格式不再支援（缺陷 2）
8. `VfsTrieNode` 的 `nt_idx` / `b_index` 由 `short` 升成 `int`

**效能**

9. TrieNode 的欄位存取器改走整筆記錄；`trienode_get` / `trienode_set` 直接讀寫 `VfsTrieNode`
10. `p_lookup_key` / `p_insert_key` / `p_node_iterate` 改成每個節點只讀一次記錄
11. FAT 配置器換成空閒點陣圖（`VfsFatHandle::free_bits`），
    `vfs_fat_open` 不再於開檔時做全表掃描
12. `vfs_iio_CACHE_PAGES` 取代寫死的 8（預設 64）、
    `vfs_data_CACHE_BYTES` 取代寫死的 64KB（預設值不變）
13. `vfs_stat_*` 診斷計數器與 `vfs_stat_reset()`

## 接下來

| 項目 | 為什麼 |
|---|---|
| `vfs.cpp` 最後 3 處 Win32 相依補 POSIX 分支 | 全平台可編；現在有測試能驗 |
| 增量更新工具（manifest + 原地 patch） | 把整包重下變成只下改動的部分 |

增量更新的技術前提現在都到位了：原地覆寫夠快（520 MB/s）、刪除不會掉資料、
打包位元級可重現、patch 變動量已經有指標在盯（`vfs bench patch`）。
