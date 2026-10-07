// refscan.cpp — 悬空引用扫描/清理实现（PIT-132，2026-10-07）。见 refscan.h。
#include "refscan.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <set>

namespace sysrecover {
namespace refscan {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return s;
}

std::string W2U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr,
                            nullptr);
    return s;
}

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[2048] = {};
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    if (n == 0 || n > 2048) return s;
    return buf;
}

bool FileMissing(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES;
}

// 32 位进程在 64 位系统上（WOW64）：注册表同样有重定向（PIT-082 家族），
// 必须显式选视图：32 位进程默认看 32 位视图，要读 64 位视图得加
// KEY_WOW64_64KEY（反之亦然）。
bool IsWow64() {
    BOOL b = FALSE;
    typedef BOOL(WINAPI * Fn)(HANDLE, PBOOL);
    if (HMODULE k = GetModuleHandleW(L"kernel32.dll")) {
        auto fn = reinterpret_cast<Fn>(
            reinterpret_cast<void*>(GetProcAddress(k, "IsWow64Process")));
        if (fn) fn(GetCurrentProcess(), &b);
    }
    return b != FALSE;
}

bool Is64BitOs() {
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64;
}

// 64 位视图 / 32 位视图各一遍（32 位系统上无 WOW64 → 单遍）。
struct Views {
    REGSAM v64 = 0;
    REGSAM v32 = 0;
    bool dual = false;
};
Views GetViews() {
    Views v;
    if (!Is64BitOs()) return v;  // 单遍（view=0）
    v.dual = true;
    bool wow = IsWow64();
    v.v64 = wow ? KEY_WOW64_64KEY : 0;
    v.v32 = wow ? 0 : KEY_WOW64_32KEY;
    return v;
}

const wchar_t* RootName(HKEY h) {
    if (h == HKEY_LOCAL_MACHINE) return L"HKLM";
    if (h == HKEY_USERS) return L"HKU";
    if (h == HKEY_CURRENT_USER) return L"HKCU";
    return L"HK?";
}

struct RootCtx {
    HKEY h = nullptr;
    REGSAM view = 0;
    std::wstring classesRel;  // CLSID 相对根：如 L"SOFTWARE\\Classes\\CLSID\\"
};

const wchar_t* const kCtxRoots[] = {
    L"*\\shellex\\ContextMenuHandlers",
    L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
    L"Directory\\shellex\\ContextMenuHandlers",
    L"Directory\\Background\\shellex\\ContextMenuHandlers",
    L"Drive\\shellex\\ContextMenuHandlers",
    L"Folder\\shellex\\ContextMenuHandlers",
    L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shellex\\"
    L"ContextMenuHandlers",
};
const wchar_t* kOverlayRel =
    L"Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellIconOverlayIdentifiers";
const wchar_t* kHooksRel =
    L"Microsoft\\Windows\\CurrentVersion\\Explorer\\ShellExecuteHooks";
const wchar_t* kRunRel = L"Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunOnceRel = L"Microsoft\\Windows\\CurrentVersion\\RunOnce";

HKEY OpenWithView(HKEY root, const std::wstring& sub, REGSAM view,
                  REGSAM access) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, access | view, &k) != ERROR_SUCCESS)
        return nullptr;
    return k;
}

// 注册表"路径值"清理：去引号；无引号时截到首个 .dll/.exe（容忍路径空格）；
// 最后展开环境变量。空串返回空。
std::wstring CleanPathValue(const std::wstring& raw) {
    std::wstring d = raw;
    if (!d.empty() && d[0] == L'"') {
        size_t e = d.find(L'"', 1);
        if (e != std::wstring::npos) d = d.substr(1, e - 1);
    } else {
        std::wstring low = Lower(d);
        size_t ex = low.find(L".dll");
        if (ex == std::wstring::npos) ex = low.find(L".exe");
        if (ex != std::wstring::npos) {
            d = d.substr(0, ex + 4);
        } else {
            size_t sp = d.find(L' ');
            if (sp != std::wstring::npos) d = d.substr(0, sp);
        }
    }
    if (d.empty()) return {};
    return Expand(d);
}

