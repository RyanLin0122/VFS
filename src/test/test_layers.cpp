// test_layers.cpp — L2 IIO / L3 Data / L4 FAT / L5 NT / L6 DT 的分層測試
//
// 分層測試的價值在於定位：L7 壞掉時，這些案例告訴你是哪一層的問題。
// 重構時也是它們先變紅 —— 例如把 FAT 從單向鏈改成 extent，壞的會是 fat 這組，
// 而不是一路燒到 file 那組才發現。

#include <vfs_test.h>
#include <vfs_testutil.h>
#include <vfs_platform.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <cstring>
#include <string>
#include <vector>

using vfsutil::Blob;
using vfsutil::TempArchive;

// ================================================================ L2：IIO

VFS_TEST(iio, channel_layout_is_fixed) {
	// .paki 的通道配置是寫死的約定，遊戲端與工具端都依賴它：
	// Ch0 = DT TrieNode, Ch1 = DT KeyNode, Ch2 = NT, Ch3 = FAT
	TempArchive a("iio_layout");
	REQUIRE(a.Ok());
	VfsIioFile* iio = a.h()->iio_handle;
	REQUIRE(iio != nullptr);

	CHECK_GE(iio->num_channels, static_cast<short>(4));
	CHECK_EQ(a.h()->dt_handle->trienode_channel_id, 0);
	CHECK_EQ(a.h()->dt_handle->keynode_channel_id, 1);
	CHECK_EQ(a.h()->nt_handle->nt_iio_channel_id, 2);
	CHECK_EQ(a.h()->fat_handle->fat_iio_channel_id, 3);
}

VFS_TEST(iio, read_write_roundtrip_within_channel) {
	TempArchive a("iio_rw");
	REQUIRE(a.Ok());
	VfsIioFile* iio = a.h()->iio_handle;
	REQUIRE(iio != nullptr);

	// 借用 NT 通道（2）做讀寫往返。寫在很後面的位置，強迫通道成長。
	const int ch = 2;
	const std::vector<char> data = Blob(4096, 77);

	REQUIRE_EQ(vfs_iio_seek(iio, ch, 100000), 100000);   // 回傳新位置
	CHECK_EQ(vfs_iio_write(iio, ch, data.data(), static_cast<int>(data.size())),
	         static_cast<int>(data.size()));

	std::vector<char> back(data.size());
	REQUIRE_EQ(vfs_iio_seek(iio, ch, 100000), 100000);   // 回傳新位置
	CHECK_EQ(vfs_iio_read(iio, ch, back.data(), static_cast<int>(back.size())),
	         static_cast<int>(back.size()));
	CHECK_BLOB_EQ(back, data);
}

VFS_TEST(iio, writes_survive_cache_eviction) {
	// IIO 每個通道的 LRU 上限是 8 頁（vfs.cpp:829）。寫超過 8 個 stripe
	// 會逼出淘汰，被淘汰的髒頁必須先刷回磁碟 —— 這裡驗的就是那條路徑。
	TempArchive a("iio_evict");
	REQUIRE(a.Ok());
	VfsIioFile* iio = a.h()->iio_handle;
	REQUIRE(iio != nullptr);

	const int ch = 2;
	VfsIioChannel* chan = vfs_iio_get_channel(iio, ch);
	REQUIRE(chan != nullptr);
	const int stripe_bytes = chan->blocks_per_stripe * vfs_iio_BLOCK_SIZEv;
	REQUIRE_GT(stripe_bytes, 0);

	// 寫 24 個 stripe，是快取容量的 3 倍
	const int kStripes = 24;
	std::vector<std::vector<char>> written;
	for (int i = 0; i < kStripes; ++i) {
		std::vector<char> d = Blob(static_cast<size_t>(stripe_bytes), static_cast<unsigned>(i + 500));
		REQUIRE_EQ(vfs_iio_seek(iio, ch, i * stripe_bytes), i * stripe_bytes);
		REQUIRE_EQ(vfs_iio_write(iio, ch, d.data(), stripe_bytes), stripe_bytes);
		written.push_back(std::move(d));
	}

	// 全部回頭讀一次；若淘汰時沒刷回去，這裡會讀到舊資料
	int bad = 0;
	std::vector<char> back(static_cast<size_t>(stripe_bytes));
	for (int i = 0; i < kStripes; ++i) {
		if (vfs_iio_seek(iio, ch, i * stripe_bytes) != i * stripe_bytes) { ++bad; continue; }
		if (vfs_iio_read(iio, ch, back.data(), stripe_bytes) != stripe_bytes) { ++bad; continue; }
		if (back != written[i]) ++bad;
	}
	CHECK_MSG(bad == 0,
		("超出快取容量後有 " + std::to_string(bad) + " / " + std::to_string(kStripes) +
		 " 個 stripe 內容錯誤（髒頁淘汰時沒寫回）").c_str());
}

