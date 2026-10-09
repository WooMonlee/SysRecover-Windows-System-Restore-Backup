// A 引擎残留检测与分流实现（0.7，用户 2026-10-09 规格；决策表 docs/23 §5）：
//   ~new： 过期(≥24h)→删；近期+有标记→Reuse（还原时比对 content 免重解压）；
//          近期+无标记→Redo（还原时自动删重解，启动期只记日志）
//   ~old： 根目录缺 Windows →AskUndo（任何账龄：~old 可能是仅存的旧系统，绝不自动删）；
//          同盘 ~new 仍在 →AskUndo（交换中途崩溃）；
//          其余（根完好且 ~new 已消费）→Delete（补做首启清理/过期垃圾）
// 注意：~old 的结构判据**优先于时间** —— "隔久直接删"只适用于"确定是垃圾"的场景。
#include "swapleft.h"

#include <windows.h>

#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

#include "../common/logger.h"

namespace sysrecover {

namespace {

// ops.cpp 的 DeleteTreeRec 在匿名命名空间（内链，外部链接不到）→ 本文件自带
void DeleteRec(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteRec(p);
            else
                DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// W2U 没有头声明（各 TU 匿名命名空间自带副本，i18n.h 只有 Tr）→ 本文件自带
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr,
                                   nullptr);
    return s;
}

std::mutex g_busyMx;
std::condition_variable g_busyCv;
std::set<wchar_t> g_busy;  // 正在删除/撤销中的盘

std::wstring RootPath(wchar_t drive) {
    return std::wstring(1, drive) + L":\\";
}

std::string DriveStr(wchar_t drive) {
    std::string s(1, (char)drive);
    s += ":";
    return s;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

long AgeHoursFrom(const FILETIME& ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = ft.dwLowDateTime;
    a.HighPart = ft.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return 0;
    return (long)((b.QuadPart - a.QuadPart) / 10000000ULL / 3600ULL);
}

// ~new 的"最近活动"取 目录 mtime 与 .zj-done mtime 的较新者
long AgeHoursMax(const FILETIME& a, const FILETIME& b) {
    ULARGE_INTEGER x, y;
    x.LowPart = a.dwLowDateTime;
    x.HighPart = a.dwHighDateTime;
    y.LowPart = b.dwLowDateTime;
    y.HighPart = b.dwHighDateTime;
    return x.QuadPart >= y.QuadPart ? AgeHoursFrom(a) : AgeHoursFrom(b);
}

// 读 ~new\.zj-done：content= 字节 + 文件时间（格式由 ops.cpp 写入）
bool ReadMarker(const std::wstring& path, unsigned long long& content,
                FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return false;
    ft = fad.ftLastWriteTime;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[256] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    content = 0;
    if (rd >= 8 && std::strncmp(buf, "content=", 8) == 0)
        content = std::strtoull(buf + 8, nullptr, 10);
    return true;
}

// 根下 ZJRESTORE-status.txt（救援黑匣子回执）：result=OK？
bool ReadRescueOk(const std::wstring& root) {
    HANDLE h = CreateFileW((root + L"ZJRESTORE-status.txt").c_str(),
                           GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[1024] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    return std::strstr(buf, "result=OK") != nullptr;
}

// 探测一块盘上的 ~new(wantNew=true) / ~old；无残留返回 false
bool ProbeDrive(wchar_t drive, bool wantNew, SwapLeftInfo& out) {
    std::wstring root = RootPath(drive);
    std::wstring dir = root + (wantNew ? L"~new" : L"~old");
    if (!IsDir(dir)) return false;
    out = SwapLeftInfo{};
    out.drive = drive;
    out.isNew = wantNew;
    if (wantNew) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        FILETIME dirFt{};
        if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &fad))
            dirFt = fad.ftLastWriteTime;
        FILETIME mkFt{};
        out.hasMarker = ReadMarker(dir + L"\\.zj-done", out.markerContent, mkFt);
        out.ageHours =
            out.hasMarker ? AgeHoursMax(dirFt, mkFt) : AgeHoursFrom(dirFt);
    } else {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &fad))
            out.ageHours = AgeHoursFrom(fad.ftLastWriteTime);
        out.rootIntact = IsDir(root + L"Windows");
        out.siblingNew = IsDir(root + L"~new");
        out.rescueOk = ReadRescueOk(root);
    }
    return true;
}

void BusyMark(wchar_t drive) {
    std::lock_guard<std::mutex> lk(g_busyMx);
    g_busy.insert(drive);
}
void BusyUnmark(wchar_t drive) {
    {
        std::lock_guard<std::mutex> lk(g_busyMx);
        g_busy.erase(drive);
    }
    g_busyCv.notify_all();
}

}  // namespace