// 单个 (root, classesRel, view) 下的 CLSID → InprocServer32 默认值。
std::wstring TryClsidIn(HKEY root, const std::wstring& classesRel, REGSAM view,
                        const std::wstring& c) {
    HKEY k = OpenWithView(root, classesRel + c + L"\\InprocServer32", view,
                          KEY_READ);
    if (!k) return {};
    wchar_t p[1024] = {};
    DWORD cb = sizeof(p) - 2, t = 0;
    RegQueryValueExW(k, nullptr, nullptr, &t, (LPBYTE)p, &cb);
    RegCloseKey(k);
    return CleanPathValue(p);
}

// CLSID → InprocServer32 默认值。找不到空。
// ★ 跨 hive 解析（2026-10-07 客户实测）：右键处理器（*\shellex\…）与其
//   CLSID 的 InprocServer32 可能分处不同 hive（便携软件常见：一处 HKLM、
//   一处 HKCU）。只在本根解析会双双漏掉——diag 用 HKCR 合并视图却能看到
//   → "诊断标 MISSING、清理却不动它"的怪象（PIT-134）。
// 顺序：本根/本视图 → HKLM 双视图 → HKCU → 所有已加载用户 hive。
std::wstring ResolveClsid(const RootCtx& rc, const wchar_t* clsid) {
    if (!clsid || !clsid[0]) return {};
    std::wstring c = clsid;
    if (c[0] != L'{') c = L"{" + c + L"}";
    std::wstring p = TryClsidIn(rc.h, rc.classesRel, rc.view, c);
    if (!p.empty()) return p;
    p = TryClsidIn(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\",
                   KEY_WOW64_64KEY, c);
    if (!p.empty()) return p;
    p = TryClsidIn(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\",
                   KEY_WOW64_32KEY, c);
    if (!p.empty()) return p;
    // HKCU/HKU 同样要过 64/32 两个视图（x86 补写器 view=0 只看 32 位视图，
    // 64 位程序注册的 CLSID 就在 64 位视图里——2026-10-07 实测 found=0 的坑）。
    for (REGSAM view : {REGSAM(KEY_WOW64_64KEY), REGSAM(KEY_WOW64_32KEY),
                        REGSAM(0)}) {
        p = TryClsidIn(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\", view,
                       c);
        if (!p.empty()) return p;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(HKEY_USERS, i, name, &nl, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        size_t len = wcslen(name);
        if (len >= 8 && !_wcsicmp(name + len - 8, L"_Classes")) continue;
        for (REGSAM view : {REGSAM(KEY_WOW64_64KEY), REGSAM(KEY_WOW64_32KEY),
                            REGSAM(0)}) {
            p = TryClsidIn(HKEY_USERS,
                           std::wstring(name) + L"\\Software\\Classes\\CLSID\\",
                           view, c);
            if (!p.empty()) return p;
        }
    }
    return {};
}

// 收录"指向易失目录"的引用（两种状态都收：missingNow 标记区分）——
// 备份视角：存在也收（还原后必失）；首启清理视角：只动 missingNow 的。
void PushIfDangling(std::vector<DanglingRef>& out, const RootCtx& rc,
                    const std::wstring& fullSub, const std::wstring& valueName,
                    const std::wstring& file, bool wholeKey = false,
                    bool isTask = false) {
    if (file.empty()) return;
    if (!IsVolatilePath(file)) return;
    DanglingRef r;
    r.hroot = rc.h;
    r.view = rc.view;
    r.subKey = fullSub;
    r.valueName = valueName;
    r.filePath = file;
    r.missingNow = FileMissing(file);
    r.wholeKey = wholeKey;
    r.isTask = isTask;
    if (isTask) {
        r.display = L"task " + fullSub + L" -> " + file;
    } else {
        r.display = std::wstring(RootName(rc.h)) + L"\\" + fullSub;
        if (!valueName.empty()) r.display += L"!" + valueName;
        r.display += L" -> " + file;
    }
    r.display += r.missingNow ? L"  [missing]" : L"  [at-risk]";
    out.push_back(r);
}

// 子键形态（子键名=友好名、默认值=CLSID；或子键名=CLSID、默认值=显示名）
void ScanCtxSubkeys(const RootCtx& rc, const std::wstring& rel,
                    std::vector<DanglingRef>& out) {
    HKEY k = OpenWithView(rc.h, rel, rc.view, KEY_READ);
    if (!k) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        HKEY h = nullptr;
        if (RegOpenKeyExW(k, name, 0, KEY_READ | rc.view, &h) != ERROR_SUCCESS)
            continue;
        wchar_t clsid[128] = {};
        DWORD cb = sizeof(clsid) - 2, t = 0;
        RegQueryValueExW(h, nullptr, nullptr, &t, (LPBYTE)clsid, &cb);
        RegCloseKey(h);
        const wchar_t* cls = (name[0] == L'{') ? name : clsid;
        PushIfDangling(out, rc, rel + L"\\" + name, L"",
                       ResolveClsid(rc, cls), /*wholeKey=*/true);
    }
    RegCloseKey(k);
}

