// 进程执行器实现。
#include "process.h"

#include <windows.h>

#include <vector>

namespace sysrecover {
namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

std::string OemToUtf8(const std::string& oem) {
    if (oem.empty())
        return {};
    UINT cp = GetOEMCP();
    int n = MultiByteToWideChar(cp, 0, oem.c_str(), -1, nullptr, 0);
    if (n <= 0)
        return oem;  // 兜底：原样返回（ASCII 不受影响）
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(cp, 0, oem.c_str(), -1, w.data(), n);
    return ToUtf8(w);
}

struct Handle {
    HANDLE h = nullptr;
    explicit Handle(HANDLE v = nullptr) : h(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    void reset() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = nullptr;
    }
};

}  // namespace

std::wstring SysToolPath(const wchar_t* exeName) {
    wchar_t sys[MAX_PATH] = {};
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0)
        return exeName;
    return std::wstring(sys) + L"\\" + exeName;
}

int RunProcess(const std::wstring& exe, const std::wstring& args,
               std::string& outUtf8) {
    outUtf8.clear();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rRaw = nullptr, wRaw = nullptr;
    if (!CreatePipe(&rRaw, &wRaw, &sa, 0))
        return -1;
    Handle rPipe(rRaw), wPipe(wRaw);
    // 读端不继承，避免子进程持有导致读阻塞
    SetHandleInformation(rPipe.h, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wPipe.h;
    si.hStdError = wPipe.h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};

    // vector<wchar_t> 保证可写缓冲（CreateProcessW 要求）
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    if (!CreateProcessW(exe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return -1;
    Handle proc(pi.hProcess), thread(pi.hThread);

    wPipe = Handle();  // 父进程关闭写端，否则读不到 EOF
    std::string raw;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rPipe.h, buf, sizeof(buf), &n, nullptr) && n > 0)
        raw.append(buf, n);

    WaitForSingleObject(proc.h, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(proc.h, &code);
    outUtf8 = OemToUtf8(raw);
    return static_cast<int>(code);
}

}  // namespace sysrecover
