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

bool ReexecX64IfNeeded(int* exitCode, bool wait) {
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

    if (!wait) {
        // GUI：不等待 —— 父进程（32 位）立即退出，只留 64 位那份在跑。
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        if (exitCode)
            *exitCode = 0;
        return true;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    if (exitCode)
        *exitCode = static_cast<int>(code);
    return true;
}

std::wstring AppDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t pos = s.find_last_of(L"\\/");
    std::wstring dir =
        pos == std::wstring::npos ? std::wstring(L".") : s.substr(0, pos);
    // 启动器布局（见 src/launcher/launcher.cpp）：真正的程序在 <root>\x86\ 或 <root>\x64\ 下，
    // 而 bootfiles/ skin/ lang/ logs/ 等**与位数无关**的资源放在 <root>，所以"应用目录"要上移一级。
    // 仅在目录名恰为 x86/x64 **且**上一级确实是应用根（含 version.json 或 bootfiles）时才上移，
    // 保证老的扁平布局（exe 直接在根目录）行为不变。
    size_t p2 = dir.find_last_of(L"\\/");
    if (p2 != std::wstring::npos) {
        std::wstring leaf = dir.substr(p2 + 1);
        for (size_t i = 0; i < leaf.size(); ++i) {
            wchar_t c = leaf[i];
            if (c >= L'A' && c <= L'Z')
                leaf[i] = (wchar_t)(c - L'A' + L'a');
        }
        if (leaf == L"x86" || leaf == L"x64") {
            std::wstring parent = dir.substr(0, p2);
            if (GetFileAttributesW((parent + L"\\version.json").c_str()) !=
                    INVALID_FILE_ATTRIBUTES ||
                GetFileAttributesW((parent + L"\\bootfiles").c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                return parent;
        }
    }
    return dir;
}

}  // namespace sysrecover