// COM 服务器（CLSID 下 InprocServer32 / LocalServer32 默认值）指向易失目录。
// rc.classesRel 已指向 <...>\Classes\CLSID\；枚举其下每个 CLSID。
void ScanComServersIn(const RootCtx& rc, std::vector<DanglingRef>& out) {
    std::wstring parent = rc.classesRel;
    while (!parent.empty() && parent.back() == L'\\') parent.pop_back();
    HKEY k = OpenWithView(rc.h, parent, rc.view, KEY_READ);
    if (!k) return;
    static const wchar_t* kServers[] = {L"InprocServer32", L"LocalServer32"};
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        for (const wchar_t* srv : kServers) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(k, (std::wstring(name) + L"\\" + srv).c_str(), 0,
                              KEY_READ | rc.view, &h) != ERROR_SUCCESS)
                continue;
            wchar_t p[1024] = {};
            DWORD cb = sizeof(p) - 2, t = 0;
            RegQueryValueExW(h, nullptr, nullptr, &t, (LPBYTE)p, &cb);
            RegCloseKey(h);
            PushIfDangling(out, rc, parent + L"\\" + name + L"\\" + srv, L"",
                           CleanPathValue(p), /*wholeKey=*/true);
        }
    }
    RegCloseKey(k);
}

// 服务 ImagePath 指向易失目录（HKLM\SYSTEM 不分视图，单遍）。
void ScanServices(std::vector<DanglingRef>& out) {
    RootCtx rc;
    rc.h = HKEY_LOCAL_MACHINE;
    rc.view = 0;
    const std::wstring base = L"SYSTEM\\CurrentControlSet\\Services";
    HKEY k = OpenWithView(rc.h, base, 0, KEY_READ);
    if (!k) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        HKEY h = nullptr;
        if (RegOpenKeyExW(k, name, 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        wchar_t p[1024] = {};
        DWORD cb = sizeof(p) - 2, t = 0;
        LONG q = RegQueryValueExW(h, L"ImagePath", nullptr, &t, (LPBYTE)p, &cb);
        RegCloseKey(h);
        if (q != ERROR_SUCCESS) continue;
        PushIfDangling(out, rc, base + L"\\" + name, L"ImagePath",
                       CleanPathValue(p));
    }
    RegCloseKey(k);
}

// 计划任务：System32\Tasks 下 XML 的 <Command> 指向易失目录。
void ScanTasksDir(const std::wstring& root, const std::wstring& dir, int depth,
                  std::vector<DanglingRef>& out) {
    if (depth < 0) return;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanTasksDir(root, full, depth - 1, out);
            continue;
        }
        HANDLE f = CreateFileW(full.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD sz = GetFileSize(f, nullptr);
        std::vector<char> buf;
        if (sz > 0 && sz <= 2 * 1024 * 1024) {
            buf.resize(sz);
            DWORD rd = 0;
            if (ReadFile(f, buf.data(), sz, &rd, nullptr))
                buf.resize(rd);
            else
                buf.clear();
        }
        CloseHandle(f);
        if (buf.size() < 4) continue;
        std::wstring xml;
        size_t off = 0;
        if ((unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE)
            off = 2;
        size_t n = (buf.size() - off) / 2;
        xml.resize(n);
        memcpy(&xml[0], buf.data() + off, n * 2);
        std::vector<std::wstring> cmds = ExtractTaskCommands(xml);
        if (cmds.empty()) continue;
        std::wstring rel = full.substr(root.size() + 1);
        RootCtx rc;  // 字段未用（isTask 特判），仅为满足签名
        for (auto& c : cmds)
            PushIfDangling(out, rc, rel, L"", CleanPathValue(c), false, true);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ScanTasks(std::vector<DanglingRef>& out) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring dir = std::wstring(sys) + L"\\Tasks";
    ScanTasksDir(dir, dir, 3, out);
}

// schtasks /Delete（TaskCache 与 XML 一并处理）
bool RemoveTask(const std::wstring& rel, std::wstring& err) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmdline =
        L"\"" + std::wstring(sys) + L"\\schtasks.exe\" /Delete /TN \"" + rel +
        L"\" /F";
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = L"schtasks CreateProcess failed";
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (rc != 0) {
        err = L"schtasks rc=" + std::to_wstring(rc);
        return false;
    }
    return true;
}

// 值形态（值名=CLSID，数据=描述；如 ShellExecuteHooks）
void ScanExtValues(const RootCtx& rc, const std::wstring& rel,
                   std::vector<DanglingRef>& out) {
    HKEY k = OpenWithView(rc.h, rel, rc.view, KEY_READ);
    if (!k) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        BYTE data[512] = {};
        DWORD dl = sizeof(data) - 2, t = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &t, data, &dl) !=
            ERROR_SUCCESS)
            break;
        PushIfDangling(out, rc, rel, name, ResolveClsid(rc, name));
    }
    RegCloseKey(k);
}