SwapLeftVerdict DecideSwapLeftover(const SwapLeftInfo& i, long staleHours) {
    SwapLeftVerdict v;
    if (i.isNew) {
        if (i.ageHours >= 0 && i.ageHours >= staleHours) {
            v.action = SwapLeftAction::Delete;
            v.reason = "~new expired (age " + std::to_string(i.ageHours) +
                       "h >= " + std::to_string(staleHours) + "h)";
        } else if (i.hasMarker) {
            v.action = SwapLeftAction::Reuse;
            v.reason = "~new complete (marker present, age " +
                       std::to_string(i.ageHours) + "h)";
        } else {
            v.action = SwapLeftAction::Redo;
            v.reason =
                "~new incomplete (no .zj-done) -> auto-redo on next swap restore";
        }
        return v;
    }
    // ~old：结构判据优先（时间不作自动删除依据，见文件头注释）
    if (!i.rootIntact) {
        v.action = SwapLeftAction::AskUndo;
        v.reason =
            "~old: system dir missing at root (swap interrupted; ~old may hold "
            "the only copy of the old system)";
    } else if (i.siblingNew) {
        v.action = SwapLeftAction::AskUndo;
        v.reason = "~old: sibling ~new present (swap crashed mid-way)";
    } else {
        v.action = SwapLeftAction::Delete;
        v.reason = i.rescueOk
                       ? "~old: rescue receipt OK -> post-swap leftover"
                       : "~old: root intact and ~new consumed -> swap finished, "
                         "leftover old system";
    }
    return v;
}

std::vector<SwapLeftInfo> ScanSwapLeftovers() {
    std::vector<SwapLeftInfo> out;
    wchar_t buf[512] = {0};
    if (!GetLogicalDriveStringsW(511, buf)) return out;
    for (wchar_t* p = buf; *p; p += std::wcslen(p) + 1) {
        if (GetDriveTypeW(p) != DRIVE_FIXED) continue;
        wchar_t d = p[0];
        SwapLeftInfo i;
        if (ProbeDrive(d, true, i)) out.push_back(i);
        if (ProbeDrive(d, false, i)) out.push_back(i);
    }
    return out;
}

bool ProbeSwapNew(wchar_t drive, SwapLeftInfo& out) {
    return ProbeDrive(drive, true, out);
}

void DeleteSwapLeftover(const SwapLeftInfo& i) {
    if (!i.drive) return;
    std::wstring dir = RootPath(i.drive) + (i.isNew ? L"~new" : L"~old");
    BusyMark(i.drive);
    LogInfo("swap leftover delete: " + DriveStr(i.drive) +
            (i.isNew ? " ~new" : " ~old") + " (age " +
            std::to_string(i.ageHours) + "h)");
    if (IsDir(dir)) DeleteRec(dir);
    BusyUnmark(i.drive);
}

void DeleteSwapLeftoverAsync(const SwapLeftInfo& i) {
    std::thread([i] { DeleteSwapLeftover(i); }).detach();
}

void WaitSwapCleanup(wchar_t drive) {
    std::unique_lock<std::mutex> lk(g_busyMx);
    g_busyCv.wait(lk, [drive] { return g_busy.count(drive) == 0; });
}

bool UndoSwapLeftover(wchar_t drive, std::string& detail) {
    std::wstring root = RootPath(drive);
    std::wstring old = root + L"~old";
    if (!IsDir(old)) {
        detail = "~old not found";
        return false;
    }
    BusyMark(drive);
    int moved = 0, conflict = 0, failed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((old + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring src = old + L"\\" + fd.cFileName;
            std::wstring dst = root + fd.cFileName;
            if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ++conflict;  // 根下已有同名 → 留在 ~old，不覆盖
                LogWarn("undo swap: conflict, keep in ~old: " + ToUtf8(dst));
                continue;
            }
            if (MoveFileW(src.c_str(), dst.c_str())) {
                ++moved;
            } else {
                ++failed;
                LogWarn("undo swap: move failed (err=" +
                        std::to_string(GetLastError()) + "): " + ToUtf8(src));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(old.c_str());  // 空才成功；有冲突残留则保留
    detail = "moved=" + std::to_string(moved) +
             " conflict=" + std::to_string(conflict) +
             " failed=" + std::to_string(failed);
    BusyUnmark(drive);
    LogInfo("undo swap leftover on " + DriveStr(drive) + ": " + detail);
    return conflict == 0 && failed == 0;
}

}  // namespace sysrecover
