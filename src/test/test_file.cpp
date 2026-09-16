// test_file.cpp — Layer 7（vfs_file_* / vfs_glob）功能正確性測試
//
// 這是玩家與打包工具真正會踩到的那一層，所以測得最細：
// 每個 API 的正常路徑、邊界、錯誤路徑、以及關閉重開後的持久化。

#include <vfs_test.h>
#include <vfs_testutil.h>
#include <vfs_platform.h>

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

namespace {

// vfs.cpp 內部的 open flag（沒 export 到 vfs.h，測試這邊自己定義一份）
const int kRdOnly   = 0x00;
const int kWrOnly   = 0x01;
const int kRdWr     = 0x02;
const int kAppend   = 0x08;
const int kTruncate = 0x200;

} // namespace

// ================================================================ 生命週期

VFS_TEST(file, start_creates_both_files) {
	TempArchive a("start");
	REQUIRE(a.Ok());
	CHECK_MSG(a.PakBytes() >= 0, ".pak 應該被建立");
	CHECK_MSG(a.PakiBytes() >= 0, ".paki 應該被建立");
	CHECK_MSG(vfs_exists(a.base().c_str()) != 0, "vfs_exists 應認得剛建好的封包");
}

VFS_TEST(file, start_readonly_on_missing_archive_fails) {
	// access_mode 1（唯讀）遇到不存在的封包不該靜靜生出一個空封包
	const std::string base = vfsplat::PathJoin(vfsplat::ScratchDir(), "vfs_nonexistent_ro");
	vfsutil::RemoveArchive(base);
	VfsHandle* h = vfs_start(base.c_str(), 1);
	CHECK_MSG(h == nullptr, "唯讀模式開啟不存在的封包應失敗");
	if (h) vfs_end(h, 0);
	vfsutil::RemoveArchive(base);
}

VFS_TEST(file, empty_archive_has_no_files) {
	TempArchive a("empty");
	REQUIRE(a.Ok());
	CHECK_EQ(ListKeys(a.h()).size(), static_cast<size_t>(0));
	CHECK_EQ(vfs_file_exists(a.h(), "anything"), 0);
}

// ================================================================ 寫入／讀回

// 各種大小的往返比對。512 的倍數前後各取一點，因為區塊邊界是最容易出錯的地方。
VFS_TEST(file, roundtrip_sizes) {
	TempArchive a("rt");
	REQUIRE(a.Ok());

	const int sizes[] = {
		0, 1, 2, 7, 63, 64, 127, 128, 255, 256,
		511, 512,        514, 1023, 1024,        1535, 1536,
		4095, 4096,        8191, 8192,
		65535, 65536, 65538,           // 跨越 .pak 的 64KB 快取視窗
		1000000                        // 約 1MB，多次視窗滑動
	};
	// 註：513 / 1025 / 2049 / 4097 / 65537（size % 512 == 1）目前必定失敗，
	//     獨立放在 regress/write_size_mod512_eq_1，這裡先不混進來。

	for (int sz : sizes) {
		const std::string key = "rt/" + std::to_string(sz) + ".bin";
		const std::vector<char> data = Blob(static_cast<size_t>(sz), static_cast<unsigned>(sz));

		CHECK_MSG(WriteAll(a.h(), key, data), ("寫入 " + std::to_string(sz) + " bytes").c_str());
		CHECK_EQ(FileSize(a.h(), key), sz);

		std::vector<char> back;
		CHECK_MSG(ReadAll(a.h(), key, &back), ("讀回 " + std::to_string(sz) + " bytes").c_str());
		CHECK_BLOB_EQ(back, data);
	}
}

VFS_TEST(file, roundtrip_survives_reopen) {
	TempArchive a("persist");
	REQUIRE(a.Ok());

	const std::vector<char> small = Blob(100, 1);
	const std::vector<char> large = Blob(300000, 2);
	REQUIRE(WriteAll(a.h(), "p/small.bin", small));
	REQUIRE(WriteAll(a.h(), "p/large.bin", large));

	REQUIRE(a.Reopen() != nullptr);

	std::vector<char> back;
	CHECK_MSG(ReadAll(a.h(), "p/small.bin", &back), "重開後讀小檔");
	CHECK_BLOB_EQ(back, small);
	CHECK_MSG(ReadAll(a.h(), "p/large.bin", &back), "重開後讀大檔");
	CHECK_BLOB_EQ(back, large);
}