// Run / RunOnce（值名=项名，数据=命令行）
void ScanRunKeys(const RootCtx& rc, const std::wstring& rel,
                 std::vector<DanglingRef>& out) {
    HKEY k = OpenWithView(rc.h, rel, rc.view, KEY_READ);
    if (!k) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512] = {};
        DWORD nl = 511;
        BYTE data[2048] = {};
        DWORD dl = sizeof(data) - 2, t = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &t, data, &dl) !=
            ERROR_SUCCESS)
            break;
        if (t != REG_SZ && t != REG_EXPAND_SZ) continue;
        std::wstring exe = ExtractExecutable(std::wstring((wchar_t*)data));
        if (exe.empty()) continue;
        PushIfDangling(out, rc, rel, name, Expand(exe));
    }
    RegCloseKey(k);
}

// 一个"用户根"（prefix 如 L"S-1-5-21-..." 或 L"ZJREF_1"）——HKCU 等价结构。
// ★ 视图修正（2026-10-07 PIT-134，客户豆包实测）：`HKCU\Software\Classes` 及
//   其下的类名/CLSID 树**受 WOW64 重定向**——x86 补写器用 view=0 只能看到
//   32 位视图（Wow6432Node），64 位程序注册的壳扩展/COM 全部漏掉（实测
//   applier 跑完 found=0，而 64 位视图里明明有）。机器侧本来就扫双视图，
//   所以 e2e（引用全注入 HKLM）没暴露；用户侧必须同样 64/32 各扫一遍。
//   共享部分（Explorer 覆盖图标/ShellExecuteHooks/Run）也一并双视图，重复
//   项由清理端的去重兜住（同一键两次命中→第二次直接跳过）。
void ScanUserRoot(const std::wstring& prefix, std::vector<DanglingRef>& out) {
    for (REGSAM view : {REGSAM(KEY_WOW64_64KEY), REGSAM(KEY_WOW64_32KEY)}) {
        RootCtx rc;
        rc.h = HKEY_USERS;
        rc.view = view;
        rc.classesRel = prefix + L"\\Software\\Classes\\CLSID\\";
        for (const wchar_t* s : kCtxRoots)
            ScanCtxSubkeys(rc, prefix + L"\\Software\\Classes\\" + s, out);
        ScanCtxSubkeys(rc, prefix + L"\\Software\\" + kOverlayRel, out);
        ScanExtValues(rc, prefix + L"\\Software\\" + kHooksRel, out);
        ScanRunKeys(rc, prefix + L"\\Software\\" + kRunRel, out);
        ScanRunKeys(rc, prefix + L"\\Software\\" + kRunOnceRel, out);
        ScanComServersIn(rc, out);
    }
    // ★ 预登录磁盘 hive（ZJREF_n，非 SID 名）兜底（2026-10-07 PIT-134）：WOW64
    //   重定向是否作用于手动挂载的 hive 取决于路径结构、不确定——显式再扫一遍
    //   字面 `Software\Classes\Wow6432Node\`（view=64 读字面路径），保证 32 位
    //   程序注册的壳扩展/COM 也不漏。SID 路径由上面的 view=32 遍覆盖，不重复。
    if (prefix.rfind(L"S-1-", 0) != 0) {
        RootCtx rc;
        rc.h = HKEY_USERS;
        rc.view = KEY_WOW64_64KEY;
        rc.classesRel =
            prefix + L"\\Software\\Classes\\Wow6432Node\\CLSID\\";
        for (const wchar_t* s : kCtxRoots)
            ScanCtxSubkeys(
                rc, prefix + L"\\Software\\Classes\\Wow6432Node\\" + s, out);
        ScanComServersIn(rc, out);
    }
}