VFS_TEST(iio, channel_size_tracks_writes) {
	TempArchive a("iio_size");
	REQUIRE(a.Ok());
	VfsIioFile* iio = a.h()->iio_handle;
	REQUIRE(iio != nullptr);

	VfsIioChannel* chan = vfs_iio_get_channel(iio, 2);
	REQUIRE(chan != nullptr);
	const int before = vfs_iio_channel_size(chan);

	const std::vector<char> data = Blob(8192, 9);
	REQUIRE_EQ(vfs_iio_seek(iio, 2, 50000), 50000);
	REQUIRE_EQ(vfs_iio_write(iio, 2, data.data(), 8192), 8192);

	const int after = vfs_iio_channel_size(chan);
	CHECK_GE(after, 50000 + 8192);
	CHECK_GE(after, before);
}

VFS_TEST(iio, bad_channel_index_rejected) {
	TempArchive a("iio_bad");
	REQUIRE(a.Ok());
	VfsIioFile* iio = a.h()->iio_handle;
	REQUIRE(iio != nullptr);

	char buf[16] = {};
	CHECK_LT(vfs_iio_read(iio, -1, buf, 16), 0);
	CHECK_LT(vfs_iio_read(iio, 9999, buf, 16), 0);
	CHECK_LT(vfs_iio_write(iio, -1, buf, 16), 0);
	CHECK(vfs_iio_get_channel(iio, 9999) == nullptr);
}

// ================================================================ L3：Data (.pak)

VFS_TEST(data, block_roundtrip) {
	TempArchive a("data_rt");
	REQUIRE(a.Ok());
	VfsDataHandle* d = a.h()->data_handle;
	REQUIRE(d != nullptr);

	const std::vector<char> blk = Blob(512, 31);
	std::vector<char> back(512);

	const int idx[] = { 1, 2, 127, 128, 129, 1000, 100000 };
	for (int i : idx) {
		CHECK_EQ(vfs_data_write(d, i, blk.data()), 0);
	}
	for (int i : idx) {
		CHECK_EQ(vfs_data_read(d, i, back.data()), 0);
		CHECK_BLOB_EQ(back, blk);
	}
}

VFS_TEST(data, distinct_blocks_keep_distinct_content) {
	// 64KB 滑動視窗的邊界：連續寫 300 個區塊（150KB）會跨多次視窗滑動，
	// 每次滑動都要先把髒視窗刷回去，漏掉就會互相覆蓋。
	TempArchive a("data_many");
	REQUIRE(a.Ok());
	VfsDataHandle* d = a.h()->data_handle;
	REQUIRE(d != nullptr);

	const int kN = 300;
	for (int i = 1; i <= kN; ++i) {
		const std::vector<char> blk = Blob(512, static_cast<unsigned>(i));
		REQUIRE_EQ(vfs_data_write(d, i, blk.data()), 0);
	}

	int bad = 0;
	std::vector<char> back(512);
	for (int i = 1; i <= kN; ++i) {
		if (vfs_data_read(d, i, back.data()) != 0) { ++bad; continue; }
		if (back != Blob(512, static_cast<unsigned>(i))) ++bad;
	}
	CHECK_MSG(bad == 0, ("有 " + std::to_string(bad) + " 個區塊內容錯誤").c_str());
}