VFS_TEST(file, partial_reads_reassemble) {
	// 呼叫端用小 buffer 分次讀，結果必須跟一次讀完相同
	TempArchive a("partial");
	REQUIRE(a.Ok());

	const std::vector<char> data = Blob(20000, 7);
	REQUIRE(WriteAll(a.h(), "pr.bin", data));

	const int fd = vfs_file_open(a.h(), "pr.bin", kRdWr);
	REQUIRE_GE(fd, 0);

	std::vector<char> back;
	char chunk[333];                       // 刻意不是 512 的倍數
	for (;;) {
		const int got = vfs_file_read(a.h(), fd, chunk, sizeof(chunk));
		if (got <= 0) break;
		back.insert(back.end(), chunk, chunk + got);
	}
	vfs_file_close(a.h(), fd);
	CHECK_BLOB_EQ(back, data);
}

VFS_TEST(file, reads_across_fragmented_chains) {
	// 讀取會把實體上連續的區塊併成一次讀，超過 .pak 視窗的整段直接讀進呼叫端 buffer。
	// 碎片化的鏈、從區塊中間開始、還沒寫回磁碟的內容，結果都必須正確。
	TempArchive a("fragread");
	REQUIRE(a.Ok());

	// 兩個檔交錯追加 → FAT 鏈互相穿插；x 最後再追加一大段連續區塊
	std::vector<char> x = Blob(300000, 21);
	const std::vector<char> y = Blob(300000, 22);
	const int fx = vfs_file_create(a.h(), "frag/x.bin");
	const int fy = vfs_file_create(a.h(), "frag/y.bin");
	REQUIRE_GE(fx, 0);
	REQUIRE_GE(fy, 0);
	for (size_t off = 0; off < x.size(); off += 700) {
		const int n = static_cast<int>((x.size() - off < 700) ? x.size() - off : 700);
		REQUIRE_EQ(vfs_file_write(a.h(), fx, x.data() + off, n), n);
		REQUIRE_EQ(vfs_file_write(a.h(), fy, y.data() + off, n), n);
	}
	const std::vector<char> tail = Blob(200000, 23);
	REQUIRE_EQ(vfs_file_write(a.h(), fx, tail.data(), static_cast<int>(tail.size())), static_cast<int>(tail.size()));
	x.insert(x.end(), tail.begin(), tail.end());
	vfs_file_close(a.h(), fx);
	vfs_file_close(a.h(), fy);

	// 第一個讀取就是跨視窗的大段讀，此時視窗裡還有沒寫回磁碟的尾段
	{
		const int fd = vfs_file_open(a.h(), "frag/x.bin", kRdWr);
		REQUIRE_GE(fd, 0);
		const int aligned = ((300000 + 511) / 512) * 512;
		REQUIRE_EQ(vfs_file_lseek(a.h(), fd, aligned, 0), 0);
		vfs_stat_reset();
		std::vector<char> back(x.size() - aligned);
		CHECK_EQ(vfs_file_read(a.h(), fd, back.data(), static_cast<int>(back.size())), static_cast<int>(back.size()));
		CHECK_EQ(vfs_stat_data_slide, 0);
		CHECK_GT(vfs_stat_data_direct, 0);
		CHECK_BLOB_EQ(back, std::vector<char>(x.begin() + aligned, x.end()));
		vfs_file_close(a.h(), fd);
	}

	const int chunks[] = { 1000000, 100000, 65536, 4096, 513, 512, 511, 333 };
	for (int chunk : chunks) {
		for (int which = 0; which < 2; ++which) {
			const int fd = vfs_file_open(a.h(), which ? "frag/y.bin" : "frag/x.bin", kRdWr);
			REQUIRE_GE(fd, 0);
			std::vector<char> back;
			std::vector<char> buf(static_cast<size_t>(chunk));
			for (;;) {
				const int got = vfs_file_read(a.h(), fd, buf.data(), chunk);
				if (got <= 0) break;
				back.insert(back.end(), buf.begin(), buf.begin() + got);
			}
			vfs_file_close(a.h(), fd);
			CHECK_MSG(back == (which ? y : x),
				("以 " + std::to_string(chunk) + " bytes 分次讀 " + (which ? "y" : "x") + " 結果不符").c_str());
		}
	}

	REQUIRE(a.Reopen() != nullptr);
	std::vector<char> back;
	CHECK(ReadAll(a.h(), "frag/x.bin", &back));
	CHECK_BLOB_EQ(back, x);
	CHECK(ReadAll(a.h(), "frag/y.bin", &back));
	CHECK_BLOB_EQ(back, y);
}

