// A 引擎残留检测与分流实现（0.7，用户 2026-10-09 规格；决策表 docs/23 §5）：
//   ~new： 过期(≥24h)→删；近期+有标记→Reuse（还原时比对 content 免重解压）；
//          近期+无标记→Redo（还原时自动删重解，启动期只记日志）
//   ~old： 根目录缺 Windows →AskUndo（任何账龄：~old 可能是仅存的旧系统，绝不自动删）；
//          同盘 ~new 仍在 →AskUndo（交换中途崩溃）；
//          其余（根完好且 ~new 已消费）→Delete（补做首启清理/过期垃圾）
// 注意：~old 的结构判据**优先于时间** —— "隔久直接删"只适用于"确定是垃圾"的场景。
#include "swapleft.h"

#include <windows.h>
#include <aclapi.h>

#include <condition_variable>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>

#include "../common/logger.h"
#include "../common/process.h"  // takeown/icacls/schtasks（权限兜底）
#include "../common/relocate.h"

namespace sysrecover {

namespace {

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

// ops.cpp 的 DeleteTreeRec 在匿名命名空间（内链，外部链接不到）→ 本文件自带。
// 用户 2026-10-15 实测「删除旧系统」无效：~old 是完整旧系统树，含
//   ① READONLY/SYSTEM 属性文件（DeleteFileW 直接失败）
//   ② junction（Users\...\Application Data 等）—— 递归进去会删错目标或撞句柄
//   ③ 失败静默、无计数，调用方看不出删没删掉
//   ④ MAX_PATH=260：深层路径（WinSxS/AppData 等）超限时 FindFirstFileW 直接失败
// 修法：\\?\ 前缀 + 清属性 → 判 reparse point 只删链接本身 → 计数 + 失败路径日志。
struct DelStat {
    long ok = 0;
    long fail = 0;
    long taken = 0;  // 多少个是靠提权夺权后才删掉的
    long long lastLogMs = 0;  // 上次进度日志时刻（GetTickCount64）
    long forceLogged = 0;  // ForceDeleteItem 已留证次数（前 20 次逐条）
};

// \\?\ 扩展长度前缀：绕过 MAX_PATH=260 限制（WinSxS/AppData 深层路径必需）
// 已有前缀则原样返回；盘符路径 D:\xx → \\?\D:\xx；UNC 路径 \\server\share → \\?\UNC\server\share
std::wstring ExtPath(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC" + p.substr(1);
    return L"\\\\?\\" + p;
}

// 启用指定特权（管理员默认持有但未启用）
void EnablePriv(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(tok);
}

// 取得所有权 + 给 Administrators 完全控制（对齐 takeown /f /r + icacls /grant）。
// ~old 里有 TrustedInstaller 拥有的系统目录，普通管理员连删都删不了。
void TakeOwnershipAndGrant(const std::wstring& path) {
    // 诊断（0.7.54）：前 10 次逐条 begin/done —— 根因不明时定位"死在哪次夺权"
    static long g_takeownN = 0;
    const bool logStep = g_takeownN < 10;
    if (logStep) {
        ++g_takeownN;
        LogInfo("swap leftover takeown begin #" + std::to_string(g_takeownN) +
                ": " + ToUtf8(path));
    }
    std::wstring ep = ExtPath(path);  // \\?\ 前缀，绕过 MAX_PATH
    // 1) 把 owner 换成 Administrators
    BYTE adminSidBuf[SECURITY_MAX_SID_SIZE] = {};
    DWORD sidSize = sizeof(adminSidBuf);
    PSID adminSid = (PSID)adminSidBuf;
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid,
                            &sidSize))
        return;
    SetNamedSecurityInfoW(const_cast<LPWSTR>(ep.c_str()), SE_FILE_OBJECT,
                          OWNER_SECURITY_INFORMATION, adminSid, nullptr,
                          nullptr, nullptr);

