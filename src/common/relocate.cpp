#include "relocate.h"

#include "i18n.h"
#include "logger.h"
#include "selfarch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace {

const wchar_t kCopyDirName[] = L"ZJRESTORE";  // 与恢复目录同名（用户 2026-10-03 定）
// 救援层的黑匣子兜底窝（PIT-137）：日志文件**直接摊在**该目录根下，不带 logs\ 子目录。
const wchar_t kFallbackLogDirName[] = L"ZJRESTORE-logs";
const unsigned long long kSpaceMargin = 32ull << 20;

// 日志根是否"无处可写"（规则 ④）—— 由 LogBaseDir() 每次判定，见 LogHomeDegraded()。
bool g_logHomeDegraded = false;

// SettleHome 后的日志根覆盖（LogBaseDir 优先返回它）；空 = 无覆盖。
std::wstring g_logHomeOverride;

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

// 是否运行在 WinPE 里（判据与 disk.cpp::IsWinPE 一致：Control\MiniNT 键存在）。
// 在 common 层重复这一小段是为了守住依赖 DAG（common 不依赖 disk，见 AGENTS §4）。
bool IsRunningInPe() {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Control\\MiniNT", 0, KEY_READ,
                      &h) != ERROR_SUCCESS)
        return false;
    RegCloseKey(h);
    return true;
}

// 已安装的程序目录 = `<ProgramFiles>\SysRecover`（规则 ①）。
// 注意 WOW64：32 位进程读到 `%ProgramFiles%` 仍是**原生** 64 位目录，与我们部署
// 程序时写的 `C:\Program Files\SysRecover` 一致（不用 %ProgramFiles(x86)%）。
std::wstring ProgramFilesSysRecoverDir() {
    wchar_t pf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    std::wstring base;
    if (n > 0 && n < MAX_PATH) {
        base = pf;
    } else {
        wchar_t win[MAX_PATH] = {};
        DWORD wl = GetWindowsDirectoryW(win, MAX_PATH);
        if (wl < 2 || win[1] != L':')
            return {};
        base = std::wstring(1, win[0]) + L":\\Program Files";
    }
    while (!base.empty() && base.back() == L'\\')
        base.pop_back();
    return base + L"\\SysRecover";
}

// 设隐藏属性（用户 2026-10-17："D:\ZJRESTORE 应设置隐藏"）。幂等，失败留证。
void HideDir(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return;  // 目录不存在：无事可做
    if (a & FILE_ATTRIBUTE_HIDDEN)
        return;  // 已隐藏
    if (!SetFileAttributesW(dir.c_str(), a | FILE_ATTRIBUTE_HIDDEN)) {
        sysrecover::LogWarn("hide dir failed: " + ToUtf8(dir) +
                            " err=" + std::to_string(GetLastError()));
        return;
    }
    // 复核：FAT/NTFS 都应生效；没生效说明该卷有别的行为，必须留证（否则静默不隐藏）。
    if (!(GetFileAttributesW(dir.c_str()) & FILE_ATTRIBUTE_HIDDEN))
        sysrecover::LogWarn("hide dir not sticky: " + ToUtf8(dir));
    else
        sysrecover::LogInfo("hide dir ok: " + ToUtf8(dir));
}

// 目标已存在时改成 `<名> (N)<扩展名>` —— 搬迁**绝不覆盖**已有日志。
std::wstring UniqueDest(const std::wstring& dst) {
    if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
        return dst;
    size_t slash = dst.find_last_of(L'\\');
    std::wstring dir =
        (slash == std::wstring::npos) ? std::wstring() : dst.substr(0, slash + 1);
    std::wstring name = dst.substr(slash + 1);
    std::wstring stem = name, ext;
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int n = 2; n < 1000; ++n) {
        std::wstring cand =
            dir + stem + L" (" + std::to_wstring(n) + L")" + ext;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES)
            return cand;
    }
    return dst;
}