VFS_TEST(file, partial_writes_reassemble) {
	TempArchive a("pwrite");
	REQUIRE(a.Ok());

	const std::vector<char> data = Blob(20000, 8);
	const int fd = vfs_file_create(a.h(), "pw.bin");
	REQUIRE_GE(fd, 0);

	size_t off = 0;
	bool ok = true;
	while (off < data.size()) {
		const size_t want = (data.size() - off < 333) ? data.size() - off : 333;
		const int put = vfs_file_write(a.h(), fd, data.data() + off, static_cast<int>(want));
		if (put <= 0) { ok = false; break; }
		off += static_cast<size_t>(put);
	}
	vfs_file_close(a.h(), fd);
	REQUIRE(ok);

	std::vector<char> back;
	CHECK_MSG(ReadAll(a.h(), "pw.bin", &back), "分次寫入後讀回");
	CHECK_BLOB_EQ(back, data);
}

VFS_TEST(file, read_at_eof_returns_zero) {
	TempArchive a("eof");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "e.bin", Blob(1000, 3)));

	const int fd = vfs_file_open(a.h(), "e.bin", kRdWr);
	REQUIRE_GE(fd, 0);

	char buf[2048];
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 1000), 1000);   // 讀完整個檔
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 100), 0);       // 已在 EOF
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 0), 0);         // 讀 0 bytes

	// 要求超過剩餘量時，只回傳實際有的
	vfs_file_lseek(a.h(), fd, 900, 0);
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 2000), 100);

	vfs_file_close(a.h(), fd);
}

// ================================================================ 覆寫

VFS_TEST(file, overwrite_same_size) {
	TempArchive a("ow_same");
	REQUIRE(a.Ok());
	const std::vector<char> v1 = Blob(5000, 10);
	const std::vector<char> v2 = Blob(5000, 11);
	REQUIRE(WriteAll(a.h(), "o.bin", v1));
	REQUIRE(WriteAll(a.h(), "o.bin", v2));

	std::vector<char> back;
	CHECK(ReadAll(a.h(), "o.bin", &back));
	CHECK_BLOB_EQ(back, v2);
	CHECK_EQ(FileSize(a.h(), "o.bin"), 5000);
}

VFS_TEST(file, overwrite_shrinks) {
	// 覆寫成比較小的檔案：舊資料不可以殘留在尾端
	TempArchive a("ow_small");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "o.bin", Blob(50000, 12)));
	const std::vector<char> v2 = Blob(300, 13);
	REQUIRE(WriteAll(a.h(), "o.bin", v2));

	CHECK_EQ(FileSize(a.h(), "o.bin"), 300);
	std::vector<char> back;
	CHECK(ReadAll(a.h(), "o.bin", &back));
	CHECK_BLOB_EQ(back, v2);
}

VFS_TEST(file, overwrite_grows) {
	TempArchive a("ow_big");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "o.bin", Blob(300, 14)));
	const std::vector<char> v2 = Blob(50000, 15);
	REQUIRE(WriteAll(a.h(), "o.bin", v2));

	CHECK_EQ(FileSize(a.h(), "o.bin"), 50000);
	std::vector<char> back;
	CHECK(ReadAll(a.h(), "o.bin", &back));
	CHECK_BLOB_EQ(back, v2);
}

VFS_TEST(file, create_on_existing_truncates) {
	TempArchive a("create_tr");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "t.bin", Blob(9000, 16)));

	const int fd = vfs_file_create(a.h(), "t.bin");     // 應該截斷成 0
	REQUIRE_GE(fd, 0);
	vfs_file_close(a.h(), fd);
	CHECK_EQ(FileSize(a.h(), "t.bin"), 0);
}

VFS_TEST(file, open_with_truncate_flag) {
	TempArchive a("open_tr");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "t.bin", Blob(9000, 17)));

	const int fd = vfs_file_open(a.h(), "t.bin", kRdWr | kTruncate);
	REQUIRE_GE(fd, 0);
	vfs_file_close(a.h(), fd);
	CHECK_EQ(FileSize(a.h(), "t.bin"), 0);
}

