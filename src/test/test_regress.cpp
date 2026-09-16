// test_regress.cpp — 已知缺陷與架構上限的回歸案例
//
// 這裡每一個 VFS_TEST_XFAIL 都是實測確認過的問題。標成 XFAIL 的意思是
// 「現在必定失敗，而且我們知道為什麼」：
//   - 失敗 → 報 [xfail]，不影響整輪結果
//   - 意外通過 → 報 [XPASS] 並讓整輪失敗，逼人回來把標記拿掉
//
// 修好 bug 的流程就是：改程式 → 這裡變 XPASS → 把 VFS_TEST_XFAIL 改回 VFS_TEST。

#include <vfs_test.h>
#include <vfs_testutil.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using vfsutil::Blob;
using vfsutil::ReadAll;
using vfsutil::WriteAll;
using vfsutil::FileSize;
using vfsutil::ListKeys;
using vfsutil::TempArchive;

// ================================================================ BUG 1
//
// 檔案大小 % 512 == 1 時寫入必定失敗。
//
// 成因：vfs_file_write 把「最後一個 byte 的索引」(size-1) 當成位置傳給
// vfs_file_lseek，lseek 再對這個索引呼叫 nblocks()：
//     needed = nblocks(size - 1) = ceil((size-1)/512)
// 但實際需要的是 ceil(size/512)。兩者只在 size % 512 == 1 時差一個區塊，
// 於是 FAT 鏈少配一塊，寫到最後那 1 byte 時鏈已到底 → vfs_errno = VFS_ERR_FAT_ALLOC。
//
// 影響：每 512 個檔就有 1 個被靜默丟掉（實測 1690 個檔的語料掉了 3 個）。
//       3GB 封包裡大約有數十個檔案根本沒被打包進去。
//
// 【已修正】vfs_file_write 改成傳「寫完後的檔案大小」而非「最後一個 byte 的
// 索引」。lseek 與 file_truncate 本來就把參數當大小算（ceil(size/512)），
// 錯的是呼叫端。案例保留為回歸保護。

VFS_TEST(regress, write_size_mod512_eq_1) {
	TempArchive a("bug1");
	REQUIRE(a.Ok());

	const int sizes[] = { 513, 1025, 2049, 4097, 8193, 65537 };
	for (int sz : sizes) {
		const std::string key = "b1/" + std::to_string(sz) + ".bin";
		const std::vector<char> data = Blob(static_cast<size_t>(sz), static_cast<unsigned>(sz));

		CHECK_MSG(WriteAll(a.h(), key, data),
			("寫入 " + std::to_string(sz) + " bytes（% 512 == 1）").c_str());
		CHECK_EQ(FileSize(a.h(), key), sz);

		std::vector<char> back;
		CHECK_MSG(ReadAll(a.h(), key, &back), ("讀回 " + std::to_string(sz) + " bytes").c_str());
		CHECK_BLOB_EQ(back, data);
	}
}

// 對照組：相鄰的大小都正常，證明問題確實只在 % 512 == 1
VFS_TEST(regress, write_sizes_around_mod512_boundary_ok) {
	TempArchive a("bug1_ctrl");
	REQUIRE(a.Ok());

	const int sizes[] = { 511, 512, 514, 1023, 1024, 1026, 2047, 2048, 2050 };
	for (int sz : sizes) {
		const std::string key = "ctrl/" + std::to_string(sz) + ".bin";
		const std::vector<char> data = Blob(static_cast<size_t>(sz), static_cast<unsigned>(sz));
		CHECK_MSG(WriteAll(a.h(), key, data), ("寫入 " + std::to_string(sz)).c_str());
		std::vector<char> back;
		CHECK_MSG(ReadAll(a.h(), key, &back), ("讀回 " + std::to_string(sz)).c_str());
		CHECK_BLOB_EQ(back, data);
	}
}

// ================================================================ BUG 2
//
// 超過 32767 個檔案之後靜默損毀。
//
// 成因：DT 的 TrieNode 用 short 存 NT 索引（VfsTrieNode::nt_idx），
// vfs_file_create 寫入時 static_cast<short>(fh->nt_node_idx)（src/vfs.cpp:5117）
// 直接截斷。第 32768 個檔案起，nt_idx 變成負數或指到別人的節點。
//
// 症狀特別惡劣：info 照樣回報 40000 個檔、glob 照樣列得出來，
// 但 open/read 一律回 "File not found"。備份跟驗證流程都抓不到。
//
// 實測：40000 個檔的封包，f32767 之前全部正常，f32768 之後全部讀不到。
//
// 【已修正】TrieNode 改成 20 bytes，nt_idx 與 b_index 都升成 int，
// magic 由 "AIOD" 換成 "AIO2"。舊的 16 bytes 佈局已不再支援。
//
// 這個案例用 33000 個檔跑，剛好越過舊上限又不會拖慢每日測試。

