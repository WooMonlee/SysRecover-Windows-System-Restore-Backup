// 日志实现：追加写 + 原子性不作保证（单进程写为主）。
#include "logger.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace sysrecover {
namespace {

std::wstring g_logFile;
CRITICAL_SECTION g_cs;
bool g_csInit = false;

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
}

void LogInfo(const std::string& msg) { Write("INFO", msg, stdout); }
void LogWarn(const std::string& msg) { Write("WARN", msg, stdout); }
void LogError(const std::string& msg) { Write("ERROR", msg, stderr); }

}  // namespace sysrecover