// 覆寫大量檔案後空間應該被回收，而不是無止境長大
VFS_TEST(file, repeated_overwrite_reuses_space) {
	TempArchive a("reuse");
	REQUIRE(a.Ok());

	const std::vector<char> data = Blob(100000, 18);
	REQUIRE(WriteAll(a.h(), "r.bin", data));
	a.Close();
	const long long after_first = a.PakBytes();
	REQUIRE(a.Reopen() != nullptr);

	for (int i = 0; i < 8; ++i)
		REQUIRE(WriteAll(a.h(), "r.bin", Blob(100000, 19 + i)));
	a.Close();
	const long long after_many = a.PakBytes();

	// 每次覆寫都配新空間的話，這裡會是 9 倍。允許一些配置抖動，但不該失控。
	CHECK_MSG(after_many <= after_first * 2,
		("覆寫 8 次後 .pak 從 " + std::to_string(after_first) +
		 " 變成 " + std::to_string(after_many) + " bytes，空間沒有被回收").c_str());
}

// ================================================================ lseek

VFS_TEST(file, lseek_whence_modes) {
	TempArchive a("seek");
	REQUIRE(a.Ok());
	const std::vector<char> data = Blob(10000, 20);
	REQUIRE(WriteAll(a.h(), "s.bin", data));

	const int fd = vfs_file_open(a.h(), "s.bin", kRdWr);
	REQUIRE_GE(fd, 0);

	char buf[16];
	// SEEK_SET
	CHECK_EQ(vfs_file_lseek(a.h(), fd, 4096, 0), 0);
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 16), 16);
	CHECK_EQ(memcmp(buf, data.data() + 4096, 16), 0);

	// SEEK_CUR（上面讀掉 16，目前在 4112）
	CHECK_EQ(vfs_file_lseek(a.h(), fd, 100, 1), 0);
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 16), 16);
	CHECK_EQ(memcmp(buf, data.data() + 4212, 16), 0);

	// SEEK_END
	CHECK_EQ(vfs_file_lseek(a.h(), fd, -16, 2), 0);
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 16), 16);
	CHECK_EQ(memcmp(buf, data.data() + 10000 - 16, 16), 0);

	// 不合法的 whence
	CHECK_EQ(vfs_file_lseek(a.h(), fd, 0, 99), -1);

	vfs_file_close(a.h(), fd);
}

VFS_TEST(file, lseek_negative_clamps_to_zero) {
	TempArchive a("seekneg");
	REQUIRE(a.Ok());
	const std::vector<char> data = Blob(500, 21);
	REQUIRE(WriteAll(a.h(), "s.bin", data));

	const int fd = vfs_file_open(a.h(), "s.bin", kRdWr);
	REQUIRE_GE(fd, 0);
	CHECK_EQ(vfs_file_lseek(a.h(), fd, -9999, 0), 0);

	char buf[8];
	CHECK_EQ(vfs_file_read(a.h(), fd, buf, 8), 8);
	CHECK_EQ(memcmp(buf, data.data(), 8), 0);
	vfs_file_close(a.h(), fd);
}

VFS_TEST(file, lseek_past_eof_extends_and_zero_fills) {
	// seek 超過檔尾再寫入，中間的空洞必須讀出 0（POSIX sparse file 語意）。
	//
	// 這裡刻意先把封包「弄髒」：寫一個大檔、填滿可辨識的樣式、再刪掉，
	// 讓那些區塊回到空閒串列。接著建立的稀疏檔就會配到同一批區塊 ——
	// 如果沒有補零，空洞裡讀到的就是上一個檔案的內容。
	//
	// 不這樣做的話這個案例是非決定性的：新封包的區塊碰巧是 0 就會誤過。
	TempArchive a("sparse");
	REQUIRE(a.Ok());

	const size_t kDirty = 200000;
	std::vector<char> secret(kDirty);
	for (size_t i = 0; i < kDirty; ++i) secret[i] = static_cast<char>('S');
	REQUIRE(WriteAll(a.h(), "victim/secret.bin", secret));
	REQUIRE_EQ(vfs_file_unlink(a.h(), "victim/secret.bin"), 0);

	const int fd = vfs_file_create(a.h(), "sp.bin");
	REQUIRE_GE(fd, 0);
	const char head[] = "HEAD";
	CHECK_EQ(vfs_file_write(a.h(), fd, head, 4), 4);
	CHECK_EQ(vfs_file_lseek(a.h(), fd, 100000, 0), 0);
	const char tail[] = "TAIL";
	CHECK_EQ(vfs_file_write(a.h(), fd, tail, 4), 4);
	vfs_file_close(a.h(), fd);

	CHECK_EQ(FileSize(a.h(), "sp.bin"), 100004);

	std::vector<char> back;
	REQUIRE(ReadAll(a.h(), "sp.bin", &back));
	REQUIRE_EQ(static_cast<int>(back.size()), 100004);
	CHECK_EQ(memcmp(back.data(), "HEAD", 4), 0);
	CHECK_EQ(memcmp(back.data() + 100000, "TAIL", 4), 0);

	size_t nonzero = 0, leaked = 0;
	size_t first_bad = 0;
	for (size_t i = 4; i < 100000; ++i) {
		if (back[i] == 0) continue;
		if (!nonzero) first_bad = i;
		++nonzero;
		if (back[i] == 'S') ++leaked;      // 已刪除檔案的內容浮上來了
	}
	CHECK_MSG(nonzero == 0,
		("空洞沒有補零：offset " + std::to_string(first_bad) + " 起共 " +
		 std::to_string(nonzero) + " 個 byte 非零").c_str());
	CHECK_MSG(leaked == 0,
		("空洞裡讀到了已刪除檔案的內容 " + std::to_string(leaked) +
		 " bytes —— 舊資料會外洩給新檔案的讀者").c_str());
}

