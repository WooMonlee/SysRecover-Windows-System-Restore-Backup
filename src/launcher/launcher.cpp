// SysRecover 启动器（x86，通吃 32/64 位 Windows）。
//
// 为什么需要：Windows 侧主程序的位数**跟随系统位数**（32 位系统跑 x86、64 位系统跑 x64），
// 主要是为了**备份压缩速度**（64 位对 LZMS/LZX 明显更快；还原由 Linux 救援层做，与宿主位数无关）。
// Linux 救援层固定 x86_64，与本启动器无关。
//
// 布局（发布包根目录）：
//   SysRecover.exe / SysRecoverUI.exe   <- 本启动器（x86，无第三方依赖）
//   x86\{SysRecover.exe, SysRecoverUI.exe, libwim-15.dll, UCRT...}
//   x64\{同上}
//   bootfiles\ skin\ resources\ ...     <- 与位数无关，放根目录
//
// 本启动器不含任何业务逻辑，只做：判断系统位数 → CreateProcess 对应子目录的程序 → 透传参数/退出码。
// 编辑本文件时注意保持 MinGW 可编译（不要引入 C++ 标准库以外的重依赖）。
#include <windows.h>
#include <shellapi.h>

#include <string>

namespace {

std::wstring DirOf(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : path.substr(0, k);
}

std::wstring BaseOf(const std::wstring& path) {
    size_t k = path.find_last_of(L"\\/");
    return k == std::wstring::npos ? path : path.substr(k + 1);
}

// 系统位数（不是本进程的位数）：GetNativeSystemInfo 即使在 WOW64 下也返回原生架构。
bool IsX64System() {
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64;
}

}  // namespace

int main() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0)
        return 1;
    std::wstring exePath = self;
    std::wstring name = BaseOf(exePath);
    std::wstring root = DirOf(exePath);

    const wchar_t* arch = IsX64System() ? L"x64" : L"x86";
    std::wstring target = root + L"\\" + arch + L"\\" + name;

    // 透传命令行（去掉本程序名那个 token，其余原样给子进程，保留引号与空格）。
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
    std::wstring args = cmdline.substr(i);

    std::wstring child = L"\"" + target + L"\"" + args;
    std::wstring cwd = root;  // 子进程工作目录 = 发布包根（相对资源不受影响，见 process/ExeDir）

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(target.c_str(), child.empty() ? nullptr : &child[0],
                        nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si,
                        &pi)) {
        std::wstring msg = L"找不到或无法启动 " + target +
                           L"\n请确认发布包完整（x86\\ 与 x64\\ 两个子目录都在）。";
        MessageBoxW(nullptr, msg.c_str(), L"知鉴一键还原",
                    MB_OK | MB_ICONERROR);
        return 3;
    }
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