VFS_TEST(data, contiguous_matches_individual) {
	// vfs_data_read_contiguous 必須跟逐塊讀得到一樣的結果 ——
	// 之後把 vfs_file_read 改成用 contiguous 來合併 I/O 時，靠這個案例保證等價。
	TempArchive a("data_contig");
	REQUIRE(a.Ok());
	VfsDataHandle* d = a.h()->data_handle;
	REQUIRE(d != nullptr);

	const int kStart = 10, kCount = 40;
	std::vector<char> expect;
	for (int i = 0; i < kCount; ++i) {
		const std::vector<char> blk = Blob(512, static_cast<unsigned>(i + 700));
		REQUIRE_EQ(vfs_data_write(d, kStart + i, blk.data()), 0);
		expect.insert(expect.end(), blk.begin(), blk.end());
	}

	std::vector<char> back(static_cast<size_t>(kCount) * 512);
	CHECK_EQ(vfs_data_read_contiguous(d, kStart, kCount, back.data()), 0);
	CHECK_BLOB_EQ(back, expect);
}

VFS_TEST(data, read_beyond_eof_is_zero_filled) {
	// 讀還沒寫過的區塊應該拿到 0，不是垃圾（cache_slide 有補零）
	TempArchive a("data_eof");
	REQUIRE(a.Ok());
	VfsDataHandle* d = a.h()->data_handle;
	REQUIRE(d != nullptr);

	std::vector<char> back(512, static_cast<char>(0xAB));   // 先填非零，確認真的被覆蓋
	CHECK_EQ(vfs_data_read(d, 500000, back.data()), 0);
	CHECK_BLOB_EQ(back, std::vector<char>(512, 0));
}

VFS_TEST(data, negative_index_rejected) {
	TempArchive a("data_neg");
	REQUIRE(a.Ok());
	VfsDataHandle* d = a.h()->data_handle;
	REQUIRE(d != nullptr);

	char buf[512] = {};
	CHECK_EQ(vfs_data_read(d, -1, buf), -1);
	CHECK_EQ(vfs_data_write(d, -1, buf), -1);
	CHECK_EQ(vfs_data_read(nullptr, 0, buf), -1);
	CHECK_EQ(vfs_data_read(d, 0, nullptr), -1);
}

// ================================================================ L4：FAT

VFS_TEST(fat, create_and_extend_chain) {
	TempArchive a("fat_chain");
	REQUIRE(a.Ok());
	VfsFatHandle* f = a.h()->fat_handle;
	REQUIRE(f != nullptr);

	const int head = vfs_fat_create_chain(f);
	REQUIRE_GT(head, 0);

	// 連續延伸 50 塊，每塊都要拿到有效且互不重複的索引
	std::vector<int> blocks;
	blocks.push_back(head);
	int cur = head;
	for (int i = 0; i < 50; ++i) {
		const int nb = vfs_fat_chain_extend(f, cur);
		REQUIRE_GT(nb, 0);
		blocks.push_back(nb);
		cur = nb;
	}

	// 不可重複配置
	std::vector<int> sorted = blocks;
	std::sort(sorted.begin(), sorted.end());
	const bool has_dup = std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
	CHECK_MSG(!has_dup, "FAT 把同一個區塊配置給了兩個位置");

	// get_nth 要走得到每一個
	int mismatched = 0;
	for (size_t i = 0; i < blocks.size(); ++i) {
		if (vfs_fat_chain_get_nth(f, head, static_cast<int>(i)) != blocks[i]) ++mismatched;
	}
	CHECK_MSG(mismatched == 0,
		("get_nth 有 " + std::to_string(mismatched) + " 個位置走錯").c_str());
}