// ================================================================ append

VFS_TEST(file, append_mode_always_writes_at_end) {
	TempArchive a("append");
	REQUIRE(a.Ok());
	const std::vector<char> base = Blob(1000, 22);
	REQUIRE(WriteAll(a.h(), "ap.bin", base));

	const int fd = vfs_file_open(a.h(), "ap.bin", kRdWr | kAppend);
	REQUIRE_GE(fd, 0);
	vfs_file_lseek(a.h(), fd, 0, 0);               // 明確 seek 回開頭…
	const char extra[] = "APPENDED";
	CHECK_EQ(vfs_file_write(a.h(), fd, extra, 8), 8);  // …仍應寫在尾端
	vfs_file_close(a.h(), fd);

	CHECK_EQ(FileSize(a.h(), "ap.bin"), 1008);
	std::vector<char> back;
	REQUIRE(ReadAll(a.h(), "ap.bin", &back));
	REQUIRE_EQ(static_cast<int>(back.size()), 1008);
	CHECK_EQ(memcmp(back.data(), base.data(), 1000), 0);
	CHECK_EQ(memcmp(back.data() + 1000, extra, 8), 0);
}

// ================================================================ unlink / link

VFS_TEST(file, unlink_removes_file) {
	TempArchive a("unlink");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "u/a.bin", Blob(1234, 23)));
	REQUIRE(WriteAll(a.h(), "u/b.bin", Blob(1234, 24)));

	CHECK_EQ(vfs_file_unlink(a.h(), "u/a.bin"), 0);
	CHECK_EQ(vfs_file_exists(a.h(), "u/a.bin"), 0);
	CHECK_MSG(vfs_file_exists(a.h(), "u/b.bin") != 0, "刪 a 不可以連 b 一起消失");

	REQUIRE(a.Reopen() != nullptr);
	CHECK_EQ(vfs_file_exists(a.h(), "u/a.bin"), 0);
	CHECK_MSG(vfs_file_exists(a.h(), "u/b.bin") != 0, "重開後 b 仍在");
}

VFS_TEST(file, unlink_missing_fails) {
	TempArchive a("unlink_miss");
	REQUIRE(a.Ok());
	CHECK_EQ(vfs_file_unlink(a.h(), "nope.bin"), -1);
}

VFS_TEST(file, recreate_after_unlink) {
	TempArchive a("recreate");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "x.bin", Blob(4000, 25)));
	REQUIRE_EQ(vfs_file_unlink(a.h(), "x.bin"), 0);

	const std::vector<char> v2 = Blob(7000, 26);
	CHECK(WriteAll(a.h(), "x.bin", v2));
	std::vector<char> back;
	CHECK(ReadAll(a.h(), "x.bin", &back));
	CHECK_BLOB_EQ(back, v2);
}