VFS_TEST(regress, more_than_32767_files) {
	TempArchive a("bug2");
	REQUIRE(a.Ok());

	const int kN = 33000;
	char key[64];

	int write_failed = 0;
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "f%05d", i);
		// 內容就是索引本身，讀回來能直接驗出「讀到別人的資料」
		std::vector<char> data(64);
		std::snprintf(data.data(), data.size(), "id=%d", i);
		if (!WriteAll(a.h(), key, data)) ++write_failed;
	}
	CHECK_MSG(write_failed == 0,
		("有 " + std::to_string(write_failed) + " 個檔寫入失敗").c_str());

	// 關鍵斷言：列得出來的檔案必須讀得到，而且內容要是自己的
	int unreadable = 0, wrong_content = 0;
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "f%05d", i);
		std::vector<char> back;
		if (!ReadAll(a.h(), key, &back)) { ++unreadable; continue; }
		char expect[64];
		std::snprintf(expect, sizeof(expect), "id=%d", i);
		if (back.size() < std::strlen(expect) ||
		    std::memcmp(back.data(), expect, std::strlen(expect)) != 0) ++wrong_content;
	}
	CHECK_MSG(unreadable == 0,
		("有 " + std::to_string(unreadable) + " / " + std::to_string(kN) + " 個檔讀不到").c_str());
	CHECK_MSG(wrong_content == 0,
		("有 " + std::to_string(wrong_content) + " 個檔讀到別人的內容").c_str());
	CHECK_EQ(static_cast<int>(ListKeys(a.h()).size()), kN);
}

// 對照組：32767 以內必須完全正常
VFS_TEST(regress, files_below_32767_limit_ok) {
	TempArchive a("bug2_ctrl");
	REQUIRE(a.Ok());

	const int kN = 4000;
	char key[64];
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "f%05d", i);
		std::vector<char> data(64);
		std::snprintf(data.data(), data.size(), "id=%d", i);
		REQUIRE(WriteAll(a.h(), key, data));
	}
	int bad = 0;
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "f%05d", i);
		std::vector<char> back;
		char expect[64];
		std::snprintf(expect, sizeof(expect), "id=%d", i);
		if (!ReadAll(a.h(), key, &back) ||
		    std::memcmp(back.data(), expect, std::strlen(expect)) != 0) ++bad;
	}
	CHECK_MSG(bad == 0, ("有 " + std::to_string(bad) + " 個檔不正確").c_str());
}

// ================================================================ BUG 3
//
// .pak 開頭被寫入未初始化的堆積記憶體。
//
// 成因：cache_create(VfsDataHandle*)（src/vfs.cpp 約 1985 行）malloc 了 64KB
// 之後直接 fread 進去，新建的 .pak 是空檔 → fread 讀到 0 bytes → 整個 64KB
// 緩衝區維持未初始化。之後 cache_flush 會把「整個」緩衝區寫出去。
// 對照組是 cache_slide，它有做 memset 補零 —— 所以這純粹是漏掉。
//
// 兩個後果，都很嚴重：
//   1. 安全性：堆積內容（實測含 heap 指標）被散佈到每個玩家的機器上。
//   2. 可重現性：同樣的輸入打包兩次得到不同的 .pak，delta patch 系統的地基沒了。
//
// 實測：同樣語料打包兩次，.pak 第 0 塊有 8 bytes 不同，內容是 0x1d352eb0150
//       這種堆積位址。
//
// 【已修正】cache_create 與 cache_resize 都補上「fread 讀不滿就 memset 補零」。
// IIO 層的 read_absolute_block_n 本來就有做，所以 .paki 一直是乾淨的。
// 兩個案例都保留：byte 級可重現性是 delta patch 的地基，不能再退回去。