// 机器级一遍（指定视图）：HKLM 下 classes/overlay/hooks/run。
void ScanMachineView(REGSAM view, std::vector<DanglingRef>& out) {
    RootCtx rc;
    rc.h = HKEY_LOCAL_MACHINE;
    rc.view = view;
    rc.classesRel = L"SOFTWARE\\Classes\\CLSID\\";
    for (const wchar_t* s : kCtxRoots)
        ScanCtxSubkeys(rc, std::wstring(L"SOFTWARE\\Classes\\") + s, out);
    ScanCtxSubkeys(rc, std::wstring(L"SOFTWARE\\") + kOverlayRel, out);
    ScanExtValues(rc, std::wstring(L"SOFTWARE\\") + kHooksRel, out);
    ScanRunKeys(rc, std::wstring(L"SOFTWARE\\") + kRunRel, out);
    ScanRunKeys(rc, std::wstring(L"SOFTWARE\\") + kRunOnceRel, out);
    ScanComServersIn(rc, out);
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr,
                                    nullptr);
    bool done = ok && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return done;
}

}  // namespace

bool IsVolatilePath(const std::wstring& path) {
    std::wstring p = Lower(path);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (p.rfind(L"\\??\\", 0) == 0) p = p.substr(4);
    static const wchar_t* kVol[] = {
        L"\\appdata\\local\\temp\\",
        L"\\windows\\temp\\",
        L"\\windows\\cbstemp\\",
        L"\\windows\\winsxs\\installtemp\\",
        L"\\appdata\\local\\microsoft\\windows\\inetcache\\",
    };
    for (const wchar_t* v : kVol)
        if (p.find(v) != std::wstring::npos) return true;
    return false;
}

std::wstring ExtractExecutable(const std::wstring& command) {
    size_t i = 0;
    while (i < command.size() && (command[i] == L' ' || command[i] == L'\t'))
        ++i;
    if (i >= command.size()) return {};
    if (command[i] == L'"') {
        size_t e = command.find(L'"', i + 1);
        if (e == std::wstring::npos) return command.substr(i + 1);
        return command.substr(i + 1, e - i - 1);
    }
    std::wstring rest = command.substr(i);
    size_t ex = Lower(rest).find(L".exe");
    if (ex != std::wstring::npos) return rest.substr(0, ex + 4);
    size_t sp = rest.find(L' ');
    return sp == std::wstring::npos ? rest : rest.substr(0, sp);
}

std::vector<std::wstring> ExtractTaskCommands(const std::wstring& xml) {
    std::vector<std::wstring> out;
    const std::wstring open = L"<Command>";
    const std::wstring close = L"</Command>";
    size_t pos = 0;
    while (true) {
        size_t a = xml.find(open, pos);
        if (a == std::wstring::npos) break;
        size_t b = xml.find(close, a + open.size());
        if (b == std::wstring::npos) break;
        std::wstring cmd = xml.substr(a + open.size(), b - a - open.size());
        if (!cmd.empty()) out.push_back(cmd);
        pos = b + close.size();
    }
    return out;
}

std::vector<DanglingRef> ScanDanglingRefs(bool includeLoadedUserHives) {
    std::vector<DanglingRef> out;
    Views v = GetViews();
    ScanMachineView(v.v64, out);
    if (v.dual) ScanMachineView(v.v32, out);
    ScanServices(out);  // HKLM\SYSTEM 不分视图
    ScanTasks(out);
    if (includeLoadedUserHives) {
        for (DWORD i = 0;; ++i) {
            wchar_t name[256] = {};
            DWORD nl = 255;
            if (RegEnumKeyExW(HKEY_USERS, i, name, &nl, nullptr, nullptr,
                              nullptr, nullptr) != ERROR_SUCCESS)
                break;
            size_t len = wcslen(name);
            if (len >= 8 && !_wcsicmp(name + len - 8, L"_Classes")) continue;
            ScanUserRoot(name, out);
        }
    }
    return out;
}

