// vfs_test.h — VFS 單元測試 harness
//
// 刻意做成零相依、單一 exe 的形式：測試案例用 VFS_TEST 巨集在載入期自我註冊，
// `vfs test [suite...]` 直接跑。設計目標是「壞掉的時候看得懂是哪裡壞」，
// 所以每個斷言都會印出實際值 vs 期望值，而不是只說 assertion failed。
//
// 已知 bug 用 VFS_TEST_XFAIL 標記：預期失敗，真的失敗算 PASS(xfail)，
// 意外通過算 XPASS 並讓整輪失敗 —— 修好 bug 時會被逼著回來改掉標記。
#ifndef VFS_TEST_H
#define VFS_TEST_H

#include <string>
#include <vector>

namespace vfstest {

struct TestCase {
	const char* suite;
	const char* name;
	void (*fn)();
	const char* xfail_reason;   // 非 nullptr = 已知 bug，預期失敗
	const char* file;
	int line;
};

void Register(const TestCase& tc);

// suite_filter 為空代表全跑。回傳 0 表示全部符合預期。
int RunAll(const std::vector<std::string>& suite_filter, bool verbose);

// 列出所有已註冊的 suite（給 `vfs test --list`）
void ListSuites();

// --- 斷言後端（巨集用，一般不直接呼叫）---
void ReportPass();
void ReportFail(const char* file, int line, const char* expr, const std::string& detail);

// 各型別的可讀化
std::string ToStr(bool v);
std::string ToStr(int v);
std::string ToStr(long v);
std::string ToStr(long long v);
std::string ToStr(unsigned v);
std::string ToStr(unsigned long v);
std::string ToStr(unsigned long long v);
std::string ToStr(double v);
std::string ToStr(const char* v);
std::string ToStr(const std::string& v);
std::string ToStr(const void* v);

struct Registrar { explicit Registrar(const TestCase& tc) { Register(tc); } };

// 二進位內容比對：回傳空字串表示相同，否則描述第一個差異點
std::string DiffBlob(const std::vector<char>& actual, const std::vector<char>& expect);

} // namespace vfstest

// ---------------------------------------------------------------- 註冊巨集

#define VFS_TEST_IMPL_(suite_, name_, reason_)                                     \
	static void vfstest_fn_##suite_##_##name_();                                   \
	static ::vfstest::Registrar vfstest_reg_##suite_##_##name_(                    \
		::vfstest::TestCase{ #suite_, #name_, &vfstest_fn_##suite_##_##name_,      \
		                     reason_, __FILE__, __LINE__ });                       \
	static void vfstest_fn_##suite_##_##name_()

// 一般測試
#define VFS_TEST(suite_, name_)          VFS_TEST_IMPL_(suite_, name_, nullptr)
// 已知 bug：預期失敗
#define VFS_TEST_XFAIL(suite_, name_, reason_) VFS_TEST_IMPL_(suite_, name_, reason_)

// ---------------------------------------------------------------- 斷言巨集
//
// CHECK_* 失敗後繼續跑（一個測試可以回報多個問題）
// REQUIRE_* 失敗後立刻結束該測試（前提不成立，再跑下去只會噴一堆雜訊）

#define VFS_CHECK_AT_(ok_, expr_, detail_)                                         \
	do {                                                                           \
		if (ok_) ::vfstest::ReportPass();                                          \
		else     ::vfstest::ReportFail(__FILE__, __LINE__, expr_, detail_);        \
	} while (0)

#define CHECK(expr_)                                                               \
	VFS_CHECK_AT_(!!(expr_), #expr_, std::string())

#define REQUIRE(expr_)                                                             \
	do { if (!(expr_)) {                                                           \
		::vfstest::ReportFail(__FILE__, __LINE__, #expr_, "前提不成立，中止此測試"); \
		return;                                                                    \
	} ::vfstest::ReportPass(); } while (0)

#define VFS_CMP_(a_, b_, op_, opname_, fatal_)                                     \
	do {                                                                           \
		auto vfs_a_ = (a_);                                                        \
		auto vfs_b_ = (b_);                                                        \
		if (!(vfs_a_ op_ vfs_b_)) {                                                \
			::vfstest::ReportFail(__FILE__, __LINE__, #a_ " " opname_ " " #b_,     \
				"實際 = " + ::vfstest::ToStr(vfs_a_) +                             \
				"，期望 " opname_ " " + ::vfstest::ToStr(vfs_b_));                 \
			if (fatal_) return;                                                    \
		} else ::vfstest::ReportPass();                                            \
	} while (0)

#define CHECK_EQ(a_, b_)   VFS_CMP_(a_, b_, ==, "==", false)
#define CHECK_NE(a_, b_)   VFS_CMP_(a_, b_, !=, "!=", false)
#define CHECK_LT(a_, b_)   VFS_CMP_(a_, b_, < , "<",  false)
#define CHECK_LE(a_, b_)   VFS_CMP_(a_, b_, <=, "<=", false)
#define CHECK_GT(a_, b_)   VFS_CMP_(a_, b_, > , ">",  false)
#define CHECK_GE(a_, b_)   VFS_CMP_(a_, b_, >=, ">=", false)

#define REQUIRE_EQ(a_, b_) VFS_CMP_(a_, b_, ==, "==", true)
#define REQUIRE_NE(a_, b_) VFS_CMP_(a_, b_, !=, "!=", true)
#define REQUIRE_LT(a_, b_) VFS_CMP_(a_, b_, < , "<",  true)
#define REQUIRE_LE(a_, b_) VFS_CMP_(a_, b_, <=, "<=", true)
#define REQUIRE_GT(a_, b_) VFS_CMP_(a_, b_, > , ">",  true)
#define REQUIRE_GE(a_, b_) VFS_CMP_(a_, b_, >=, ">=", true)

// 二進位內容比對（印出第一個差異的位移與前後 bytes）
#define CHECK_BLOB_EQ(actual_, expect_)                                            \
	do {                                                                           \
		std::string vfs_d_ = ::vfstest::DiffBlob((actual_), (expect_));            \
		if (vfs_d_.empty()) ::vfstest::ReportPass();                               \
		else ::vfstest::ReportFail(__FILE__, __LINE__,                             \
			#actual_ " == " #expect_, vfs_d_);                                     \
	} while (0)

// 附一句話說明的自由斷言
#define CHECK_MSG(expr_, msg_)                                                     \
	VFS_CHECK_AT_(!!(expr_), #expr_, std::string(msg_))

// 同 CHECK_MSG，但失敗就中止此測試
#define REQUIRE_MSG(expr_, msg_)                                                   \
	do { if (!(expr_)) {                                                           \
		::vfstest::ReportFail(__FILE__, __LINE__, #expr_, std::string(msg_));      \
		return;                                                                    \
	} ::vfstest::ReportPass(); } while (0)

#endif // VFS_TEST_H
