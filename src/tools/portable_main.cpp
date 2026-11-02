// 单文件（便携）封装器：把 dist 全部文件按"顺序清单"追加在本 exe 末尾，
// 运行时解到固定目录再启动真正的程序（PIT-201）。
//
// 为什么不用 7-Zip SFX：stock 的 7z.sfx 只是"7z.exe 前端"，会把参数当 7z 命令
// （实测 `SysRecover-Portable-CLI.exe version` → "Unknown command: version"）；
// 安装器版 7zSD.sfx 不在标准安装包里。这里自己封，格式极简、零依赖。
//
// 容器格式（全部小端）：
//   [stub exe]
//   repeat: 'ZJF1' | u32 nameLen | u64 dataLen | u64 mtime | name(UTF-8) | data
//   'ZJEND'
//   'ZJFT' | u64 payloadStartOffset      ← 最后 12 字节，stub 用它定位
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// PIT-202：单文件**合并成一个**（用户 2026-11-01 规格）——
// 有参数 → 附着到调用者控制台跑 SysRecover.exe（CLI，输出可见、退出码透传）；
// 无参数 → 开 SysRecoverUI.exe（GUI）。
const wchar_t* kCliExe = L"SysRecover.exe";
const wchar_t* kGuiExe = L"SysRecoverUI.exe";
const wchar_t* kTitle = L"NineTurn SysRecover";

std::wstring DirOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
}

std::wstring AppDataDir() {
    wchar_t buf[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) > 0 &&
        buf[0])
        return buf;
    if (GetWindowsDirectoryW(buf, MAX_PATH))
        return std::wstring(buf) + L"\\Temp";
    return L".";
}

bool ReadExact(HANDLE h, void* buf, DWORD n) {
    DWORD got = 0;
    return ReadFile(h, buf, n, &got, nullptr) && got == n;
}

unsigned long long FileSizeOf(HANDLE h) {
    LARGE_INTEGER li = {};
    return GetFileSizeEx(h, &li) ? (unsigned long long)li.QuadPart : 0;
}