VFS_TEST(fat, get_nth_past_end_returns_invalid) {
	TempArchive a("fat_past");
	REQUIRE(a.Ok());
	VfsFatHandle* f = a.h()->fat_handle;
	REQUIRE(f != nullptr);

	const int head = vfs_fat_create_chain(f);
	REQUIRE_GT(head, 0);
	vfs_fat_chain_extend(f, head);          // 鏈長度 2

	CHECK_LE(vfs_fat_chain_get_nth(f, head, 5), 0);
	CHECK_LE(vfs_fat_chain_get_nth(f, head, 1000), 0);
}

VFS_TEST(fat, destroy_chain_frees_blocks_for_reuse) {
	TempArchive a("fat_free");
	REQUIRE(a.Ok());
	VfsFatHandle* f = a.h()->fat_handle;
	REQUIRE(f != nullptr);

	// 配一條長鏈、記下用掉的區塊，銷毀後再配一條，應該重用到同一批
	const int head = vfs_fat_create_chain(f);
	REQUIRE_GT(head, 0);
	int cur = head;
	std::vector<int> first_pass;
	first_pass.push_back(head);
	for (int i = 0; i < 30; ++i) {
		cur = vfs_fat_chain_extend(f, cur);
		REQUIRE_GT(cur, 0);
		first_pass.push_back(cur);
	}

	REQUIRE_EQ(vfs_fat_destroy_chain(f, head), 0);

	const int head2 = vfs_fat_create_chain(f);
	REQUIRE_GT(head2, 0);
	int cur2 = head2;
	std::vector<int> second_pass;
	second_pass.push_back(head2);
	for (int i = 0; i < 30; ++i) {
		cur2 = vfs_fat_chain_extend(f, cur2);
		REQUIRE_GT(cur2, 0);
		second_pass.push_back(cur2);
	}

	// 至少要有相當比例落在原本那批，否則空間根本沒回收
	std::vector<int> sorted_first = first_pass;
	std::sort(sorted_first.begin(), sorted_first.end());
	int reused = 0;
	for (int b : second_pass)
		if (std::binary_search(sorted_first.begin(), sorted_first.end(), b)) ++reused;

	CHECK_MSG(reused > static_cast<int>(second_pass.size()) / 2,
		("銷毀鏈之後只有 " + std::to_string(reused) + " / " +
		 std::to_string(second_pass.size()) + " 個區塊被重用").c_str());
}

VFS_TEST(fat, shrink_keeps_prefix) {
	TempArchive a("fat_shrink");
	REQUIRE(a.Ok());
	VfsFatHandle* f = a.h()->fat_handle;
	REQUIRE(f != nullptr);

	const int head = vfs_fat_create_chain(f);
	REQUIRE_GT(head, 0);
	int cur = head;
	std::vector<int> blocks;
	blocks.push_back(head);
	for (int i = 0; i < 20; ++i) {
		cur = vfs_fat_chain_extend(f, cur);
		REQUIRE_GT(cur, 0);
		blocks.push_back(cur);
	}

	REQUIRE_EQ(vfs_fat_chain_shrink(f, head, 5), 0);

	// 前 5 塊不變
	for (int i = 0; i < 5; ++i)
		CHECK_EQ(vfs_fat_chain_get_nth(f, head, i), blocks[static_cast<size_t>(i)]);
	// 第 6 塊起已不存在
	CHECK_LE(vfs_fat_chain_get_nth(f, head, 5), 0);
}

// ================================================================ L5：NT

