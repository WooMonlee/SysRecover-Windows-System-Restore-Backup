// 日志实现：追加写 + 原子性不作保证（单进程写为主）。
#include "logger.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace sysrecover {
namespace {

std::wstring g_logFile;
CRITICAL_SECTION g_cs;
bool g_csInit = false;

// 总量上限（用户 2026-10-04 规格 5；2026-10-05 二次修订：支持包开始带屏幕
// 截图/更多环境信息 → 上限提高）：
//   · logs 目录（含 crash/、collected/、screens/，深度≤3）超 **64MB** 才启动清理；
//   · 第一档：先删**一周以上**的旧日志；
//   · 仍超 64MB → 第二档：最旧优先删到 **16MB 以内**。
// 文本日志一次运行仅数百 KB；大头只有屏幕 BMP（单张几 MB）与 crash dump。
// 当前活动日志永不删。
struct LogFileItem {
    ULARGE_INTEGER mtime;
    unsigned long long size;
    std::wstring path;
};

void CollectLogFiles(const std::wstring& dir, int depth,
                     std::vector<LogFileItem>& out) {
    if (depth < 0)
        return;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectLogFiles(full, depth - 1, out);
        } else {
            LogFileItem it = {};
            it.mtime.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            it.mtime.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            LARGE_INTEGER li = {};
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            it.size = (unsigned long long)li.QuadPart;
            it.path = full;
            out.push_back(it);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void PruneBySize(const std::wstring& logDir) {
    const unsigned long long kCap = 64ull << 20;   // 触发线：64MB（含截图）
    const unsigned long long kDeep = 16ull << 20;  // 第二档目标：16MB
    const ULONGLONG kWeek100ns = 7ULL * 864000000000ULL;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;

    std::vector<LogFileItem> files;
    CollectLogFiles(logDir, 3, files);
    unsigned long long total = 0;
    for (const auto& f : files)
        total += f.size;
    if (total <= kCap)
        return;
    std::sort(files.begin(), files.end(),
              [](const LogFileItem& a, const LogFileItem& b) {
                  return a.mtime.QuadPart < b.mtime.QuadPart;
              });
    auto isActive = [](const LogFileItem& f) {
        return !g_logFile.empty() &&
               _wcsicmp(f.path.c_str(), g_logFile.c_str()) == 0;
    };
    auto del = [&total](LogFileItem& f) {
        SetFileAttributesW(f.path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(f.path.c_str()))
            total -= f.size;
    };
    // 第一档：删一周以上的（最旧在前）
    for (auto& f : files) {
        if (total <= kCap)
            return;
        if (isActive(f))
            continue;
        if (now.QuadPart > f.mtime.QuadPart &&
            now.QuadPart - f.mtime.QuadPart > kWeek100ns)
            del(f);
    }
    // 第二档：仍超 32MB → 最旧优先删到 8MB 以内
    for (auto& f : files) {
        if (total <= kDeep)
            break;
        if (isActive(f))
            continue;
        del(f);
    }
}

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

void AppendLogFile(const std::string& line) {
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

void Write(const char* level, const std::string& msg, FILE* con) {
    std::string line = Timestamp() + " [" + level + "] " + msg + "\n";
    fputs(line.c_str(), con);
    fflush(con);
    AppendLogFile(line);
}

void WriteFileOnly(const char* level, const std::string& msg) {
    AppendLogFile(Timestamp() + " [" + level + "] " + msg + "\n");
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
    PruneBySize(logDir);
}

void LogInfo(const std::string& msg) { Write("INFO", msg, stdout); }
void LogWarn(const std::string& msg) { Write("WARN", msg, stdout); }
void LogError(const std::string& msg) { Write("ERROR", msg, stderr); }
void LogFileInfo(const std::string& msg) { WriteFileOnly("INFO", msg); }

}  // namespace sysrecover