VFS_TEST(file, link_shares_content) {
	TempArchive a("link");
	REQUIRE(a.Ok());
	const std::vector<char> data = Blob(3000, 27);
	REQUIRE(WriteAll(a.h(), "orig.bin", data));

	REQUIRE_EQ(vfs_file_link(a.h(), "orig.bin", "alias.bin"), 0);
	CHECK_MSG(vfs_file_exists(a.h(), "alias.bin") != 0, "link 之後新名字應存在");
	CHECK_EQ(FileSize(a.h(), "alias.bin"), 3000);

	std::vector<char> back;
	CHECK(ReadAll(a.h(), "alias.bin", &back));
	CHECK_BLOB_EQ(back, data);

	// 刪掉其中一個名字，另一個必須還讀得到（refcount 語意）
	REQUIRE_EQ(vfs_file_unlink(a.h(), "orig.bin"), 0);
	CHECK_EQ(vfs_file_exists(a.h(), "orig.bin"), 0);
	CHECK_MSG(vfs_file_exists(a.h(), "alias.bin") != 0, "刪掉 orig 後 alias 應仍在");
	CHECK_MSG(ReadAll(a.h(), "alias.bin", &back), "刪掉 orig 後 alias 仍可讀");
	CHECK_BLOB_EQ(back, data);
}

// ================================================================ fd 管理

VFS_TEST(file, invalid_fd_rejected) {
	TempArchive a("badfd");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "f.bin", Blob(10, 28)));

	char buf[16];
	CHECK_EQ(vfs_file_read(a.h(), -1, buf, 16), -1);
	CHECK_EQ(vfs_file_read(a.h(), 99999, buf, 16), -1);
	CHECK_EQ(vfs_file_write(a.h(), -1, buf, 16), -1);
	CHECK_EQ(vfs_file_lseek(a.h(), -1, 0, 0), -1);
}

VFS_TEST(file, open_missing_file_fails) {
	TempArchive a("openmiss");
	REQUIRE(a.Ok());
	CHECK_EQ(vfs_file_open(a.h(), "does/not/exist.bin", kRdWr), -1);
	CHECK_EQ(vfs_errno, VFS_ERR_FILE_NOT_FOUND);
}

VFS_TEST(file, null_args_rejected) {
	TempArchive a("nullarg");
	REQUIRE(a.Ok());
	char buf[16];
	CHECK_EQ(vfs_file_open(a.h(), nullptr, kRdWr), -1);
	CHECK_EQ(vfs_file_open(nullptr, "x", kRdWr), -1);
	CHECK_EQ(vfs_file_read(nullptr, 0, buf, 16), -1);
	CHECK_EQ(vfs_file_write(nullptr, 0, buf, 16), -1);
}

VFS_TEST(file, many_concurrent_fds) {
	// 打包工具會同時開很多檔；fd 陣列要能成長
	TempArchive a("manyfd");
	REQUIRE(a.Ok());

	const int kN = 64;
	for (int i = 0; i < kN; ++i)
		REQUIRE(WriteAll(a.h(), "m/" + std::to_string(i) + ".bin", Blob(100, static_cast<unsigned>(i))));

	std::vector<int> fds;
	for (int i = 0; i < kN; ++i) {
		const int fd = vfs_file_open(a.h(), ("m/" + std::to_string(i) + ".bin").c_str(), kRdWr);
		CHECK_GE(fd, 0);
		if (fd >= 0) fds.push_back(fd);
	}
	CHECK_EQ(static_cast<int>(fds.size()), kN);

	// 每個 fd 的讀寫位置必須彼此獨立
	bool all_distinct_ok = true;
	for (size_t i = 0; i < fds.size(); ++i) {
		vfs_file_lseek(a.h(), fds[i], static_cast<int>(i), 0);
	}
	for (size_t i = 0; i < fds.size(); ++i) {
		char c = 0;
		const std::vector<char> expect = Blob(100, static_cast<unsigned>(i));
		if (vfs_file_read(a.h(), fds[i], &c, 1) != 1 || c != expect[i]) {
			all_distinct_ok = false;
			break;
		}
	}
	CHECK_MSG(all_distinct_ok, "各 fd 的 offset 互相污染了");

	for (int fd : fds) CHECK_EQ(vfs_file_close(a.h(), fd), 0);
}

// ================================================================ glob