    // 2) 追加 Administrators 完全控制 ACE（保留原有 ACE，避免误伤）
    EXPLICIT_ACCESSW ea = {};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)adminSid;

    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR oldSd = nullptr;
    GetNamedSecurityInfoW(const_cast<LPWSTR>(ep.c_str()), SE_FILE_OBJECT,
                          DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl,
                          nullptr, &oldSd);
    PACL newDacl = nullptr;
    SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    if (newDacl) {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(ep.c_str()),
                              SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, newDacl, nullptr);
        LocalFree(newDacl);
    }
    // ⚠️ 必须释放 GetNamedSecurityInfoW 返回的**安全描述符**，不能释放 oldDacl
    // —— 后者是该描述符的**内部指针**（实测 oldDacl = sd + 0x14）。
    // 2026-10-17 现场实证：原来写成 `LocalFree(oldDacl)` → STATUS_HEAP_CORRUPTION
    // (0xC0000374) → 进程被 fastfail 秒杀（不产生用户态 dump、不经过 VEH/UEF）——
    // 症状就是「点删除旧系统后闪退、~old 一个字节都没删、下次开机又被问一次」。
    if (oldSd) LocalFree(oldSd);
    if (logStep)
        LogInfo("swap leftover takeown done #" + std::to_string(g_takeownN) +
                ": " + ToUtf8(path));
}

// 删除失败时的三级降级：清属性 → 夺权+完全控制 → 再删。成功记 taken。
bool ForceDeleteItem(const std::wstring& p, bool isDir, DelStat& st) {
    // 诊断（0.7.54）：前 20 次逐条 —— 定位"死在哪个文件/哪种操作"
    if (st.forceLogged < 20) {
        ++st.forceLogged;
        LogInfo("swap leftover force-del #" + std::to_string(st.forceLogged) +
                ": " + ToUtf8(p) + (isDir ? " [dir]" : " [file]"));
    }
    std::wstring ep = ExtPath(p);  // \\?\ 前缀，绕过 MAX_PATH
    SetFileAttributesW(ep.c_str(), FILE_ATTRIBUTE_NORMAL);
    BOOL ok = isDir ? RemoveDirectoryW(ep.c_str()) : DeleteFileW(ep.c_str());
    if (ok) return true;
    // 夺权 + 授完全控制（管理员场景下绝大多数 ACL 拒绝由此解除）
    TakeOwnershipAndGrant(p);
    SetFileAttributesW(ep.c_str(), FILE_ATTRIBUTE_NORMAL);
    ok = isDir ? RemoveDirectoryW(ep.c_str()) : DeleteFileW(ep.c_str());
    if (ok) {
        ++st.taken;
        // 夺权成功是"看起来卡死"的主因（每个 TI 目录都要 SetNamedSecurityInfo
        // 递归 + 再删），前 8 次留证 —— 有了它才能判断"慢但在动"还是"真卡死"
        if (st.taken <= 8)
            LogInfo("swap leftover takeown-then-delete ok: " + ToUtf8(p));
        return true;
    }
    return false;
}