VFS_TEST(nt, allocate_returns_distinct_nodes) {
	TempArchive a("nt_alloc");
	REQUIRE(a.Ok());
	VfsNtHandle* nt = a.h()->nt_handle;
	REQUIRE(nt != nullptr);

	std::vector<int> ids;
	for (int i = 0; i < 100; ++i) {
		const int id = vfs_nt_allocate_node(nt);
		REQUIRE_GE(id, 0);
		ids.push_back(id);
	}
	std::sort(ids.begin(), ids.end());
	CHECK_MSG(std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
		"NT 把同一個節點配置了兩次");
}

VFS_TEST(nt, node_fields_roundtrip) {
	TempArchive a("nt_rt");
	REQUIRE(a.Ok());
	VfsNtHandle* nt = a.h()->nt_handle;
	REQUIRE(nt != nullptr);

	const int id = vfs_nt_allocate_node(nt);
	REQUIRE_GE(id, 0);

	vfs_nt_node_set_size(nt, id, 123456);
	CHECK_EQ(vfs_nt_node_get_size(nt, id), 123456);

	vfs_nt_node_set_chain(nt, id, 4242);
	CHECK_EQ(vfs_nt_node_get_chain(nt, id), 4242);

	// 0 與極大值都要能存
	vfs_nt_node_set_size(nt, id, 0);
	CHECK_EQ(vfs_nt_node_get_size(nt, id), 0);
	vfs_nt_node_set_size(nt, id, 0x7FFFFFFF);
	CHECK_EQ(vfs_nt_node_get_size(nt, id), 0x7FFFFFFF);
}

VFS_TEST(nt, refcount_semantics) {
	TempArchive a("nt_ref");
	REQUIRE(a.Ok());
	VfsNtHandle* nt = a.h()->nt_handle;
	REQUIRE(nt != nullptr);

	const int id = vfs_nt_allocate_node(nt);
	REQUIRE_GE(id, 0);

	VfsNode n = {};
	REQUIRE_EQ(vfs_nt_get_node(nt, id, &n), 16);   // 回傳讀到的位元組數
	const int base = n.ref_count;

	vfs_nt_refcount_incr(nt, id);
	REQUIRE_EQ(vfs_nt_get_node(nt, id, &n), 16);   // 回傳讀到的位元組數
	CHECK_EQ(n.ref_count, base + 1);

	vfs_nt_refcount_decr(nt, id);
	REQUIRE_EQ(vfs_nt_get_node(nt, id, &n), 16);   // 回傳讀到的位元組數
	CHECK_EQ(n.ref_count, base);
}

VFS_TEST(nt, node_persists_across_reopen) {
	TempArchive a("nt_persist");
	REQUIRE(a.Ok());

	const int id = vfs_nt_allocate_node(a.h()->nt_handle);
	REQUIRE_GE(id, 0);
	vfs_nt_node_set_size(a.h()->nt_handle, id, 987654);
	vfs_nt_node_set_chain(a.h()->nt_handle, id, 321);

	REQUIRE(a.Reopen() != nullptr);
	CHECK_EQ(vfs_nt_node_get_size(a.h()->nt_handle, id), 987654);
	CHECK_EQ(vfs_nt_node_get_chain(a.h()->nt_handle, id), 321);
}

// ================================================================ L6：DT

VFS_TEST(dt, insert_lookup_delete) {
	TempArchive a("dt_basic");
	REQUIRE(a.Ok());
	VfsDtHandle* dt = a.h()->dt_handle;
	REQUIRE(dt != nullptr);

	const char* names[] = { "alpha", "beta", "gamma/delta", "a/b/c/d/e.txt" };
	for (const char* n : names) {
		CHECK_GE(vfs_dt_filename_add(dt, n), 0);
	}
	for (const char* n : names) {
		CHECK_MSG(vfs_dt_filename_lookup(dt, n) >= 0, (std::string("找不到 ") + n).c_str());
	}
	CHECK_LT(vfs_dt_filename_lookup(dt, "nonexistent"), 0);

	REQUIRE_EQ(vfs_dt_filename_delete(dt, "beta"), 0);
	CHECK_LT(vfs_dt_filename_lookup(dt, "beta"), 0);
	for (const char* n : { "alpha", "gamma/delta", "a/b/c/d/e.txt" })
		CHECK_MSG(vfs_dt_filename_lookup(dt, n) >= 0,
			(std::string("刪 beta 波及了 ") + n).c_str());
}

