// test_harness.cpp — VFS_TEST 註冊表與執行器（純標準 C++17）
//
// 輸出刻意用純文字標籤而非終端色碼：主機平台不一定有 TTY，CI 也常把輸出導到
// 檔案，色碼只會變成亂碼。對齊的 [ ok ] / [FAIL] 在哪裡都讀得懂。

#include <vfs_test.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace vfstest {
namespace {

// 註冊表放在函式內 static：Registrar 是全域物件，會在 main 之前執行，
// 直接用全域 vector 會踩到跨 TU 的靜態初始化順序問題。
std::vector<TestCase>& Registry() {
	static std::vector<TestCase> r;
	return r;
}

struct RunState {
	int checks_passed = 0;
	int checks_failed = 0;
	std::vector<std::string> failures;
};
RunState g_state;

// 路徑只留檔名，'/' 與 '\\' 都要處理（原始碼可能在任一平台編譯）
const char* BaseName(const char* path) {
	const char* last = path;
	for (const char* p = path; *p; ++p)
		if (*p == '/' || *p == '\\') last = p + 1;
	return last;
}

} // namespace

void Register(const TestCase& tc) { Registry().push_back(tc); }

void ReportPass() { ++g_state.checks_passed; }

void ReportFail(const char* file, int line, const char* expr, const std::string& detail) {
	++g_state.checks_failed;
	char head[512];
	std::snprintf(head, sizeof(head), "%s:%d  %s", BaseName(file), line, expr);
	std::string msg = head;
	if (!detail.empty()) msg += "\n        " + detail;
	g_state.failures.push_back(msg);
}

// --- ToStr ---
namespace {
template <class T>
std::string Fmt(const char* spec, T v) {
	char b[64];
	std::snprintf(b, sizeof(b), spec, v);
	return b;
}
} // namespace

std::string ToStr(bool v)               { return v ? "true" : "false"; }
std::string ToStr(int v)                { return Fmt("%d", v); }
std::string ToStr(long v)               { return Fmt("%ld", v); }
std::string ToStr(long long v)          { return Fmt("%lld", v); }
std::string ToStr(unsigned v)           { return Fmt("%u", v); }
std::string ToStr(unsigned long v)      { return Fmt("%lu", v); }
std::string ToStr(unsigned long long v) { return Fmt("%llu", v); }
std::string ToStr(double v)             { return Fmt("%.6g", v); }
std::string ToStr(const char* v)        { return v ? std::string("\"") + v + "\"" : "(null)"; }
std::string ToStr(const std::string& v) { return "\"" + v + "\""; }
std::string ToStr(const void* v)        { return Fmt("%p", v); }

std::string DiffBlob(const std::vector<char>& actual, const std::vector<char>& expect) {
	char b[512];
	if (actual.size() != expect.size()) {
		std::snprintf(b, sizeof(b), "長度不同：實際 %llu bytes，期望 %llu bytes",
			static_cast<unsigned long long>(actual.size()),
			static_cast<unsigned long long>(expect.size()));
		return b;
	}
	for (size_t i = 0; i < actual.size(); ++i) {
		if (actual[i] == expect[i]) continue;
		// 統計差異總量，用來區分「單點損毀」與「整段錯位」
		size_t ndiff = 0;
		for (size_t j = i; j < actual.size(); ++j)
			if (actual[j] != expect[j]) ++ndiff;
		std::snprintf(b, sizeof(b),
			"內容不同：首個差異在 offset %llu（實際 0x%02X，期望 0x%02X），"
			"自該點起共 %llu / %llu bytes 不同",
			static_cast<unsigned long long>(i),
			static_cast<unsigned char>(actual[i]),
			static_cast<unsigned char>(expect[i]),
			static_cast<unsigned long long>(ndiff),
			static_cast<unsigned long long>(actual.size() - i));
		return b;
	}
	return std::string();
}

void ListSuites() {
	std::map<std::string, int> counts;
	for (const TestCase& tc : Registry()) counts[tc.suite]++;
	std::printf("可用的測試 suite：\n");
	for (const auto& kv : counts)
		std::printf("  %-12s %d 個案例\n", kv.first.c_str(), kv.second);
}

int RunAll(const std::vector<std::string>& suite_filter, bool verbose) {
	auto wanted = [&](const char* suite) {
		if (suite_filter.empty()) return true;
		for (const std::string& s : suite_filter)
			if (s == suite) return true;
		return false;
	};

	int n_pass = 0, n_fail = 0, n_xfail = 0, n_xpass = 0;
	std::string current_suite;
	std::vector<std::string> needs_attention;

	for (const TestCase& tc : Registry()) {
		if (!wanted(tc.suite)) continue;

		if (current_suite != tc.suite) {
			current_suite = tc.suite;
			std::printf("\n[%s]\n", current_suite.c_str());
		}

		g_state = RunState();
		tc.fn();

		const bool failed = g_state.checks_failed > 0;
		const bool is_xfail = (tc.xfail_reason != nullptr);

		const char* tag;
		if (is_xfail && failed)       { tag = "[xfail]"; ++n_xfail; }
		else if (is_xfail && !failed) { tag = "[XPASS]"; ++n_xpass; }
		else if (failed)              { tag = "[FAIL ]"; ++n_fail; }
		else                          { tag = "[ ok  ]"; ++n_pass; }

		std::printf("  %s %-44s %d 項斷言\n", tag, tc.name,
			g_state.checks_passed + g_state.checks_failed);

		// XFAIL 的失敗細節平常不用看，加 -v 才印
		if (failed && (!is_xfail || verbose)) {
			for (const std::string& f : g_state.failures)
				std::printf("        %s\n", f.c_str());
		}
		if (is_xfail && failed && !verbose)
			std::printf("        已知問題：%s\n", tc.xfail_reason);
		if (is_xfail && !failed) {
			std::printf("        這個案例本來預期失敗（%s），現在卻通過了。\n", tc.xfail_reason);
			std::printf("        如果 bug 已修好，請把 VFS_TEST_XFAIL 改回 VFS_TEST。\n");
			needs_attention.push_back(std::string(tc.suite) + "/" + tc.name + "  (XPASS：該把標記拿掉了)");
		}
		if (failed && !is_xfail)
			needs_attention.push_back(std::string(tc.suite) + "/" + tc.name);
	}

	std::printf("\n----------------------------------------------\n");
	std::printf("通過 %d", n_pass);
	if (n_xfail) std::printf("，已知失敗 %d", n_xfail);
	if (n_fail)  std::printf("，失敗 %d", n_fail);
	if (n_xpass) std::printf("，意外通過 %d", n_xpass);
	std::printf("\n");

	if (!needs_attention.empty()) {
		std::printf("\n需要處理：\n");
		for (const std::string& n : needs_attention) std::printf("  - %s\n", n.c_str());
	}
	return (n_fail || n_xpass) ? 1 : 0;
}

} // namespace vfstest