VFS_TEST(regress, pack_is_byte_reproducible) {
	// 同樣的內容、同樣的順序，打包兩次應該得到位元組完全相同的封包。
	// 這是任何 delta patch 方案的前提。
	auto build = [](TempArchive* a) {
		for (int i = 0; i < 40; ++i) {
			char key[64];
			std::snprintf(key, sizeof(key), "r/%03d.bin", i);
			if (!WriteAll(a->h(), key,
			              Blob(static_cast<size_t>(700 + i * 37), static_cast<unsigned>(i))))
				return false;
		}
		a->Close();
		return true;
	};

	TempArchive a1("repro1");
	TempArchive a2("repro2");
	REQUIRE(a1.Ok());
	REQUIRE(a2.Ok());
	REQUIRE(build(&a1));
	REQUIRE(build(&a2));

	long long total = 0, grew = 0;
	const long long pak_diff =
		vfsutil::DiffFileBlocks(a1.base() + ".pak", a2.base() + ".pak", 4096, &total, &grew);
	REQUIRE_GE(pak_diff, 0);
	CHECK_MSG(pak_diff == 0,
		(".pak 有 " + std::to_string(pak_diff) + " / " + std::to_string(total) +
		 " 個區塊在兩次打包之間不同（內容完全一樣）").c_str());
	CHECK_EQ(grew, 0LL);

	const long long paki_diff =
		vfsutil::DiffFileBlocks(a1.base() + ".paki", a2.base() + ".paki", 4096, &total, &grew);
	REQUIRE_GE(paki_diff, 0);
	CHECK_MSG(paki_diff == 0,
		(".paki 有 " + std::to_string(paki_diff) + " / " + std::to_string(total) +
		 " 個區塊在兩次打包之間不同").c_str());
}

// 更直接地指出問題所在：新建的空封包，.pak 開頭應該是全 0
VFS_TEST(regress, fresh_pak_header_is_zeroed) {
	TempArchive a("zeroed");
	REQUIRE(a.Ok());
	// 寫一個小檔逼快取刷出去，然後關檔
	REQUIRE(WriteAll(a.h(), "x.bin", Blob(100, 1)));
	a.Close();

	std::FILE* fp = std::fopen((a.base() + ".pak").c_str(), "rb");
	REQUIRE(fp != nullptr);
	unsigned char head[512] = {};
	const size_t got = std::fread(head, 1, sizeof(head), fp);
	std::fclose(fp);
	REQUIRE_EQ(got, sizeof(head));

	// 區塊 0 不屬於任何檔案（FAT 鏈從 1 開始），應該全是 0
	int nonzero = 0;
	int first_nonzero = -1;
	for (size_t i = 0; i < sizeof(head); ++i) {
		if (head[i] != 0) { ++nonzero; if (first_nonzero < 0) first_nonzero = static_cast<int>(i); }
	}
	CHECK_MSG(nonzero == 0,
		(".pak 區塊 0 有 " + std::to_string(nonzero) + " 個非零 byte，首個在 offset " +
		 std::to_string(first_nonzero) + "（未初始化的堆積記憶體外洩到封包裡）").c_str());
}

// ================================================================ 增量更新
//
// 這幾個案例不是 bug，是把「為什麼玩家要重下 3GB」用測試固定下來。
// 之後任何重構都不能讓這些數字變差。

VFS_TEST(regress, in_place_update_touches_few_blocks) {
	// 原地更新一個檔案，.pak 的變動量必須是「小」的。
	// 這是增量更新方案能成立的基礎 —— 實測是 0.01% 等級。
	TempArchive a("inplace");
	REQUIRE(a.Ok());

	for (int i = 0; i < 300; ++i) {
		char key[64];
		std::snprintf(key, sizeof(key), "d/%03d.bin", i);
		REQUIRE(WriteAll(a.h(), key, Blob(20000, static_cast<unsigned>(i))));
	}
	REQUIRE(a.Snapshot("before"));

	// 換掉其中一個檔（大小不變）
	REQUIRE(WriteAll(a.h(), "d/150.bin", Blob(20000, 999)));
	a.Close();

	long long total = 0, grew = 0;
	const long long diff = a.DiffVsSnapshot("before", false, &total, &grew);
	REQUIRE_GE(diff, 0);
	REQUIRE_GT(total, 0LL);

	const double pct = 100.0 * static_cast<double>(diff) / static_cast<double>(total);
	CHECK_MSG(pct < 5.0,
		("原地更新一個檔卻改動了 .pak 的 " + std::to_string(pct) + "%（" +
		 std::to_string(diff) + " / " + std::to_string(total) + " 個區塊）").c_str());
}

