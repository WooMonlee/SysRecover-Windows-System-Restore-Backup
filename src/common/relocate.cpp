#include "relocate.h"

#include "i18n.h"
#include "selfarch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

const wchar_t kCopyDirName[] = L"ZJRESTORE";  // 与恢复目录同名（用户 2026-10-03 定）
const unsigned long long kSpaceMargin = 32ull << 20;

std::string ToUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr,
                            nullptr);
    return s;
}

// 写探针：能建并删掉临时文件 = 目录可写（只读介质/U盘写保护会失败）。
bool DirWritable(const std::wstring& dir) {
    if (dir.empty())
        return false;
    std::wstring probe = dir + L"\\.zj-write-probe.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}

// 该分区装有 Windows（还原目标最可能是它；PE 下离线系统分区也命中）。
bool HasWindows(const std::wstring& driveRoot) {
    return GetFileAttributesW(
               (driveRoot + L"Windows\\System32\\winload.exe").c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

unsigned long long TreeSizeRec(const std::wstring& dir, bool root) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (root && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            _wcsicmp(fd.cFileName, L"logs") == 0)
            continue;  // logs 不复制（目的地已有日志只增不减）
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += TreeSizeRec(full, false);
        } else {
            LARGE_INTEGER li{};
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            total += (unsigned long long)li.QuadPart;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

bool CopyTree(const std::wstring& src, const std::wstring& dst, bool root,
              std::string& err) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return true;  // 空目录
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (root && isDir && _wcsicmp(fd.cFileName, L"logs") == 0)
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        if (isDir) {
            if (!CopyTree(s, d, false, err))
                return false;
        } else {
            // 覆盖隐藏/只读/系统文件时先归零属性（PIT-067/PIT-068 的教训）
            SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
                char m[64];
                std::snprintf(m, sizeof(m), " (err=%lu)", GetLastError());
                err = ToUtf8(s) + m;
                return false;
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

// 原命令行去掉首个 token（保留引号内空格）；用于给新副本转发参数。
std::wstring CmdLineTail() {
    std::wstring c = GetCommandLineW();
    size_t i = 0;
    if (!c.empty() && c[0] == L'"') {
        size_t e = c.find(L'"', 1);
        i = (e == std::wstring::npos) ? c.size() : e + 1;
    } else {
        size_t e = c.find_first_of(L" \t");
        i = (e == std::wstring::npos) ? c.size() : e;
    }
    return c.substr(i);
}

}  // namespace

namespace sysrecover {

wchar_t SelectRelocateDrive(const std::vector<wchar_t>& candidates,
                            wchar_t systemLetter) {
    wchar_t sys = (wchar_t)towupper(systemLetter);
    for (wchar_t c : candidates) {
        wchar_t u = (wchar_t)towupper(c);
        if (u >= L'A' && u <= L'Z' && u != sys)
            return u;
    }
    for (wchar_t c : candidates)
        if ((wchar_t)towupper(c) == sys)
            return sys;
    return 0;
}

unsigned long long AppTreeSize() { return TreeSizeRec(AppDir(), true); }

std::wstring LogBaseDir() {
    std::wstring root = AppDir();
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, MAX_PATH))
        return root;
    wchar_t sys = (wchar_t)towupper(win[0]);
    if (root.size() < 2 || (wchar_t)towupper(root[0]) != sys)
        return root;  // 软件不在系统盘：日志就在程序目录（PIT-104）
    // 软件在系统盘：找一块数据盘放日志（PIT-105，用户 2026-10-03 规格）——
    // 还原/重装系统盘会格式化系统盘，日志必须放别的盘才留得住。
    DWORD mask = GetLogicalDrives();
    std::vector<wchar_t> order;
    auto fixedNonSys = [&](wchar_t c) {
        if (!(mask & (1u << (c - L'A'))))
            return false;
        wchar_t drv[4] = {c, L':', L'\\', 0};
        return GetDriveTypeW(drv) == DRIVE_FIXED &&
               (wchar_t)towupper(c) != sys;
    };
    if (fixedNonSys(L'D'))
        order.push_back(L'D');  // 与 FindDataDrive 同规则：优先 D:
    for (wchar_t c = L'C'; c <= L'Z'; ++c)
        if (fixedNonSys(c) && c != L'D')
            order.push_back(c);
    // 两轮：先试**不含 Windows** 的数据盘（PE 下 C: 是离线系统盘，
    // 还原 C: 时日志会被格式化）；都不行再用含 Windows 的盘。
    for (int pass = 0; pass < 2; ++pass) {
        for (wchar_t c : order) {
            std::wstring drv = std::wstring(1, c) + L":\\";
            if (pass == 0 && HasWindows(drv))
                continue;
            std::wstring base = drv + kCopyDirName;
            CreateDirectoryW(base.c_str(), nullptr);
            if (DirWritable(base))
                return base;
        }
    }
    return root;  // 没有可写数据盘 → 只能留在系统盘（还原时日志会丢，别无选择）
}

RelocatePlan CheckRelocate() {
    RelocatePlan p;
    std::wstring root = AppDir();
    std::wstring rootOf =
        (root.size() >= 2 && root[1] == L':') ? root.substr(0, 3) : root;
    UINT type = GetDriveTypeW(rootOf.c_str());
    if (type == DRIVE_CDROM) {
        p.needed = true;
        p.why = L"cdrom";
    } else if (type == DRIVE_REMOVABLE) {
        p.needed = true;  // U盘（即便可写）也搬迁/询问（用户 2026-10-03 规格）
        p.why = L"removable";
    } else if (!DirWritable(root)) {
        p.needed = true;
        p.why = L"readonly";
    }
    if (!p.needed)
        return p;

    p.needBytes = AppTreeSize() + kSpaceMargin;
    wchar_t sys = 0;
    {
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        sys = (wchar_t)towupper(win[0]);
    }
    DWORD mask = GetLogicalDrives();
    std::vector<wchar_t> usable, noWindows;
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        wchar_t drv[4] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(drv) != DRIVE_FIXED)
            continue;
        ULARGE_INTEGER freeB{};
        if (!GetDiskFreeSpaceExW(drv, &freeB, nullptr, nullptr))
            continue;
        if (freeB.QuadPart < p.needBytes)
            continue;
        if (!DirWritable(drv))
            continue;
        usable.push_back(c);
        if (!HasWindows(drv))
            noWindows.push_back(c);  // 优先避开装了 Windows 的分区（还原目标）
    }
    const std::vector<wchar_t>& list = noWindows.empty() ? usable : noWindows;
    wchar_t pick = SelectRelocateDrive(list, sys);
    if (pick) {
        p.destDrive = std::wstring(1, pick) + L":\\";
        p.destRoot = p.destDrive + kCopyDirName;
        p.onlySystemDrive =
            ((wchar_t)towupper(pick) == sys) || HasWindows(p.destDrive);
    }
    return p;
}