VFS_TEST(dt, key_name_roundtrip) {
	// 存進去的鍵名要能原樣取回來（KeyNode 片段是 60 bytes，長名會跨片段）
	TempArchive a("dt_name");
	REQUIRE(a.Ok());
	VfsDtHandle* dt = a.h()->dt_handle;
	REQUIRE(dt != nullptr);

	const int lens[] = { 1, 30, 59, 60, 61, 100, 119, 120, 121, 200 };
	for (int n : lens) {
		const std::string key(static_cast<size_t>(n), 'x');
		const int tn = vfs_dt_filename_add(dt, key.c_str());
		REQUIRE_GE(tn, 0);

		char out[4096] = {};
		REQUIRE_EQ(vfs_dt_filename_get_name(dt, tn, out), 0);
		CHECK_MSG(key == out,
			("長度 " + std::to_string(n) + " 的鍵名取回時變了：\"" + out + "\"").c_str());
	}
}

VFS_TEST(dt, nt_index_roundtrip) {
	TempArchive a("dt_ntidx");
	REQUIRE(a.Ok());
	VfsDtHandle* dt = a.h()->dt_handle;
	REQUIRE(dt != nullptr);

	const int tn = vfs_dt_filename_add(dt, "some/file.bin");
	REQUIRE_GE(tn, 0);

	// set 成功回 0（版本化之後改成 read-modify-write，不再回傳位元組數）
	REQUIRE_EQ(vfs_dt_filename_set_nt_index(dt, tn, 12345), 0);
	CHECK_EQ(vfs_dt_filename_get_nt_index(dt, tn), 12345);

	// 舊格式的 short 上界。現在必須能跨過去 —— 這是 BUG 2 的核心。
	const int beyond_short[] = { 32767, 32768, 100000, 1000000, 2000000000 };
	for (int v : beyond_short) {
		CHECK_MSG(vfs_dt_filename_set_nt_index(dt, tn, v) == 0,
			("設定 nt_idx = " + std::to_string(v) + " 失敗").c_str());
		CHECK_EQ(vfs_dt_filename_get_nt_index(dt, tn), v);
	}
}

VFS_TEST(dt, pmatch_semantics) {
	// glob 的樣式比對是獨立可測的純函式
	CHECK_EQ(vfs_pmatch("*", "anything", 0), 0);
	CHECK_EQ(vfs_pmatch("*.txt", "readme.txt", 0), 0);
	CHECK_NE(vfs_pmatch("*.txt", "readme.md", 0), 0);
	CHECK_EQ(vfs_pmatch("a?c", "abc", 0), 0);
	CHECK_NE(vfs_pmatch("a?c", "abbc", 0), 0);
	CHECK_EQ(vfs_pmatch("dir/*", "dir/file", 0), 0);
	CHECK_EQ(vfs_pmatch("exact", "exact", 0), 0);
	CHECK_NE(vfs_pmatch("exact", "exacts", 0), 0);
	CHECK_EQ(vfs_pmatch("", "", 0), 0);
}