VFS_TEST(regress, adding_file_does_not_rewrite_existing_data) {
	// 新增檔案應該只是往尾端追加，不該把既有資料整個往後推。
	// 整包重打之所以會產生 50%+ 的差異，正是因為位移；
	// 增量新增沒有這個問題，這個案例把它釘住。
	TempArchive a("append_only");
	REQUIRE(a.Ok());

	for (int i = 0; i < 200; ++i) {
		char key[64];
		std::snprintf(key, sizeof(key), "d/%03d.bin", i);
		REQUIRE(WriteAll(a.h(), key, Blob(10000, static_cast<unsigned>(i))));
	}
	REQUIRE(a.Snapshot("before"));

	REQUIRE(WriteAll(a.h(), "d/zzz_new.bin", Blob(150000, 4242)));
	a.Close();

	long long total = 0, grew = 0;
	const long long diff = a.DiffVsSnapshot("before", false, &total, &grew);
	REQUIRE_GE(diff, 0);
	REQUIRE_GT(total, 0LL);

	const double pct = 100.0 * static_cast<double>(diff) / static_cast<double>(total);
	CHECK_MSG(pct < 5.0,
		("新增一個檔卻改動了既有 .pak 的 " + std::to_string(pct) + "%（" +
		 std::to_string(diff) + " / " + std::to_string(total) + " 個區塊）").c_str());
}

// ================================================================ BUG 4
//
// 刪除一個鍵會連帶毀掉另一個還活著的鍵。
//
// 成因：Patricia trie 的刪除演算法沒做「鍵搬移」。
// p_remove_key（src/vfs.cpp）找到持有目標鍵的節點 current 與內部節點 parent 後：
//   - 步驟 6 把 current 的 k_index 清成 0
//   - 步驟 7a 對 parent 做 fnode_free(parent 的 k_index) 然後 trienode_recover(parent)
// 但 parent 持有的是「另一把還在使用中的鍵」。標準 PATRICIA 刪除必須先把
// parent 的鍵（k_index 與 nt_idx）搬進 current 的位置，再把 parent 從樹上拆掉。
// 這份實作直接把它釋放了 → 那把鍵從此查不到，對應的 NT 節點與 FAT 鏈也成了孤兒。
//
// 受害者不一定是文字上的前綴 —— 是「剛好被存在那個內部節點裡的鍵」，
// 所以從外部看起來像是隨機有檔案消失。實測：
//   刪 "a"   → "ab" 消失          刪 "c/1"  → "c/10" 消失
//   刪 "ab"  → "abc" 消失         刪 "a/b"  → "a/b/c" 消失
//   200 個 "c/0".."c/199" 刪偶數項 → 刪到一半時 "c/8" 已經不見了
//
// 影響：任何會刪檔的流程都可能靜默掉資料 —— 增量更新、覆寫（WriteAll 會先 unlink）、
//       CLI 的 delete。遊戲資源命名（hero / hero_idle、map/1 / map/10）天生就踩得到。
//
// 【已修正】補上 PATRICIA 刪除該有的兩個步驟：
//   1. 把 parent 手上那把鍵（k_index + nt_idx）搬進 current 的記錄，
//      再回收 parent —— 而不是直接把它 fnode_free 掉。
//   2. 鍵搬家之後，原本指向 parent 的那條 upward link 也要改指到 current
//      （p_relink_upward）。少了這一步會留下懸空指標：該鍵查不到，
//      而且那個節點被重用之後還會查到別人的資料。
//
// 驗證靠 dt/random_insert_delete_matches_model —— 拿 std::set 當參考模型，
// 4 組種子各跑 1200 次隨機插入／刪除，每 50 步全量對拍一次。

VFS_TEST(regress, delete_preserves_other_keys) {
	// 最小重現：三個互為前綴的鍵，刪中間那個
	TempArchive a("bug4");
	REQUIRE(a.Ok());

	REQUIRE(WriteAll(a.h(), "a", Blob(100, 1)));
	REQUIRE(WriteAll(a.h(), "ab", Blob(200, 2)));
	REQUIRE(WriteAll(a.h(), "abc", Blob(300, 3)));

	REQUIRE_EQ(vfs_file_unlink(a.h(), "a"), 0);

	CHECK_EQ(vfs_file_exists(a.h(), "a"), 0);
	CHECK_MSG(vfs_file_exists(a.h(), "ab") != 0, "刪 \"a\" 連帶害死了 \"ab\"");
	CHECK_MSG(vfs_file_exists(a.h(), "abc") != 0, "刪 \"a\" 連帶害死了 \"abc\"");

	std::vector<char> back;
	CHECK_MSG(ReadAll(a.h(), "ab", &back), "\"ab\" 讀不到了");
	CHECK_BLOB_EQ(back, Blob(200, 2));
}