VFS_TEST(file, glob_patterns) {
	TempArchive a("glob");
	REQUIRE(a.Ok());
	const char* keys[] = {
		"character/hero.ca", "character/villain.ca", "character/hero.png",
		"map/town.map", "map/dungeon.map",
		"ui/button.png", "readme.txt",
	};
	for (const char* k : keys) REQUIRE(WriteAll(a.h(), k, Blob(64, 1)));

	CHECK_EQ(ListKeys(a.h(), "*").size(), static_cast<size_t>(7));
	CHECK_EQ(ListKeys(a.h(), "character/*").size(), static_cast<size_t>(3));
	CHECK_EQ(ListKeys(a.h(), "character/*.ca").size(), static_cast<size_t>(2));
	CHECK_EQ(ListKeys(a.h(), "map/*.map").size(), static_cast<size_t>(2));
	CHECK_EQ(ListKeys(a.h(), "*.png").size(), static_cast<size_t>(2));
	CHECK_EQ(ListKeys(a.h(), "character/hero.ca").size(), static_cast<size_t>(1));

	VfsGlobResults g = {};
	CHECK_MSG(vfs_glob(a.h(), "zzz/*", 0, nullptr, &g) == 1, "無匹配應回 GLOB_NOMATCH(1)");
	vfs_glob_free(&g);
}

VFS_TEST(file, glob_question_mark) {
	TempArchive a("globq");
	REQUIRE(a.Ok());
	for (const char* k : { "a1.bin", "a2.bin", "a10.bin", "b1.bin" })
		REQUIRE(WriteAll(a.h(), k, Blob(8, 1)));

	CHECK_EQ(ListKeys(a.h(), "a?.bin").size(), static_cast<size_t>(2));
	CHECK_EQ(ListKeys(a.h(), "?1.bin").size(), static_cast<size_t>(2));
}

VFS_TEST(file, glob_results_are_readable) {
	// glob 回傳的鍵必須真的能拿去 open —— 聽起來理所當然，
	// 但 nt_idx 溢位那類 bug 正好會在這裡現形（列得出來卻讀不到）
	TempArchive a("globread");
	REQUIRE(a.Ok());
	for (int i = 0; i < 50; ++i)
		REQUIRE(WriteAll(a.h(), "g/" + std::to_string(i) + ".bin",
			Blob(static_cast<size_t>(100 + i), static_cast<unsigned>(i))));

	const std::vector<std::string> keys = ListKeys(a.h(), "g/*");
	REQUIRE_EQ(keys.size(), static_cast<size_t>(50));

	int unreadable = 0;
	for (const std::string& k : keys) {
		std::vector<char> back;
		if (!ReadAll(a.h(), k, &back)) ++unreadable;
	}
	CHECK_MSG(unreadable == 0,
		("glob 列出的檔案有 " + std::to_string(unreadable) + " 個讀不到").c_str());
}

// ================================================================ 檔名

VFS_TEST(file, key_lengths) {
	// KeyNode 的片段是 60 bytes，長檔名會跨多個片段
	TempArchive a("keylen");
	REQUIRE(a.Ok());

	const int lens[] = { 1, 10, 59, 60, 61, 119, 120, 121, 250 };
	for (int n : lens) {
		const std::string key(static_cast<size_t>(n), 'k');
		const std::vector<char> data = Blob(200, static_cast<unsigned>(n));
		CHECK_MSG(WriteAll(a.h(), key, data), ("檔名長度 " + std::to_string(n)).c_str());
		std::vector<char> back;
		CHECK_MSG(ReadAll(a.h(), key, &back), ("檔名長度 " + std::to_string(n) + " 讀回").c_str());
		CHECK_BLOB_EQ(back, data);
	}
}

VFS_TEST(file, keys_with_shared_prefixes) {
	// Patricia trie 最容易出錯的地方：一個鍵是另一個鍵的前綴
	TempArchive a("prefix");
	REQUIRE(a.Ok());
	const char* keys[] = { "a", "ab", "abc", "abcd", "abcde", "b", "ba" };
	for (const char* k : keys)
		REQUIRE(WriteAll(a.h(), k, Blob(strlen(k) * 10 + 1, static_cast<unsigned>(k[0]))));

	for (const char* k : keys) {
		std::vector<char> back;
		CHECK_MSG(ReadAll(a.h(), k, &back), (std::string("讀回 ") + k).c_str());
		CHECK_BLOB_EQ(back, Blob(strlen(k) * 10 + 1, static_cast<unsigned>(k[0])));
	}
	CHECK_EQ(ListKeys(a.h()).size(), static_cast<size_t>(7));

	// 刪掉中間那個，其他必須不受影響
	REQUIRE_EQ(vfs_file_unlink(a.h(), "abc"), 0);
	CHECK_EQ(vfs_file_exists(a.h(), "abc"), 0);
	for (const char* k : { "a", "ab", "abcd", "abcde", "b", "ba" })
		CHECK_MSG(vfs_file_exists(a.h(), k) != 0, (std::string(k) + " 被 abc 的刪除波及").c_str());
}