bool DoRelocate(const RelocatePlan& plan, bool wait, int* exitCode,
                std::string* err) {
    if (exitCode)
        *exitCode = 0;
    if (plan.destRoot.empty()) {
        if (err)
            *err = Tr("未找到可写的本地磁盘");
        return false;
    }
    std::string detail;
    if (!CopyTree(AppDir(), plan.destRoot, true, detail)) {
        if (err)
            *err = std::string(Tr("复制失败：")) + detail;
        return false;
    }
    // 新副本 exe：保持相对应用根的路径（如 \x64\SysRecoverUI.exe）
    wchar_t exeBuf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exeBuf, MAX_PATH * 2 - 1);
    std::wstring exe = exeBuf;
    std::wstring rootDir = AppDir();
    std::wstring rel;
    if (exe.size() > rootDir.size() &&
        _wcsnicmp(exe.c_str(), rootDir.c_str(), rootDir.size()) == 0 &&
        exe[rootDir.size()] == L'\\')
        rel = exe.substr(rootDir.size());
    else {
        size_t pos = exe.find_last_of(L'\\');
        rel = (pos == std::wstring::npos) ? exe : exe.substr(pos);
    }
    std::wstring newExe = plan.destRoot + rel;
    std::wstring cmd = L"\"" + newExe + L"\"" + CmdLineTail();
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(newExe.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                          0, nullptr, plan.destRoot.c_str(), &si, &pi)) {
        char m[64];
        std::snprintf(m, sizeof(m), " (err=%lu)", GetLastError());
        if (err)
            *err = std::string(Tr("启动新副本失败")) + m;
        return false;
    }
    if (wait) {
        ::WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        if (exitCode)
            *exitCode = (int)code;
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

}  // namespace sysrecover