// ================================================================ BUG 5
//
// seek 超過檔尾造成的空洞沒有補零，讀到的是已刪除檔案的殘留內容。
//
// 成因：vfs_file_lseek 擴展檔案時只呼叫 vfs_fat_chain_extend 配置區塊，
// 沒有把新配到的區塊清成 0。那些區塊來自空閒串列，裡面裝的是前一個
// 檔案被刪除前的資料 —— 於是舊內容原封不動地出現在新檔案的空洞裡。
//
// POSIX 的 sparse file 語意要求空洞讀出 0。這裡不只是語意不合，
// 更是資訊外洩：被刪掉的資源內容會回到新檔案的讀者手上。
//
// 實測：寫一個 200KB 全 'S' 的檔、刪掉、再建一個有 100KB 空洞的稀疏檔，
//       空洞裡 99996 / 99996 bytes 全部是 'S'。
//
// 注意這個 bug 是非決定性的 —— 全新封包的區塊碰巧是 0 就看不出來。
// file/lseek_past_eof_extends_and_zero_fills 那個案例刻意先把封包弄髒，
// 才能穩定重現。
//
// 【已修正】擴展檔案的邏輯抽成 file_grow(handle, fh, new_size, zero_until)：
//   - 新配置到的區塊，落在空洞範圍內的一律寫 0
//   - 舊 EOF 所在區塊的尾巴（例如大小 100 的檔，區塊 0 的 100..511）也要清
//
// zero_until 參數是為了不讓打包變慢：vfs_file_write 擴展完會立刻把整段新空間
// 覆寫掉，先補零等於寫兩遍。所以 write 只要求「舊 EOF 到寫入起點」那段補零，
// lseek 才要求整段都補 —— 因為它的呼叫端不保證會寫。

VFS_TEST(regress, extend_does_not_leak_deleted_data) {
	TempArchive a("bug5");
	REQUIRE(a.Ok());

	// 先在空閒串列裡留下可辨識的殘骸
	std::vector<char> secret(300000, 'S');
	REQUIRE(WriteAll(a.h(), "old/deleted.bin", secret));
	REQUIRE_EQ(vfs_file_unlink(a.h(), "old/deleted.bin"), 0);

	// 新檔案先寫 4 bytes，再跳到 200000 寫 4 bytes
	const int fd = vfs_file_create(a.h(), "new/sparse.bin");
	REQUIRE_GE(fd, 0);
	REQUIRE_EQ(vfs_file_write(a.h(), fd, "HEAD", 4), 4);
	REQUIRE_EQ(vfs_file_lseek(a.h(), fd, 200000, 0), 0);
	REQUIRE_EQ(vfs_file_write(a.h(), fd, "TAIL", 4), 4);
	vfs_file_close(a.h(), fd);

	std::vector<char> back;
	REQUIRE(ReadAll(a.h(), "new/sparse.bin", &back));
	REQUIRE_EQ(static_cast<int>(back.size()), 200004);

	size_t leaked = 0;
	for (size_t i = 4; i < 200000; ++i)
		if (back[i] == 'S') ++leaked;

	CHECK_MSG(leaked == 0,
		("空洞裡有 " + std::to_string(leaked) +
		 " bytes 是已刪除檔案的內容（應該全部是 0）").c_str());
}

// ================================================================ 格式
//
// .paki 的 magic 是 "AIO2"。只認得舊 16 bytes TrieNode 佈局（"AIOD"）的讀取器
// 靠它直接拒絕開啟，而不是照舊佈局誤讀出一堆垃圾。

VFS_TEST(compat, magic_on_disk) {
	auto read_magic = [](const std::string& paki) -> int {
		std::FILE* fp = std::fopen(paki.c_str(), "rb");
		if (!fp) return 0;
		int magic = 0;
		const size_t got = std::fread(&magic, 1, sizeof(magic), fp);
		std::fclose(fp);
		return (got == sizeof(magic)) ? magic : 0;
	};

	TempArchive a("magic");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "x", Blob(100, 1)));
	a.Close();
	CHECK_EQ(read_magic(a.base() + ".paki"), 0x324F4941);   // "AIO2"
}
