// zj-ea-apply.exe — 首启 EA 补写器（PIT-122/123，2026-10-06）
//
// 由本地组策略「启动脚本」在**登录前、SYSTEM 身份**拉起（QEMU 全链实测）。
// 无第三方依赖（不链接 wimlib）：读 eapack.dat → NtSetEaFile 把 EA 写回 →
// 成功后自清理（策略/脚本/包/自身），失败保留以便下次引导重试（最多 3 次）。
// 日志：<SystemDrive>\ZJRESTORE\ea\ea-apply.log（UTF-8）。
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../common/ea.h"
#include "../common/version.h"

using namespace sysrecover;

namespace {

std::wstring g_logPath;

void logLine(const std::string& s) {
    if (g_logPath.empty()) return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // 裸字节追加（不走 CRT 的 ccs 模式：实测 `_wfopen("a, ccs=UTF-8")` 会把
    // 文本吃掉、只留 CRLF 的 UTF-8 形态 —— U+0A0D 乱码）。
    std::string out = std::string(ts) + s + "\r\n";
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(h);
    }
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? (char)c : '?';
    return s;
}

int ReadAttempts(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    return atoi(buf);
}

void WriteAttempts(const std::wstring& path, int n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", n);
    DWORD wr = 0;
    WriteFile(h, buf, (DWORD)len, &wr, nullptr);
    CloseHandle(h);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // ⚠️ WOW64 文件系统重定向（PIT-082 家族坑，2026-10-06 实测踩到）：
    // 本 exe 是 **32 位**（x86/x64 目标都要能跑），在 64 位 Windows 上访问
    // `C:\Windows\System32\...` 会被**重定向到 SysWOW64** —— 组策略文件在真实的
    // System32\GroupPolicy（SysWOW64 里没有）→ 清理全部静默跳过；EA 目标若在
    // System32 下也会写错文件。进程级关掉重定向（本程序不需要它）。
    PVOID oldRedir = nullptr;
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        using Wow64DisableFn = BOOL(WINAPI*)(PVOID*);
        // 两步 cast（经 void*）规避 -Wcast-function-type
        auto fn = reinterpret_cast<Wow64DisableFn>(reinterpret_cast<void*>(
            GetProcAddress(k32, "Wow64DisableWow64FsRedirection")));
        if (fn) fn(&oldRedir);
    }
    std::wstring pack;
    std::wstring rootOverride;
    bool noCleanup = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pack") && i + 1 < argc) {
            pack = argv[++i];
        } else if (!wcscmp(argv[i], L"--root") && i + 1 < argc) {
            rootOverride = argv[++i];  // 实验室/自测：EA 路径按此根解析
        } else if (!wcscmp(argv[i], L"--no-cleanup")) {
            noCleanup = true;  // 实验室/自测：跳过钩子清理与自删
        }
    }
    std::wstring root;
    if (!rootOverride.empty()) {
        root = rootOverride;
        while (!root.empty() &&
               (root.back() == L'/' || root.back() == L'\\'))
            root.pop_back();
    } else {
        wchar_t winDir[MAX_PATH] = {};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        root.assign(winDir, wcslen(winDir) >= 2 ? 2 : wcslen(winDir));  // "C:"
    }
    const std::wstring eaDir = root + L"\\ZJRESTORE\\ea";
    CreateDirectoryW(eaDir.c_str(), nullptr);
    if (pack.empty()) pack = eaDir + L"\\eapack.dat";

    g_logPath = eaDir + L"\\ea-apply.log";
    logLine("ea-apply start build=" SYSRECOVER_VERSION);
    logLine("pack=" + Ascii(pack));
    // 诊断：本进程在"登录前"还是"登录后"跑（Shell_TrayWnd = 任务栏）
    logLine(std::string("shell_started=") +
            (FindWindowW(L"Shell_TrayWnd", nullptr) ? "yes" : "no (pre-logon)"));

    ea::ApplyStats st;
    int rc = ea::ApplyPack(pack, root, st, logLine);
    char sum[256];
    snprintf(sum, sizeof(sum),
             "result files=%llu ok=%llu missing=%llu failed=%llu lastStatus=0x%08lX",
             st.files, st.ok, st.missing, st.failed, st.lastStatus);
    logLine(sum);
    if (!st.firstErrors.empty())
        logLine(std::string("error samples:\r\n") + st.firstErrors);

    // 结果一行（支持包/收集器直接读）
    {
        SYSTEMTIME stw = {};
        GetLocalTime(&stw);
        char line[256];
        int len = snprintf(line, sizeof(line),
                           "time=%04d-%02d-%02d %02d:%02d:%02d ok=%llu missing=%llu failed=%llu rc=%d\r\n",
                           stw.wYear, stw.wMonth, stw.wDay, stw.wHour, stw.wMinute,
                           stw.wSecond, st.ok, st.missing, st.failed, rc);
        std::vector<unsigned char> one(line, line + len);
        ea::WriteBytes(eaDir + L"\\ea-result.txt", one);
    }

    if (rc != 0) {
        const std::wstring attemptsPath = eaDir + L"\\attempts.txt";
        int n = ReadAttempts(attemptsPath) + 1;
        WriteAttempts(attemptsPath, n);
        logLine("failed; attempt " + std::to_string(n) + "/3");
        if (n >= 3) {
            std::wstring failed = pack + L".failed";
            if (MoveFileExW(pack.c_str(), failed.c_str(),
                            MOVEFILE_REPLACE_EXISTING))
                logLine("give up after 3 attempts; pack renamed .failed");
            else
                logLine("give up after 3 attempts; rename FAILED");
        } else {
            logLine("keeping files for retry on next boot");
        }
    } else if (noCleanup) {
        logLine("cleanup skipped (--no-cleanup)");
    } else {
        logLine("cleanup start");
        ea::CleanupHookFiles(root, logLine);
        logLine("cleanup done");
    }
    logLine("ea-apply end rc=" + std::to_string(rc));
    return rc;
}