bool RemoveRef(const DanglingRef& r, std::wstring& err) {
    if (r.isTask) return RemoveTask(r.subKey, err);
    if (r.wholeKey) {
        size_t pos = r.subKey.find_last_of(L'\\');
        std::wstring parent =
            pos == std::wstring::npos ? std::wstring() : r.subKey.substr(0, pos);
        std::wstring leaf =
            pos == std::wstring::npos ? r.subKey : r.subKey.substr(pos + 1);
        HKEY k = OpenWithView(r.hroot, parent, r.view, KEY_READ | KEY_WRITE);
        if (!k) {
            err = L"open parent failed";
            return false;
        }
        LONG rc = RegDeleteTreeW(k, leaf.c_str());
        RegCloseKey(k);
        if (rc != ERROR_SUCCESS) {
            err = L"RegDeleteTree rc=" + std::to_wstring(rc);
            return false;
        }
        return true;
    }
    HKEY k = OpenWithView(r.hroot, r.subKey, r.view, KEY_SET_VALUE);
    if (!k) {
        err = L"open key failed";
        return false;
    }
    LONG rc = RegDeleteValueW(k, r.valueName.empty() ? nullptr
                                                     : r.valueName.c_str());
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) {
        err = L"RegDeleteValue rc=" + std::to_wstring(rc);
        return false;
    }
    return true;
}

CleanStats CleanAllDanglingRefs(
    const std::function<void(const std::string&)>& log, std::string& backupOut,
    const std::wstring& usersDir) {
    CleanStats st;
    auto emit = [&](const std::string& s) {
        if (log) log(s);
    };
    std::set<std::wstring> seen;  // 双视图重复命中同一键：第二次直接跳过
    auto cleanList = [&](std::vector<DanglingRef>& refs) {
        for (auto& r : refs) {
            if (!seen.insert(r.display).second) continue;
            st.found++;
            if (!r.missingNow) {
                // 文件还在（如卷级还原保留了易失目录）→ 引用有效，不动。
                st.atRisk++;
                emit("refs: at-risk (file exists) kept: " + W2U8(r.display));
                continue;
            }
            std::wstring err;
            if (RemoveRef(r, err)) {
                st.cleaned++;
                emit("refs: removed " + W2U8(r.display));
                backupOut += "removed: " + W2U8(r.display) + "\r\n";
            } else {
                st.failed++;
                emit("refs: remove FAILED (" + W2U8(err) +
                     "): " + W2U8(r.display));
                backupOut += "failed: " + W2U8(r.display) + "\r\n";
            }
        }
    };

    // ① 机器级（HKLM 双视图）
    std::vector<DanglingRef> m = ScanDanglingRefs(false);
    cleanList(m);

    // ② 已加载的用户 hive（登录后/异步执行时）
    std::vector<std::wstring> loaded;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(HKEY_USERS, i, name, &nl, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        size_t len = wcslen(name);
        if (len >= 8 && !_wcsicmp(name + len - 8, L"_Classes")) continue;
        loaded.push_back(name);
    }
    for (auto& n : loaded) {
        std::vector<DanglingRef> r;
        ScanUserRoot(n, r);
        cleanList(r);
    }

    // ③ 磁盘上的用户 hive（登录前：加载→扫描→删除→卸载）
    EnablePrivilege(L"SeRestorePrivilege");
    EnablePrivilege(L"SeBackupPrivilege");
    std::wstring ud = usersDir;
    if (ud.empty()) {
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        ud.assign(win, wcslen(win) >= 2 ? 2 : wcslen(win));
        ud += L"\\Users";
    }
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((ud + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        int idx = 0;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (!_wcsicmp(fd.cFileName, L"Default") ||
                !_wcsicmp(fd.cFileName, L"Public") ||
                !_wcsicmp(fd.cFileName, L"All Users"))
                continue;
            std::wstring hive = ud + L"\\" + fd.cFileName + L"\\NTUSER.DAT";
            if (GetFileAttributesW(hive.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            wchar_t keyName[64] = {};
            swprintf(keyName, 64, L"ZJREF_%d", idx++);
            LONG rc = RegLoadKeyW(HKEY_USERS, keyName, hive.c_str());
            if (rc != ERROR_SUCCESS) {
                emit("refs: user hive load skipped (" + W2U8(fd.cFileName) +
                     ", rc=" + std::to_string(rc) + ")");
                continue;
            }
            std::vector<DanglingRef> r;
            ScanUserRoot(keyName, r);
            cleanList(r);
            RegUnLoadKeyW(HKEY_USERS, keyName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return st;
}

}  // namespace refscan
}  // namespace sysrecover
