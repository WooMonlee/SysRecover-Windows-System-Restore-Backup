#include "tiny_test.h"

#include <windows.h>

#include <cstdio>

namespace tinytest {

std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}

int& FailCount() {
    static int n = 0;
    return n;
}

int& CheckCount() {
    static int n = 0;
    return n;
}

void ReportFail(const char* file, int line, const std::string& msg) {
    ++FailCount();
    std::fprintf(stderr, "  [FAIL] %s:%d  %s\n", file, line, msg.c_str());
}

std::string Show(const std::string& s) { return s; }
std::string Show(const char* s) { return s ? s : "(null)"; }

namespace {
std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}
}  // namespace

std::string Show(const std::wstring& s) { return W2U(s); }
std::string Show(const wchar_t* s) { return s ? W2U(s) : "(null)"; }

}  // namespace tinytest
