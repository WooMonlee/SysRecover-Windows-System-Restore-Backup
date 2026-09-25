#include "selfarch.h"

#include <windows.h>

#include <string>

namespace sysrecover {
namespace {

std::wstring DirOf(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : path.substr(0, k);
}

std::wstring BaseOf(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? path : path.substr(k + 1);
}

}  // namespace

bool ReexecX64IfNeeded(int* exitCode) {
    if (sizeof(void*) != 4)
        return false;  // 已经是 64 位，无需切换
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);  // WOW64 下也返回原生架构
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64 &&
        si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_IA64)
        return false;  // 真正的 32 位系统 → 本进程继续跑

    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0)
        return false;
    std::wstring exePath = self;
    std::wstring dir = DirOf(exePath);
    std::wstring name = BaseOf(exePath);
    std::wstring target = dir + L"\\x64\\" + name;
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;  // 没有 x64 版（例如只发了 x86 包）→ 本进程继续跑

    // 命令行：去掉本程序名那个 token，其余原样转发（保留引号/空格）。
    std::wstring cmdline = GetCommandLineW();
    size_t i = 0;
    while (i < cmdline.size() && cmdline[i] == L' ')
        ++i;
    if (i < cmdline.size() && cmdline[i] == L'"') {
        size_t j = cmdline.find(L'"', i + 1);
        i = (j == std::wstring::npos) ? cmdline.size() : j + 1;
    } else {
        size_t j = cmdline.find(L' ', i);
        i = (j == std::wstring::npos) ? cmdline.size() : j;
    }
    std::wstring child = L"\"" + target + L"\"" + cmdline.substr(i);

    // 继承标准句柄：控制台/管道/重定向都能透传（GUI 进程句柄为 NULL，不设该标志）。
    STARTUPINFOW si2 = {};
    si2.cb = sizeof(si2);
    HANDLE hi = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE he = GetStdHandle(STD_ERROR_HANDLE);
    if (hi || ho || he) {
        si2.dwFlags |= STARTF_USESTDHANDLES;
        si2.hStdInput = hi;
        si2.hStdOutput = ho;
        si2.hStdError = he;
    }
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(target.c_str(), child.empty() ? nullptr : &child[0],
                        nullptr, nullptr, TRUE, 0, nullptr, dir.c_str(), &si2,
                        &pi))
        return false;  // 转发失败 → 退回本进程（不要把用户挡在门外）

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (exitCode)
        *exitCode = static_cast<int>(code);
    return true;
}

}  // namespace sysrecover