bool MakeDirs(const std::wstring& path) {
    std::wstring p = path;
    for (size_t i = 3; i < p.size(); ++i) {
        if (p[i] == L'\\' || p[i] == L'/') {
            const std::wstring sub = p.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);
        }
    }
    CreateDirectoryW(p.c_str(), nullptr);
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return 1;

    // PIT-210（用户 2026-11-02：单文件打开前会闪一下黑框）：
    // **先判断有没有参数**，GUI 模式立刻把控制台藏掉/释放 —— 以前要等 49MB
    // 解压完才 FreeConsole，控制台窗口就一直亮着（看起来像"闪一下"甚至更久）。
    std::wstring cmdArgs = GetCommandLineW();
    {
        size_t i = 0;
        while (i < cmdArgs.size() && cmdArgs[i] == L' ') ++i;
        if (i < cmdArgs.size() && cmdArgs[i] == L'"') {
            const size_t j = cmdArgs.find(L'"', i + 1);
            i = (j == std::wstring::npos) ? cmdArgs.size() : j + 1;
        } else {
            const size_t j = cmdArgs.find(L' ', i);
            i = (j == std::wstring::npos) ? cmdArgs.size() : j;
        }
        cmdArgs = cmdArgs.substr(i);
        while (!cmdArgs.empty() && cmdArgs[0] == L' ') cmdArgs.erase(0, 1);
    }
    const bool wantCli = !cmdArgs.empty();
    if (!wantCli) {
        if (HWND con = GetConsoleWindow()) ShowWindow(con, SW_HIDE);
        FreeConsole();
    }

    HANDLE h = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    const unsigned long long total = FileSizeOf(h);
    unsigned char foot[12] = {};
    if (total < 12) {
        CloseHandle(h);
        return 1;
    }
    LARGE_INTEGER pos = {};
    pos.QuadPart = (LONGLONG)(total - 12);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    if (!ReadExact(h, foot, 12) || memcmp(foot, "ZJFT", 4) != 0) {
        CloseHandle(h);
        MessageBoxW(nullptr, L"This file is not a valid SysRecover portable.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    unsigned long long payload = 0;
    memcpy(&payload, foot + 4, 8);
    if (payload >= total) {
        CloseHandle(h);
        return 1;
    }

    // 解压目标：%LOCALAPPDATA%\SysRecover\app（用户 2026-11-01 确认：单文件默认
    // 解到"临时性质"的目录就行，**不要**往单文件旁边解）；需要落盘的产物
    //（如"导出驱动"）由用户自己选文件夹。
    const std::wstring dest = AppDataDir() + L"\\SysRecover\\app";
    if (!MakeDirs(dest)) {
        CloseHandle(h);
        MessageBoxW(nullptr, L"Cannot create the working folder.",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }

    // 版本戳：同一次发布的载荷解过一次就跳过（原来每次运行都要写 49MB）。
    // 戳 = 本 exe 的大小 + 最后写入时间（载荷追加在 exe 末尾，换版本必然变）。
    wchar_t stampPathBuf[MAX_PATH * 2] = {};
    _snwprintf(stampPathBuf, MAX_PATH * 2, L"%ls\\.zj-stamp", dest.c_str());
    const std::wstring stampPath = stampPathBuf;
    char stampNow[96] = {};
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(self, GetFileExInfoStandard, &fad)) {
            const unsigned long long sz =
                ((unsigned long long)fad.nFileSizeHigh << 32) |
                (unsigned long long)fad.nFileSizeLow;
            const unsigned long long tm =
                ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
            snprintf(stampNow, sizeof(stampNow), "size=%llu time=%llu", sz, tm);
        }
    }
    bool skipExtract = false;
    if (stampNow[0]) {
        std::string old;
        HANDLE hs = CreateFileW(stampPath.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (hs != INVALID_HANDLE_VALUE) {
            char buf[128] = {};
            DWORD got = 0;
            if (ReadFile(hs, buf, sizeof(buf) - 1, &got, nullptr) && got > 0)
                old.assign(buf, got);
            CloseHandle(hs);
        }
        if (old == stampNow) {
            const std::wstring probe =
                dest + L"\\" + L"SysRecoverUI.exe";
            if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CloseHandle(h);
                // 已解压过（同版本）→ 直接启动，跳过 49MB 重写
                skipExtract = true;
            }
        }
    }

    if (!skipExtract) {
    // 顺序解析条目并落盘（覆盖写；同名同大小则跳过以加快二次启动）
    pos.QuadPart = (LONGLONG)payload;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    char magic[4] = {};
    int written = 0;
    while (ReadExact(h, magic, 4)) {
        if (memcmp(magic, "ZJEND", 4) == 0 || memcmp(magic, "ZJFT", 4) == 0)
            break;
        if (memcmp(magic, "ZJF1", 4) != 0) break;
        unsigned int nameLen = 0;
        unsigned long long dataLen = 0, mtime = 0;
        if (!ReadExact(h, &nameLen, 4) || !ReadExact(h, &dataLen, 8) ||
            !ReadExact(h, &mtime, 8))
            break;
        if (nameLen == 0 || nameLen > 4096) break;
        std::vector<char> nb(nameLen);
        if (!ReadExact(h, nb.data(), nameLen)) break;
        int wn = MultiByteToWideChar(CP_UTF8, 0, nb.data(), (int)nameLen,
                                     nullptr, 0);
        std::wstring rel(wn > 0 ? wn : 0, L'\0');
        if (wn > 0)
            MultiByteToWideChar(CP_UTF8, 0, nb.data(), (int)nameLen, &rel[0],
                                wn);
        for (auto& c : rel)
            if (c == L'/') c = L'\\';
        const std::wstring full = dest + L"\\" + rel;
        MakeDirs(DirOf(full));
        HANDLE out = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            // 跳过该文件的数据
            LARGE_INTEGER skip = {};
            skip.QuadPart = (LONGLONG)dataLen;
            SetFilePointerEx(h, skip, nullptr, FILE_CURRENT);
            continue;
        }
        char buf[64 * 1024];
        unsigned long long left = dataLen;
        while (left > 0) {
            const DWORD ask = (DWORD)(left > sizeof(buf) ? sizeof(buf) : left);
            DWORD got = 0;
            if (!ReadFile(h, buf, ask, &got, nullptr) || got != ask) break;
            DWORD wr = 0;
            WriteFile(out, buf, got, &wr, nullptr);
            left -= got;
        }
        CloseHandle(out);
        ++written;
    }
    CloseHandle(h);
    if (stampNow[0]) {
        HANDLE hs = CreateFileW(stampPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (hs != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            WriteFile(hs, stampNow, (DWORD)strlen(stampNow), &wr, nullptr);
            CloseHandle(hs);
        }
    }
    }  // !skipExtract

    // 告诉子进程"原始单文件在哪" —— imgsearch 用它先扫用户真正放文件的目录
    //（否则解压到 %LOCALAPPDATA% 后，"exe 上一级"规则扫的是程序目录，找不到镜像）。
    SetEnvironmentVariableW(L"ZJ_ORIGIN_DIR", DirOf(self).c_str());

    // 参数已在入口处解析好（PIT-210：GUI 模式要尽早藏掉控制台）
    const std::wstring& args = cmdArgs;

    const std::wstring target =
        dest + L"\\" + (wantCli ? kCliExe : kGuiExe);
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"Extraction incomplete (main program missing).",
                    kTitle, MB_OK | MB_ICONERROR);
        return 1;
    }
    // 把原始命令行（去掉本 exe 那一段）透传
    if (!wantCli) {
        // 双击打开：控制台在入口已释放/隐藏，直接开 GUI
        HINSTANCE r = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr,
                                    dest.c_str(), SW_SHOWNORMAL);
        return ((INT_PTR)r > 32) ? 0 : 1;
    }
    // CLI 模式：附着到调用者控制台（cmd/PowerShell），把 stdout/stderr 接到
    // CONOUT$，子进程继承 → 输出能看见、退出码能透传。
    // 附着失败（从资源管理器带参数启动等）→ 退回开 GUI，不静默。
    // 控制台子系统进程本身就有控制台；没有才附着。**附着失败也继续跑** ——
    // 绝不能退回开 GUI（那会让 `xxx.exe version` 变成弹窗口，脚本拿不到输出）。
    if (GetConsoleWindow() == nullptr)
        AttachConsole(ATTACH_PARENT_PROCESS);
    // 先看调用者是不是已经给了 stdout（cmd 的 `> file` 重定向就是这种）——
    // 有就沿用，别用 CONOUT$ 覆盖掉，否则脚本取不到输出（实测踩过）。
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == nullptr || hOut == INVALID_HANDLE_VALUE) {
        hOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (hOut != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, hOut);
            SetStdHandle(STD_ERROR_HANDLE, hOut);
        }
    }
    // cmd 的 `> file` 句柄未必带可继承标志 → 直接传给子进程会失效（实测输出丢失）。
    // 复制一份可继承的再传。
    auto inherit = [](HANDLE h) -> HANDLE {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return h;
        HANDLE d = nullptr;
        return DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &d,
                               0, TRUE, DUPLICATE_SAME_ACCESS)
                   ? d
                   : h;
    };
    hOut = inherit(hOut);
    HANDLE hIn = inherit(GetStdHandle(STD_INPUT_HANDLE));
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hIn;
    si.hStdOutput = hOut == INVALID_HANDLE_VALUE ? nullptr : hOut;
    si.hStdError = hOut == INVALID_HANDLE_VALUE ? nullptr : hOut;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + target + L"\" " + args;
    if (!CreateProcessW(target.c_str(), &cmd[0], nullptr, nullptr, TRUE, 0,
                        nullptr, dest.c_str(), &si, &pi)) {
        const DWORD e = GetLastError();
        std::fwprintf(stderr, L"Cannot start (error %lu)\n", e);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)rc;
}