VFS_TEST(dt, many_keys_all_findable) {
	// Patricia trie 在大量相似鍵下最容易出錯
	TempArchive a("dt_many");
	REQUIRE(a.Ok());
	VfsDtHandle* dt = a.h()->dt_handle;
	REQUIRE(dt != nullptr);

	const int kN = 2000;
	char key[64];
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "assets/texture/tex_%05d.dds", i);
		REQUIRE_GE(vfs_dt_filename_add(dt, key), 0);
	}

	int missing = 0;
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "assets/texture/tex_%05d.dds", i);
		if (vfs_dt_filename_lookup(dt, key) < 0) ++missing;
	}
	CHECK_MSG(missing == 0, ("有 " + std::to_string(missing) + " 個鍵查不到").c_str());

	// 刪掉一半，剩下一半必須完好
	for (int i = 0; i < kN; i += 2) {
		std::snprintf(key, sizeof(key), "assets/texture/tex_%05d.dds", i);
		REQUIRE_EQ(vfs_dt_filename_delete(dt, key), 0);
	}
	int wrong = 0;
	for (int i = 0; i < kN; ++i) {
		std::snprintf(key, sizeof(key), "assets/texture/tex_%05d.dds", i);
		const bool found = vfs_dt_filename_lookup(dt, key) >= 0;
		if (found != (i % 2 == 1)) ++wrong;
	}
	CHECK_MSG(wrong == 0,
		("刪掉一半之後有 " + std::to_string(wrong) + " 個鍵的存在狀態不對").c_str());
}

// ---------------------------------------------------------------- 刪除的副作用
//
// 診斷用：逐一刪除每個鍵，檢查其餘的鍵是否全部倖存。
// 失敗訊息會指名「刪 X 害死了 Y」，讓成因一目了然。
VFS_TEST(dt, delete_does_not_disturb_siblings) {
	struct Case { const char* name; std::vector<std::string> keys; };
	const Case cases[] = {
		{ "共用前綴（逐字加長）", { "a", "ab", "abc", "abcd", "abcde" } },
		{ "共用前綴（分岔）",     { "tex", "tex_a", "tex_b", "texture" } },
		{ "數字尾綴",             { "c/1", "c/2", "c/10", "c/11", "c/100" } },
		{ "同長度不同內容",       { "aaa", "aab", "aba", "baa" } },
		{ "路徑階層",             { "a/b", "a/b/c", "a/b/c/d" } },
	};

	for (const Case& cs : cases) {
		for (size_t victim = 0; victim < cs.keys.size(); ++victim) {
			TempArchive a("dt_del");
			REQUIRE(a.Ok());
			VfsDtHandle* dt = a.h()->dt_handle;
			REQUIRE(dt != nullptr);

			for (const std::string& k : cs.keys)
				REQUIRE_GE(vfs_dt_filename_add(dt, k.c_str()), 0);

			REQUIRE_EQ(vfs_dt_filename_delete(dt, cs.keys[victim].c_str()), 0);

			for (size_t i = 0; i < cs.keys.size(); ++i) {
				if (i == victim) {
					CHECK_MSG(vfs_dt_filename_lookup(dt, cs.keys[i].c_str()) < 0,
						(std::string(cs.name) + "：刪掉的 \"" + cs.keys[i] + "\" 還查得到").c_str());
				} else {
					CHECK_MSG(vfs_dt_filename_lookup(dt, cs.keys[i].c_str()) >= 0,
						(std::string(cs.name) + "：刪 \"" + cs.keys[victim] +
						 "\" 連帶害死了 \"" + cs.keys[i] + "\"").c_str());
				}
			}
		}
	}
}

// 插入階段就要驗：鍵互為前綴時，全部都必須查得到。
// （delete_does_not_disturb_siblings 驗的是刪除；這裡把「插入本身有沒有壞」分離出來）
VFS_TEST(dt, prefix_keys_all_findable_after_insert) {
	TempArchive a("dt_prefix_ins");
	REQUIRE(a.Ok());
	VfsDtHandle* dt = a.h()->dt_handle;
	REQUIRE(dt != nullptr);

	// 典型的遊戲資源命名：數字尾綴讓短鍵天然是長鍵的前綴
	std::vector<std::string> keys;
	for (int i = 0; i < 200; ++i) keys.push_back("c/" + std::to_string(i));
	for (const std::string& k : keys)
		REQUIRE_GE(vfs_dt_filename_add(dt, k.c_str()), 0);

	int missing = 0;
	std::string first_missing;
	for (const std::string& k : keys) {
		if (vfs_dt_filename_lookup(dt, k.c_str()) < 0) {
			++missing;
			if (first_missing.empty()) first_missing = k;
		}
	}
	CHECK_MSG(missing == 0,
		("插入 " + std::to_string(keys.size()) + " 個互為前綴的鍵之後，有 " +
		 std::to_string(missing) + " 個查不到（第一個是 \"" + first_missing + "\"）").c_str());
}