// 只在**空**目录时删掉它（非空时保留 —— 里面可能还有 bootfix/脚本等资产）。
// 只清 READONLY（RemoveDirectoryW 需要），**保留 HIDDEN** 等其它属性 —— 早先一律清成
// NORMAL，把刚设好的隐藏位抹掉：D:\ZJRESTORE 非空删不掉，却已经变成不隐藏
// （2026-10-17 e2e 实测：日志 `hide dir ok: D:\ZJRESTORE` 但盘上 attr=0x10）。
bool RemoveEmptyDir(const std::wstring& dir) {
    if (dir.empty())
        return false;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(dir.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    return RemoveDirectoryW(dir.c_str()) != 0;
}

struct MigrateCount {
    int moved = 0;
    int failed = 0;
};

// 递归把 src 下所有文件**移动**到 dst（保留子目录结构）；不覆盖同名文件
// （见 UniqueDest）；跨卷/被占用时退化为"复制 + 登记下次启动删除"。
void MoveTreeMerge(const std::wstring& src, const std::wstring& dst,
                   MigrateCount& c) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            MoveTreeMerge(s, dst + L"\\" + fd.cFileName, c);
            continue;
        }
        std::wstring d = UniqueDest(dst + L"\\" + fd.cFileName);
        SetFileAttributesW(s.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (MoveFileExW(s.c_str(), d.c_str(),
                        MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
            ++c.moved;
            continue;
        }
        // 移动失败（跨卷/被打开/权限）→ 复制成功也算搬走，源文件登记开机删除。
        if (CopyFileW(s.c_str(), d.c_str(), FALSE) &&
            MoveFileExW(s.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            ++c.moved;
            continue;
        }
        ++c.failed;
        sysrecover::LogWarn("log migrate: move failed: " + ToUtf8(s) +
                            " (err=" + std::to_string(GetLastError()) + ")");
    } while (FindNextFileW(h, &fd));
    FindClose(h);
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

// 尽力删除整棵子树（先归零属性；失败不中断 —— 调用方会复核残留并留账）。
// 用于搬迁后清目的地的旧 logs（用户 2026-10-17 规格）。
void DeleteTreeBestEffort(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring full = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTreeBestEffort(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(dir.c_str());
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
                            wchar_t systemLetter, wchar_t avoidTarget) {
    wchar_t sys = (wchar_t)towupper(systemLetter);
    wchar_t avoid = (wchar_t)towupper(avoidTarget);
    for (wchar_t c : candidates) {
        wchar_t u = (wchar_t)towupper(c);
        if (u >= L'A' && u <= L'Z' && u != sys && u != avoid)
            return u;
    }
    // 回退：都在系统分区上则用系统分区（单分区机器）；目标盘 != 系统盘时不回退到目标盘
    for (wchar_t c : candidates) {
        wchar_t u = (wchar_t)towupper(c);
        // 回退条件：是系统盘，且（目标盘为空 或 系统盘就是目标盘 或 系统盘不是目标盘）
        // —— 总之：系统盘本身就是最后的回退（哪怕它也是目标盘，单分区机器没办法）
        if (u == sys)
            return u;
    }
    return 0;
}

void SetLogHomeOverride(const std::wstring& dir) {
    g_logHomeOverride = dir;
}

std::wstring ContractDir() {
    if (!g_logHomeOverride.empty())
        return g_logHomeOverride;
    return AppDir();
}

bool ShouldSettleHome(const std::wstring& exeDir, wchar_t targetLetter) {
    // 还原只格式化目标分区 —— 同盘不同分区的程序不受影响，无需搬迁。
    if (!targetLetter || exeDir.size() < 2 || exeDir[1] != L':')
        return false;
    return (wchar_t)towupper(exeDir[0]) == (wchar_t)towupper(targetLetter);
}

ZjrFolderKind ClassifyZjrResidue(const std::vector<std::wstring>& topNames) {
    // 保守策略（PIT-156）：看得懂的删、看不懂的留 —— 宁可残留也不误删用户数据。
    if (topNames.empty())
        return ZjrFolderKind::Empty;
    bool keepExe = false, keepMenu = false, ours = false;
    for (const auto& raw : topNames) {
        std::wstring n = raw;
        for (auto& ch : n)
            ch = (wchar_t)towlower(ch);
        if (n == L"sysrecover.exe" || n == L"sysrecoverui.exe")
            keepExe = true;  // SettleHome 复制来的程序的家（logs\ 正在被写）
        else if (n == L"boot" || n == L"scripts")
            keepMenu = true;  // 已安装启动菜单的救援载荷（精确名，非前缀：
                              // bootfix/bootfiles 不命中）
        else if (n == L"ea" || n == L"logs" || n == L"bootfix" ||
                 n == L"source.txt" || n == L"rescue-build.txt")
            ours = true;  // 我方一次性信号（还原期载荷/诊断）
    }
    if (keepExe || keepMenu)
        return ZjrFolderKind::Keep;
    if (ours)
        return ZjrFolderKind::Delete;
    return ZjrFolderKind::Keep;
}

unsigned long long AppTreeSize() { return TreeSizeRec(AppDir(), true); }

std::wstring LogBaseDir() {
    // SettleHome 覆盖（Q1「先落盘后搬家」）：优先返回覆盖目录
    if (!g_logHomeOverride.empty())
        return g_logHomeOverride;

    g_logHomeDegraded = false;
    const std::wstring appRoot = AppDir();
    wchar_t win[MAX_PATH] = {};
    DWORD winLen = GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring winDir = winLen ? std::wstring(win, winLen) : std::wstring();
    const wchar_t sys =
        (winLen >= 2 && win[1] == L':') ? (wchar_t)towupper(win[0]) : 0;
    const bool isPe = IsRunningInPe();

    // 规则 ①（用户 2026-10-17）：已安装 → <系统盘>\Program Files\SysRecover\logs。
    const std::wstring installDir = ProgramFilesSysRecoverDir();
    if (UseInstallDirLogHome(installDir, winDir, isPe)) {
        const std::wstring logs = installDir + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        if (DirWritable(logs))
            return installDir;
    }

    // 规则 ②：程序所在文件夹。PE 例外：PE 的 X:\ 是 RAM 盘（重启即失），
    // 且软件常常就在 X:\ 上 → 跳过它，让日志落数据盘（PIT-105/107 的 PE 行为）。
    const bool appOnWinDrive = sys != 0 && appRoot.size() >= 2 &&
                               (wchar_t)towupper(appRoot[0]) == sys;
    if (DirWritable(appRoot) && !(isPe && appOnWinDrive))
        return appRoot;

    // 规则 ③：只读介质（光盘/写保护）→ 除系统盘外第一个可写固定分区 \ZJRESTORE。
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
            if (DirWritable(base)) {
                HideDir(base);  // 盘根目录不留可见的我们自己的文件夹
                return base;
            }
        }
    }

    // 规则 ④：一个都没有 → 退回程序目录并标记（GUI/CLI 提示日志保不住）。
    g_logHomeDegraded = true;
    LogWarn("log home: nowhere writable (app dir + all fixed drives) -> could "
            "not save logs");
    return appRoot;
}

bool UseInstallDirLogHome(const std::wstring& installDir,
                          const std::wstring& winDir, bool isPe) {
    if (installDir.size() < 3 || installDir[1] != L':')
        return false;
    if (isPe)
        return false;  // PE 里 Program Files 可能是离线系统盘（还原目标）
    if (winDir.size() < 2 || winDir[1] != L':')
        return false;
    return (wchar_t)towupper(installDir[0]) == (wchar_t)towupper(winDir[0]);
}

bool LogHomeDegraded() { return g_logHomeDegraded; }

namespace {

bool MakeDirTree(const std::wstring& dir, int depth = 0) {
    if (dir.empty() || depth > 8) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos >= 2)
        MakeDirTree(dir.substr(0, pos), depth + 1);
    CreateDirectoryW(dir.c_str(), nullptr);
    const DWORD a2 = GetFileAttributesW(dir.c_str());
    return a2 != INVALID_FILE_ATTRIBUTES && (a2 & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

void EnsureTempDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n > 0) {
        std::wstring d(buf, n);
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/'))
            d.pop_back();
        if (MakeDirTree(d) && DirWritable(d)) return;
        LogWarn("temp dir unusable: " + ToUtf8(d) +
                " -> falling back to <Windows>\\Temp");
    }
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return;
    const std::wstring alt = std::wstring(win) + L"\\Temp";
    if (MakeDirTree(alt) && DirWritable(alt)) {
        SetEnvironmentVariableW(L"TEMP", alt.c_str());
        SetEnvironmentVariableW(L"TMP", alt.c_str());
        LogInfo("temp dir redirected to " + ToUtf8(alt));
    } else {
        LogWarn("temp dir fallback failed: " + ToUtf8(alt));
    }
}

std::string MigrateLogsToLogHome() {
    // PIT-156（2026-10-18）：触发面推广到**每次启动**（不再只限"备返克隆首启"）
    // —— 目的是给 CleanupStrayLogs 的"整个删掉残留文件夹"创造条件：先把残留里
    // 还有价值的日志搬进日志根，再让清理器放心删壳。两个守卫：
    //   · PE 不搬：日志根本来就在数据盘上（规则 ③），搬过去 = 自搬自；
    //   · 暂存待还原/返回旧系统期间不搬（PIT-115/150）：pending-restore.txt 还在，
    //     此刻日志根可能是临时位置、且马上要重启进救援层。
    if (IsRunningInPe())
        return {};
    const std::wstring activeBase = LogBaseDir();
    if (activeBase.empty())
        return {};
    const std::wstring dest = activeBase + L"\\logs";
    CreateDirectoryW(dest.c_str(), nullptr);
    if (!DirWritable(dest))
        return {};
    if (GetFileAttributesW((dest + L"\\pending-restore.txt").c_str()) !=
        INVALID_FILE_ATTRIBUTES) {
        LogInfo("log migrate: skipped (pending restore)");
        return {};
    }

    wchar_t win[MAX_PATH] = {};
    DWORD winLen = GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring winDir = winLen ? std::wstring(win, winLen) : std::wstring();
    const wchar_t sys = (winDir.size() >= 2 && winDir[1] == L':')
                            ? (wchar_t)towupper(winDir[0])
                            : 0;
    DWORD mask = GetLogicalDrives();
    MigrateCount c;
    int sources = 0;
    for (wchar_t d = L'C'; d <= L'Z'; ++d) {
        if (!(mask & (1u << (d - L'A'))))
            continue;
        if (sys != 0 && (wchar_t)towupper(d) == sys)
            continue;  // 系统盘不扫（日志根自己就在那儿）
        wchar_t drv[4] = {d, L':', L'\\', 0};
        if (GetDriveTypeW(drv) != DRIVE_FIXED)
            continue;
        const std::wstring base = std::wstring(drv) + kCopyDirName;      // 软件目录回退
        const std::wstring fb = std::wstring(drv) + kFallbackLogDirName; // 黑匣子兜底窝
        if (GetFileAttributesW(base.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // 源 == 日志根本身 → 跳过（否则自搬自；AppDir/规则 ① 路径无尾斜杠）
            if (_wcsicmp(base.c_str(), activeBase.c_str()) != 0) {
                HideDir(base);  // 用户 2026-10-17：<盘>:\ZJRESTORE 设隐藏
                const std::wstring src = base + L"\\logs";
                if (GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ++sources;
                    MoveTreeMerge(src, dest, c);
                    if (RemoveEmptyDir(src))
                        RemoveEmptyDir(base);  // 搬空且 ZJRESTORE 也空了 → 整个删掉
                }
                HideDir(base);  // 兜一层：删不掉（还有 bootfix 等）时确保隐藏位在
            }
        }
        if (GetFileAttributesW(fb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            HideDir(fb);
            ++sources;
            MoveTreeMerge(fb, dest, c);
            RemoveEmptyDir(fb);
        }
    }
    if (sources == 0 && c.moved == 0 && c.failed == 0)
        return {};  // 无源：每次启动少一行噪音（PIT-156）
    // 结尾留证：现场只看这一行就能判断"搬了几个源、成功/失败多少"。
    return "log migrate: moved=" + std::to_string(c.moved) +
           " failed=" + std::to_string(c.failed) +
           " sources=" + std::to_string(sources) + " dest=" + ToUtf8(dest);
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
    // 用户 2026-10-17 规格：光盘/U盘上的旧日志与本次运行无关（CopyTree 也
    // 从不复制源端 logs），但目的地可能残留以前版本运行留下的老日志 ——
    // 复制成功后先清掉目的地的旧 logs，再由新实例生成本次的新日志。
    // ⚠️ 必须在 CreateProcess 之前删：新实例启动即写 logs，之后删会误删新日志。
    {
        std::wstring staleLogs = plan.destRoot + L"\\logs";
        if (GetFileAttributesW(staleLogs.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            DeleteTreeBestEffort(staleLogs);
            if (GetFileAttributesW(staleLogs.c_str()) !=
                INVALID_FILE_ATTRIBUTES)
                sysrecover::LogWarn(
                    "relocate: stale destination logs removal incomplete: " +
                    ToUtf8(staleLogs));
            else
                sysrecover::LogInfo(
                    "relocate: stale destination logs removed: " +
                    ToUtf8(staleLogs));
        }
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