void DeleteRec(const std::wstring& dir, DelStat& st,
               const DeleteProgressFn* progress, int depth) {
    // 诊断（0.7.54）：根与第一层子目录必留一条 —— 判断"进了哪个目录后死的"
    if (depth <= 1)
        LogInfo("swap leftover rec enter: " + ToUtf8(dir) +
                " depth=" + std::to_string(depth));
    // 深度安全网（0.7.55）：每帧栈占用 ≈ WIN32_FIND_DATAW(592B) + 两个
    // wstring + 编译器开销 ≈ 700B~1KB。真实 Windows 树极少超过 50 层
    // （junction 已在下方按 reparse 跳过，不会成环）；512 层给 8MB 线程栈
    // （main_win.cpp RunDeleteWithProgress）留 10 倍余量。病态超深树到顶后
    // 停止递归 —— 该子树留着不删（rmdir 报 DIR_NOT_EMPTY 记账留证），
    // 绝不把进程栈打穿（栈溢出 = 无 dump 秒退，正是 1016-5 现场症状）。
    if (depth >= 512) {
        ++st.fail;
        LogWarn("swap leftover depth cap: " + ToUtf8(dir) +
                " depth>=512, subtree left in place");
        return;
    }
    // 属性不为 0 的目录先归一，否则 RemoveDirectory 也可能被拒
    std::wstring edir = ExtPath(dir);
    SetFileAttributesW(edir.c_str(), FILE_ATTRIBUTE_NORMAL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((edir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        // ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND = 目录已空/已删（正常）
        // 其余 = 真失败（权限/路径/句柄），必须留账
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
            LogWarn("swap leftover enum fail: " + ToUtf8(dir) +
                    " err=" + std::to_string(err) +
                    " depth=" + std::to_string(depth));
    } else {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0)
                continue;
            // 保留备返标记：删除失败时它是"这是用户选择删除的旧系统"的证据
            // （成功了会在 DeleteSwapLeftover 里补删，不影响结果）。
            if (depth == 0 && std::wcscmp(fd.cFileName, L".zj-keepold") == 0)
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            // junction/symlink：只删链接本身，绝不递归进目标
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (ForceDeleteItem(p, true, st) ||
                    ForceDeleteItem(p, false, st))
                    ++st.ok;
                else
                    ++st.fail;
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteRec(p, st, progress, depth + 1);
                continue;
            }
            // 普通文件：清属性 → 夺权 → 再删
            if (ForceDeleteItem(p, false, st)) {
                ++st.ok;
            } else {
                ++st.fail;
                if (st.fail <= 8)
                    LogWarn("swap leftover del fail: " + ToUtf8(p) +
                            " err=" + std::to_string(GetLastError()));
            }
            // 进度：每 200 个文件回调一次（大树删除可显示进度）
            if (progress && *progress &&
                ((st.ok + st.fail) % 200) == 0)
                (*progress)(st.ok, st.fail, st.taken);
            // 进度日志：**全局**计数每 500 条 或 距上次 ≥10s 心跳（任一满足即打）。
            // 不再按单目录 localCount —— Windows 树单目录极少超 2000，
            // 删完整棵树也可能全程零日志（0.7.52 之前的盲区，用户现场只见
            // "contracts backed up" 后无下文）。
            long globalN = st.ok + st.fail;
            unsigned long long nowMs = GetTickCount64();
            if (globalN > 0 &&
                (globalN % 500 == 0 ||
                 nowMs - (unsigned long long)st.lastLogMs >= 10000)) {
                st.lastLogMs = (long long)nowMs;
                LogInfo("swap leftover del progress: ok=" +
                        std::to_string(st.ok) +
                        " fail=" + std::to_string(st.fail) +
                        " taken=" + std::to_string(st.taken) +
                        " cur=" + ToUtf8(dir));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(edir.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(edir.c_str())) {
        DWORD rerr = GetLastError();
        // ERROR_DIR_NOT_EMPTY = 还有子文件没删完（递归里失败过）
        // 其余 = 权限/句柄问题
        TakeOwnershipAndGrant(dir);
        SetFileAttributesW(edir.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!RemoveDirectoryW(edir.c_str())) {
            ++st.fail;
            LogWarn("swap leftover rmdir fail: " + ToUtf8(dir) +
                    " err=" + std::to_string(GetLastError()) +
                    " first_err=" + std::to_string(rerr) +
                    " depth=" + std::to_string(depth));
        } else {
            ++st.taken;
        }
    }
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
        // 备返标记：ops.cpp 暂存时写在目标根 → zjswap.sh 移入 ~old/
        out.keepOldMarker =
            GetFileAttributesW((dir + L"\\.zj-keepold").c_str()) !=
            INVALID_FILE_ATTRIBUTES;
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
    if (i.keepOldMarker) {
        // 备返模式（ops.cpp 暂存时写 .zj-keepold）：~old 是用户有意保留的，
        // 永不自动删除 —— 交给 AskUndo 对话框（用户可删/撤销/继续留着）。
        v.action = SwapLeftAction::AskUndo;
        v.reason = "~old: keep-old marker present (user chose to keep old system)";
    } else if (!i.rootIntact) {
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

// 克隆（备返）后首启判定：只查 <盘>:\~old\.zj-keepold 的属性 —— 该标记由
// ops.cpp 暂存时写在目标根、由 zjswap.sh 随旧系统树移进 ~old/，所以它存在
// 就等于「交换已完成 + 用户当初勾了备返」（暂存中还没换、交换中断时都没有）。
// 单次 GetFileAttributesW，主窗建窗期调用无感。
bool CloneFirstBootPending() {
    wchar_t buf[512] = {0};
    if (!GetLogicalDriveStringsW(511, buf)) return false;
    for (wchar_t* p = buf; *p; p += std::wcslen(p) + 1) {
        if (GetDriveTypeW(p) != DRIVE_FIXED) continue;
        std::wstring mk = RootPath(p[0]) + L"~old\\.zj-keepold";
        if (GetFileAttributesW(mk.c_str()) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    return false;
}

// 删除 ~old 前把根下的契约文件（_zjresy*.log / restore-task.conf|json）备份到
// 软件 logs\ 目录 —— 契约记录了「上次还原到哪块盘/哪个分区/哪个镜像」，
// 删掉后现场无从查证；备份成本几乎为零（几 KB）。
void BackupContractsBeforeDelete(const std::wstring& oldRoot) {
    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    int copied = 0;
    const wchar_t* pats[] = {L"_zjresy*.log", L"restore-task.conf",
                             L"restore-task.json"};
    for (const wchar_t* pat : pats) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((oldRoot + L"\\" + pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring src = oldRoot + L"\\" + fd.cFileName;
            std::wstring dst = logsDir + L"\\" + fd.cFileName;
            SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++copied;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (copied)
        LogInfo("contracts backed up before ~old delete: " +
                std::to_string(copied) + " file(s) -> logs/");
}

// 批量夺权（2026-10-30）：逐文件 API 夺权在部分 TrustedInstaller ACL 上仍 err=5
// （用户实测：takeown 调了 10 次、taken=0）。改用系统自带 takeown/icacls 对
// **剩余** 的少量文件做一次批量处理，再重试删除。
void BatchTakeOwnAndGrant(const std::wstring& dir, std::string& log) {
    const std::wstring tk = SysToolPath(L"takeown.exe");
    const std::wstring ic = SysToolPath(L"icacls.exe");
    std::string out;
    RunProcess(tk, L"/f \"" + dir + L"\" /r /d y /a", out);
    log += "takeown rc? out=" + out.substr(0, 400) + "\n";
    out.clear();
    RunProcess(ic, L"\"" + dir + L"\" /grant *S-1-5-32-544:F /t /c /q", out);
    log += "icacls out=" + out.substr(0, 400) + "\n";
}

// 最后兜底（2026-10-30）：仍删不掉 → 登记**下次开机**由 SYSTEM 计划任务删。
// 开机阶段没有用户进程占用、SYSTEM 对 TrustedInstaller 目录也有完全控制；
// 任务执行成功后会自删（cmd 删除自己 + schtasks /delete）。
bool g_bootCleanupScheduled = false;

bool ScheduleBootCleanup(const std::wstring& dir) {
    const std::wstring cmdPath = L"C:\\Windows\\Temp\\zj-cleanup-old.cmd";
    const std::string d = ToUtf8(dir);
    std::string body;
    body += "@echo off\r\n";
    body += "takeown /f \"" + d + "\" /r /d y /a >nul 2>&1\r\n";
    body += "icacls \"" + d + "\" /grant *S-1-5-32-544:F /t /c /q >nul 2>&1\r\n";
    body += "rd /s /q \"" + d + "\"\r\n";
    body += "if exist \"" + d + "\" exit /b 1\r\n";
    body += "del \"%~f0\"\r\n";
    body += "schtasks /delete /tn ZJSysRecoverOldCleanup /f >nul 2>&1\r\n";
    HANDLE h = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogWarn("boot cleanup: write cmd failed err=" +
                std::to_string(GetLastError()));
        return false;
    }
    DWORD wr = 0;
    WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    CloseHandle(h);
    std::string out;
    const int rc = RunProcess(
        SysToolPath(L"schtasks.exe"),
        L"/create /tn ZJSysRecoverOldCleanup /tr \"" + cmdPath +
            L"\" /sc onstart /ru SYSTEM /rl HIGHEST /f",
        out);
    LogInfo("boot cleanup task: rc=" + std::to_string(rc) + " " +
            out.substr(0, 200));
    g_bootCleanupScheduled = (rc == 0);
    return rc == 0;
}

bool LastDeleteBootCleanupScheduled() { return g_bootCleanupScheduled; }

void DeleteSwapLeftover(const SwapLeftInfo& i, DeleteProgressFn progress) {
    if (!i.drive) return;
    g_bootCleanupScheduled = false;
    std::wstring dir = RootPath(i.drive) + (i.isNew ? L"~new" : L"~old");
    BusyMark(i.drive);
    auto t0 = std::chrono::steady_clock::now();
    LogInfo("swap leftover delete: " + DriveStr(i.drive) +
            (i.isNew ? " ~new" : " ~old") + " (age " +
            std::to_string(i.ageHours) + "h)");
    if (IsDir(dir)) {
        // 备份契约文件（仅 ~old；~new 没有契约）
        if (!i.isNew) BackupContractsBeforeDelete(dir);
        // 启用特权：~old 里有 TrustedInstaller 拥有的系统目录，普通管理员
        // 连删都删不了 —— 必须启用 SeTakeOwnershipPrivilege + SeBackup/
        // SeRestorePrivilege 才能夺权后删除（对齐 takeown /f）。
        EnablePriv(L"SeTakeOwnershipPrivilege");
        EnablePriv(L"SeBackupPrivilege");
        EnablePriv(L"SeRestorePrivilege");
        // 锚点日志：进入删除本体的标志 —— 之前只有 "swap leftover delete"
        // （决策）与 "contracts backed up"；此后到 delete done/incomplete 之间
        // 曾是日志盲区（0.7.52），现在 begin + 全局进度日志补齐。
        LogInfo("swap leftover tree delete begin: " + ToUtf8(dir));
        // 根目录预夺权：~old 整棵树常为 TrustedInstaller 所有，先给根授
        // Administrators 完全控制（子项在 ForceDeleteItem/DeleteRec 里逐个夺）。
        TakeOwnershipAndGrant(dir);
        // 诊断（0.7.54）：列出 ~old 顶层条目（前 20 + 总数）——看清"在删什么"
        {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((ExtPath(dir) + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                int n = 0;
                std::string listing;
                do {
                    if (std::wcscmp(fd.cFileName, L".") == 0 ||
                        std::wcscmp(fd.cFileName, L"..") == 0)
                        continue;
                    if (n < 20) {
                        if (n) listing += ", ";
                        listing += ToUtf8(fd.cFileName);
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                            listing += "/";
                    }
                    ++n;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                LogInfo("swap leftover tree top (" + std::to_string(n) +
                        " entries): " + listing);
            } else {
                LogWarn("swap leftover tree top enum fail: err=" +
                        std::to_string(GetLastError()));
            }
        }
        DelStat st;
        DeleteRec(dir, st, progress ? &progress : nullptr, 0);
        // ── 权限兜底（2026-10-30）────────────────────────────────────
        // 首轮失败（典型：.NET/TrustedInstaller ACL，err=5）→ 系统 takeown +
        // icacls 批量处理剩余项，再删一次；仍不行 → 登记下次开机 SYSTEM 任务。
        bool bootCleanup = false;
        // 只差 .zj-keepold（删除成功的常见收尾）→ 清标记 + rmdir，别走夺权
        {
            const std::wstring mk = ExtPath(dir) + L"\\\\.zj-keepold";
            if (GetFileAttributesW(mk.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(mk.c_str());
                RemoveDirectoryW(ExtPath(dir).c_str());
            }
        }
        if (IsDir(dir)) {
            std::string blog;
            LogInfo("swap leftover: batch takeown/icacls retry (fail=" +
                    std::to_string(st.fail) + ")");
            BatchTakeOwnAndGrant(dir, blog);
            LogInfo("swap leftover batch: " + blog);
            DelStat st2;
            DeleteRec(dir, st2, progress ? &progress : nullptr, 0);
            st.ok += st2.ok;
            st.fail = st2.fail;
            st.taken += st2.taken;
            // 只剩 .zj-keepold 时补删并 rmdir
            if (IsDir(dir)) {
                DeleteFileW((ExtPath(dir) + L"\\.zj-keepold").c_str());
                RemoveDirectoryW(ExtPath(dir).c_str());
            }
            if (IsDir(dir)) {
                bootCleanup = ScheduleBootCleanup(dir);
                LogWarn("swap leftover: still present after retry -> boot "
                        "cleanup scheduled=" +
                        std::string(bootCleanup ? "yes" : "no"));
            }
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        // 删完复核：~old 仍在 = 没删干净（用户实测「点了删除没反应」就是
        // 这里静默失败）——必须留账，否则下次启动又弹框、还原还被门禁拦。
        if (IsDir(dir)) {
            LogWarn("swap leftover delete incomplete: " + DriveStr(i.drive) +
                    (i.isNew ? " ~new" : " ~old") +
                    " ok=" + std::to_string(st.ok) +
                    " fail=" + std::to_string(st.fail) +
                    " taken=" + std::to_string(st.taken) +
                    " elapsed=" + std::to_string(ms) + "ms" +
                    (bootCleanup ? " (dir still present; boot cleanup scheduled)"
                                 : " (dir still present)"));
        } else {
            LogInfo("swap leftover delete done: " + DriveStr(i.drive) +
                    (i.isNew ? " ~new" : " ~old") +
                    " ok=" + std::to_string(st.ok) +
                    " fail=" + std::to_string(st.fail) +
                    " taken=" + std::to_string(st.taken) +
                    " elapsed=" + std::to_string(ms) + "ms");
        }
        // 最后回调一次（小树可能一次都没到 200 的阈值）
        if (progress) progress(st.ok, st.fail, st.taken);
    } else {
        LogWarn("swap leftover delete: " + DriveStr(i.drive) +
                (i.isNew ? " ~new" : " ~old") +
                " dir not found (already gone?)");
    }
    BusyUnmark(i.drive);
}

void DeleteSwapLeftoverAsync(const SwapLeftInfo& i) {
    std::thread([i] { DeleteSwapLeftover(i); }).detach();
}

void WaitSwapCleanup(wchar_t drive) {
    std::unique_lock<std::mutex> lk(g_busyMx);
    g_busyCv.wait(lk, [drive] { return g_busy.count(drive) == 0; });
}

// ⚠️ `UndoSwapLeftover` 已移除（PIT-139）：它曾是「返回旧系统」的 Windows 侧
// 实现 —— 根下同名一律 conflict 却把返回值写成 `conflict==0 && failed==0`，
// 于是「一有同名就报失败、其实一个字节都没搬」；且在 Windows 里改名正在运行的
// 系统树本就不可回滚。取代者见 ops::StageReturnOld + bootfiles/zjswap-undo.sh。

}  // namespace sysrecover