// ---------------------------------------------------------------- 隨機對拍
//
// Trie 的刪除是最容易改錯的地方：改壞了不會當掉，只會讓某些鍵悄悄消失，
// 而且要很特定的形狀才踩得到。所以用參考模型對拍 —— 拿 std::set 當「正確答案」，
// 隨機插入／刪除幾千次，每一步都要求兩邊完全一致。
//
// 固定種子跑多組，失敗時訊息會指出是哪一組種子、第幾步、哪個鍵，可以直接重現。
VFS_TEST(dt, random_insert_delete_matches_model) {
	// 刻意用會大量產生共用前綴的名字：真實遊戲資源就長這樣
	auto make_key = [](int n) {
		char buf[64];
		switch (n % 4) {
			case 0: std::snprintf(buf, sizeof(buf), "tex/%d", n); break;
			case 1: std::snprintf(buf, sizeof(buf), "tex/%d_hi", n); break;
			case 2: std::snprintf(buf, sizeof(buf), "snd/bgm%d", n); break;
			default: std::snprintf(buf, sizeof(buf), "map/%d/tiles", n); break;
		}
		return std::string(buf);
	};

	const unsigned long long seeds[] = { 1, 7, 12345, 0xABCDEF };
	for (unsigned long long seed : seeds) {
		TempArchive a("dt_model");
		REQUIRE(a.Ok());
		VfsDtHandle* dt = a.h()->dt_handle;
		REQUIRE(dt != nullptr);

		std::set<std::string> model;       // 參考答案
		vfsutil::Rng rng(seed);
		const int kPool = 400;
		const int kSteps = 1200;

		for (int step = 0; step < kSteps; ++step) {
			const std::string key = make_key(static_cast<int>(rng.Range(0, kPool - 1)));
			const bool want_insert = (rng.Unit() < 0.55);   // 稍微偏向插入，讓樹長得起來

			if (want_insert) {
				if (model.insert(key).second) {
					if (vfs_dt_filename_add(dt, key.c_str()) < 0) {
						CHECK_MSG(false, ("seed " + std::to_string(seed) + " step " +
							std::to_string(step) + "：插入 \"" + key + "\" 失敗").c_str());
						return;
					}
				}
			} else {
				if (model.erase(key) > 0) {
					if (vfs_dt_filename_delete(dt, key.c_str()) < 0) {
						CHECK_MSG(false, ("seed " + std::to_string(seed) + " step " +
							std::to_string(step) + "：刪除 \"" + key + "\" 失敗").c_str());
						return;
					}
				}
			}

			// 每 50 步全量對拍一次（每步都比會太慢）
			if (step % 50 != 0) continue;

			for (int i = 0; i < kPool; ++i) {
				const std::string k = make_key(i);
				const bool in_model = (model.count(k) > 0);
				const bool in_trie = (vfs_dt_filename_lookup(dt, k.c_str()) >= 0);
				if (in_model != in_trie) {
					CHECK_MSG(false, ("seed " + std::to_string(seed) + " step " +
						std::to_string(step) + "：\"" + k + "\" 在模型中" +
						(in_model ? "存在" : "不存在") + "，在 trie 中" +
						(in_trie ? "存在" : "不存在")).c_str());
					return;
				}
			}
		}
		CHECK(true);   // 這一組種子從頭到尾都一致
	}
}
