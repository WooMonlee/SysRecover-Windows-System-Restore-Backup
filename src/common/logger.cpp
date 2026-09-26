// 日志实现：追加写 + 原子性不作保证（单进程写为主）。
#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace sysrecover {
namespace {

std::wstring g_logFile;
CRITICAL_SECTION g_cs;
bool g_csInit = false;

// 日志轮转（问题清单 A3/B5）：logs\ 与 logs\crash\ 原先只增不减。
//   · 按天日志 SysRecover-*.log：保留最近 14 天；
//   · crash\ 只留最新 30 个 crash-*（文件名含时间戳 → 字典序即时间序）。
void PruneLogs(const std::wstring& logDir) {
    const ULONGLONG kDay100ns = 864000000000ULL;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((logDir + L"\\SysRecover-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                ULARGE_INTEGER w;
                w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                if (now.QuadPart > w.QuadPart &&
                    now.QuadPart - w.QuadPart > 14ULL * kDay100ns)
                    DeleteFileW((logDir + L"\\" + fd.cFileName).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::wstring cd = logDir + L"\\crash";
    std::vector<std::wstring> names;
    HANDLE h2 = FindFirstFileW((cd + L"\\crash-*").c_str(), &fd);
    if (h2 != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                names.push_back(fd.cFileName);
        } while (FindNextFileW(h2, &fd));
        FindClose(h2);
    }
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + 30 < names.size(); ++i)
        DeleteFileW((cd + L"\\" + names[i]).c_str());
}

void EnsureCs() {
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
}

std::string Timestamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

void Write(const char* level, const std::string& msg, FILE* con) {
    std::string line = Timestamp() + " [" + level + "] " + msg + "\n";
    fputs(line.c_str(), con);
    fflush(con);
    if (g_logFile.empty())
        return;
    EnsureCs();
    EnterCriticalSection(&g_cs);
    HANDLE h =
        CreateFileW(g_logFile.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&g_cs);
}

}  // namespace

void LogInit(const std::wstring& logDir) {
    CreateDirectoryW(logDir.c_str(), nullptr);
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[64] = {};
    swprintf_s(name, L"SysRecover-%04d%02d%02d.log", st.wYear, st.wMonth,
               st.wDay);
    g_logFile = logDir + L"\\" + name;
    PruneLogs(logDir);
}

void LogInfo(const std::string& msg) { Write("INFO", msg, stdout); }
void LogWarn(const std::string& msg) { Write("WARN", msg, stdout); }
void LogError(const std::string& msg) { Write("ERROR", msg, stderr); }

}  // namespace sysrecover