VFS_TEST(file, keys_are_case_sensitive_at_api_level) {
	// 正規化是 CLI 層的事；API 層不該自作主張
	TempArchive a("case");
	REQUIRE(a.Ok());
	REQUIRE(WriteAll(a.h(), "lower.bin", Blob(10, 1)));
	CHECK_EQ(vfs_file_exists(a.h(), "LOWER.BIN"), 0);
}

// ================================================================ 大量檔案

VFS_TEST(file, thousand_files_all_readable) {
	TempArchive a("thousand");
	REQUIRE(a.Ok());

	const int kN = 1000;
	for (int i = 0; i < kN; ++i) {
		char key[64];
		std::snprintf(key, sizeof(key), "bulk/%04d.bin", i);
		REQUIRE(WriteAll(a.h(), key, Blob(static_cast<size_t>(50 + (i % 400)), static_cast<unsigned>(i))));
	}
	REQUIRE(a.Reopen() != nullptr);

	int bad_size = 0, bad_content = 0;
	for (int i = 0; i < kN; ++i) {
		char key[64];
		std::snprintf(key, sizeof(key), "bulk/%04d.bin", i);
		const std::vector<char> expect = Blob(static_cast<size_t>(50 + (i % 400)), static_cast<unsigned>(i));
		if (FileSize(a.h(), key) != static_cast<int>(expect.size())) { ++bad_size; continue; }
		std::vector<char> back;
		if (!ReadAll(a.h(), key, &back) || back != expect) ++bad_content;
	}
	CHECK_MSG(bad_size == 0, ("有 " + std::to_string(bad_size) + " 個檔大小不對").c_str());
	CHECK_MSG(bad_content == 0, ("有 " + std::to_string(bad_content) + " 個檔內容不對").c_str());
	CHECK_EQ(ListKeys(a.h(), "bulk/*").size(), static_cast<size_t>(kN));
}

// ================================================================ 交錯操作

VFS_TEST(file, interleaved_write_delete_rewrite) {
	// 模擬真實的增量更新：寫一批、刪一半、再寫回去，內容不可以互相污染
	TempArchive a("churn");
	REQUIRE(a.Ok());

	const int kN = 200;
	for (int i = 0; i < kN; ++i)
		REQUIRE(WriteAll(a.h(), "c/" + std::to_string(i),
			Blob(static_cast<size_t>(1000 + i * 7), static_cast<unsigned>(i))));

	// 刪一半。失敗時要指名是哪個鍵、errno 是多少 —— 互為前綴的鍵
	// （"c/1" 是 "c/10" 的前綴）正是 dt/delete_does_not_disturb_siblings 的引爆點。
	int unlink_failed = 0;
	std::string first_fail;
	int first_errno = 0;
	for (int i = 0; i < kN; i += 2) {
		const std::string key = "c/" + std::to_string(i);
		if (vfs_file_unlink(a.h(), key.c_str()) != 0) {
			++unlink_failed;
			if (first_fail.empty()) { first_fail = key; first_errno = vfs_errno; }
		}
	}
	REQUIRE_MSG(unlink_failed == 0,
		("有 " + std::to_string(unlink_failed) + " 個 unlink 失敗，第一個是 \"" +
		 first_fail + "\"（vfs_errno=" + std::to_string(first_errno) +
		 "，11=查不到 / 14=NT 索引無效 / 18=DT 刪除失敗）").c_str());

	for (int i = 0; i < kN; i += 2)
		REQUIRE(WriteAll(a.h(), "c/" + std::to_string(i),
			Blob(static_cast<size_t>(3000 + i * 3), static_cast<unsigned>(i + 10000))));

	REQUIRE(a.Reopen() != nullptr);

	int bad = 0;
	for (int i = 0; i < kN; ++i) {
		const std::vector<char> expect = (i % 2 == 0)
			? Blob(static_cast<size_t>(3000 + i * 3), static_cast<unsigned>(i + 10000))
			: Blob(static_cast<size_t>(1000 + i * 7), static_cast<unsigned>(i));
		std::vector<char> back;
		if (!ReadAll(a.h(), "c/" + std::to_string(i), &back) || back != expect) ++bad;
	}
	CHECK_MSG(bad == 0, ("churn 之後有 " + std::to_string(bad) + " 個檔內容錯誤").c_str());
	CHECK_EQ(ListKeys(a.h(), "c/*").size(), static_cast<size_t>(kN));
}
