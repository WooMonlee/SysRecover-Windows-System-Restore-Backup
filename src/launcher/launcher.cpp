// SysRecover 总入口（x64）：图标由 .rc 提供，真正的程序在 x64\ 子目录。
// 根目录 SysRecoverUI.exe / SysRecover.exe 都是这个小型 x64 启动器；
// 32 位入口是 SysRecoverUI_x86.exe / SysRecover_x86.exe。
// 这样 x64-only PE（没有 WOW64）也能直接启动，不需要 32 位自举。
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

std::wstring DirOf(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
}

std::wstring StripExeToken(const std::wstring& cmdline) {
    size_t i = 0;
    while (i < cmdline.size() && cmdline[i] == L' ') ++i;
    if (i < cmdline.size() && cmdline[i] == L'"') {
        size_t j = cmdline.find(L'"', i + 1);
        return j == std::wstring::npos ? std::wstring() : cmdline.substr(j + 1);
    }
    size_t j = cmdline.find(L' ', i);
    return j == std::wstring::npos ? std::wstring() : cmdline.substr(j);
}

#ifdef ZJ_LAUNCH_GUI
const wchar_t* kTarget = L"x64\\SysRecoverUI.exe";
const wchar_t* kLabel = L"SysRecoverUI";
#elif defined(ZJ_LAUNCH_CHECK)
const wchar_t* kTarget = L"x64\\SysRecoverCheck.exe";
const wchar_t* kLabel = L"SysRecoverCheck";

// 父进程是不是「自动化宿主」（命令解释器 / 计划任务 / 我们的门禁脚本）。
// 这类调用方要的是"跑完就退、退出码可读"，绝不能停在那儿等回车。
bool StartedByAutomationHost() {
    static const wchar_t* kHosts[] = {
        L"cmd.exe",   L"powershell.exe", L"pwsh.exe",   L"python.exe",
        L"pythonw.exe", L"cscript.exe",  L"wscript.exe", L"svchost.exe",
        L"services.exe",
    };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    DWORD parent = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == GetCurrentProcessId()) {
                parent = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    bool automation = false;
    if (parent != 0) {
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID != parent)
                    continue;
                for (const wchar_t* h : kHosts) {
                    if (_wcsicmp(pe.szExeFile, h) == 0) {
                        automation = true;
                        break;
                    }
                }
                break;
            } while (Process32NextW(snap, &pe));
        }
    }
    CloseHandle(snap);
    return automation;
}

// 「双击打开」时控制台是本启动器独占的（进程数 = 1），子进程会因为
// GetConsoleProcessList() 看到两个进程（启动器 + 自己）而误判成"脚本调用"：
// 不弹文件选择框、跑完立刻随控制台一起消失（用户只看到窗口一闪）。
// 这里给子进程留一句话「这个控制台现在是你的」——环境变量随 CreateProcessW
// 自动继承，子进程据此仍然弹框、并在结束时等回车。
// 从 cmd/批处理/计划任务调用时要么控制台进程数 ≥ 2、要么父进程是自动化宿主，
// 都不打标记，保持"不弹框、不等待"的脚本语义（否则门禁会卡在等回车）。
void MarkConsoleForChildIfOwned() {
    DWORD pids[4] = {};
    const DWORD n = GetConsoleProcessList(pids, 4);
    if (n != 1)
        return;
    if (StartedByAutomationHost())
        return;
    SetEnvironmentVariableW(L"ZJ_CHECK_LAUNCHER", L"1");
}
#else
const wchar_t* kTarget = L"x64\\SysRecover.exe";
const wchar_t* kLabel = L"SysRecover";
#endif

int Launch(bool wait) {
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return 1;
    const std::wstring dir = DirOf(self);
    const std::wstring target = dir + L"\\" + kTarget;
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
#ifdef ZJ_LAUNCH_GUI
        std::wstring msg = L"64-bit program not found:\n" + target +
                           L"\n\nKeep the x64 subfolder in the release package.";
        MessageBoxW(nullptr, msg.c_str(), kLabel, MB_OK | MB_ICONERROR);
#else
        std::fwprintf(stderr, L"64-bit program not found: %ls\n", target.c_str());
#endif
        return 1;
    }

    std::wstring child = L"\"" + target + L"\"" + StripExeToken(GetCommandLineW());
#ifdef ZJ_LAUNCH_CHECK
    MarkConsoleForChildIfOwned();
#endif
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(target.c_str(), child.empty() ? nullptr : &child[0],
                        nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si,
                        &pi)) {
        const DWORD e = GetLastError();
#ifdef ZJ_LAUNCH_GUI
        wchar_t b[96] = {};
        swprintf(b, 96, L"Cannot start (error %lu):\n%ls", e, target.c_str());
        MessageBoxW(nullptr, b, kLabel, MB_OK | MB_ICONERROR);
#else
        std::fwprintf(stderr, L"Cannot start (error %lu): %ls\n", e,
                      target.c_str());
#endif
        return 1;
    }
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

}  // namespace

#ifdef ZJ_LAUNCH_GUI
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return Launch(false); }
#else
int main() { return Launch(true); }
#endif
