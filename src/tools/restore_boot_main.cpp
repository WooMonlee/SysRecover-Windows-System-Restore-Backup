// zj-restore-boot.exe —— “恢复引导菜单”首启清理器
//
// 日常还原（非「安装菜单」）把本程序投放到新系统 ProgramData，并由公共
// 启动文件夹的 cmd 在第一次登录时启动；启动时先弹说明框，再自行请求 UAC
// （非提权进程 → MessageBox → ShellExecute runas），删除**临时**引导项：
//   - UEFI：描述为 "SysRecover Temporary Restore" 的 Boot#### + BootNext；
//   - BIOS：临时 GUID 的 BCD 条目 + bootsequence；
//   - 若无常驻菜单，还清 ESP\EFI\ZJRESTORE 和系统盘根 boot 文件。
// 常驻菜单（"SysRecover" / 原 RecoveryGuid）绝不触碰。
//
// 不链接产品库（保持 x86 小体积、可在 x86/x64 目标运行）；bcdedit/mountvol
// 走 Sysnative（32 位进程在 64 位系统上必须避开 SysWOW64，PIT-082）。
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootnames.h"

namespace {

using namespace sysrecover;

const wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
const wchar_t kRunOnceName[] = L"\u6062\u590d\u5f15\u5bfc\u83dc\u5355";

std::wstring gLog;

void Log(const std::string& s) {
    if (gLog.empty()) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(ts) + s + "\r\n";
    HANDLE h = CreateFileW(gLog.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, line.data(), (DWORD)line.size(), &wr, nullptr);
        CloseHandle(h);
    }
}

std::wstring ProgramData() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramData", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring s = win;
    if (s.size() >= 2 && s[1] == L':') s = s.substr(0, 2);
    return s + L"\\ProgramData";
}

std::wstring Sys32(const wchar_t* exe) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    std::wstring base = dir;
    // WOW64：32 位进程的 System32 被重定向；bcdedit/mountvol 只在原生目录。
    BOOL wow = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) {
        size_t p = base.find_last_of(L"\\/");
        if (p != std::wstring::npos) base = base.substr(0, p) + L"\\Sysnative";
    }
    return base + L"\\" + exe;
}

void EnableEnvPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return;
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(tok);
}

std::wstring VarName(int n) {
    wchar_t b[16];
    swprintf(b, 16, L"Boot%04X", n);
    return b;
}

bool ReadVar(const std::wstring& name, std::vector<BYTE>& out) {
    DWORD cap = 8192;
    out.resize(cap);
    DWORD n = GetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid,
                                              out.data(), cap);
    if (n == 0) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
}

bool WriteVar(const std::wstring& name, const void* data, DWORD size) {
    return SetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid,
                                            const_cast<void*>(data), size) != 0;
}

bool DeleteVar(const std::wstring& name) {
    return SetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid, nullptr,
                                            0) != 0;
}

std::wstring OptionDesc(const std::vector<BYTE>& b) {
    if (b.size() < 8) return {};
    WORD fpll = 0;
    memcpy(&fpll, &b[4], 2);
    (void)fpll;
    std::wstring d;
    size_t off = 6;
    while (off + 2 <= b.size()) {
        wchar_t c = 0;
        memcpy(&c, &b[off], 2);
        off += 2;
        if (c == 0) break;
        d.push_back(c);
    }
    return d;
}

int FindUefiEntry(const wchar_t* desc) {
    std::vector<BYTE> order;
    if (!ReadVar(L"BootOrder", order)) return -1;
    for (size_t i = 0; i + 1 < order.size(); i += 2) {
        WORD v = 0;
        memcpy(&v, &order[i], 2);
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b)) continue;
        if (OptionDesc(b) == desc) return v;
    }
    return -1;
}

void RemoveUefiEntry(int num) {
    if (num < 0) return;
    std::vector<BYTE> order;
    if (ReadVar(L"BootOrder", order)) {
        std::vector<WORD> keep;
        for (size_t i = 0; i + 1 < order.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &order[i], 2);
            if (v != (WORD)num) keep.push_back(v);
        }
        std::vector<BYTE> nb(keep.size() * 2);
        for (size_t i = 0; i < keep.size(); ++i)
            memcpy(&nb[i * 2], &keep[i], 2);
        WriteVar(L"BootOrder", nb.data(), (DWORD)nb.size());
    }
    DeleteVar(VarName(num));
    WORD next = 0;
    DWORD n = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid,
                                              &next, sizeof(next));
    if (n == sizeof(next) && next == (WORD)num) DeleteVar(L"BootNext");
    Log("uefi temporary entry removed");
}

// 同步执行并抓 stdout（bcdedit 条目存在性、mountvol 不必抓）。
bool RunCapture(const std::wstring& exe, const std::wstring& args,
                std::string& out) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (ok) {
        char tmp[1024];
        DWORD n = 0;
        while (ReadFile(rd, tmp, sizeof(tmp), &n, nullptr) && n > 0)
            out.append(tmp, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    return ok != FALSE;
}

bool RunNoCapture(const std::wstring& exe, const std::wstring& args) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return rc == 0;
}

bool BcdMenuExists() {
    std::string out;
    if (!RunCapture(Sys32(L"bcdedit.exe"), std::wstring(L"/enum ") + RecoveryGuid(),
                    out))
        return false;
    return out.find("{12345678-1234-1234-1234-123456789abc}") !=
           std::string::npos ||
           out.find("{12345678-1234-1234-1234-123456789ABC}") !=
               std::string::npos;
}

bool BcdTempExists() {
    std::string out;
    if (!RunCapture(Sys32(L"bcdedit.exe"),
                    std::wstring(L"/enum ") + TemporaryGuid(), out))
        return false;
    return out.find("{87654321-4321-4321-4321-cba987654321}") !=
           std::string::npos;
}

bool RemoveTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            RemoveTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0;
}

void RemoveEspRescue() {
    DWORD used = GetLogicalDrives();
    for (wchar_t c = L'S'; c <= L'Z'; ++c) {
        if (used & (1u << (c - L'A'))) continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        std::wstring args = std::wstring(1, c) + L": /s";
        if (!RunNoCapture(Sys32(L"mountvol.exe"), args)) continue;
        std::wstring dir = std::wstring(root) + L"EFI\\ZJRESTORE";
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveTree(dir);
            Log("ESP rescue dir removed");
        }
        RunNoCapture(Sys32(L"mountvol.exe"), std::wstring(1, c) + L": /d");
        return;
    }
    Log("ESP not mounted (no free letter or mountvol failed)");
}

void RemoveBcdTemp() {
    if (!BcdTempExists()) return;  // 不要动用户自己的 bootsequence
    RunNoCapture(Sys32(L"bcdedit.exe"),
                 std::wstring(L"/delete ") + TemporaryGuid() + L" /f");
    RunNoCapture(Sys32(L"bcdedit.exe"),
                 L"/deletevalue {bootmgr} bootsequence");
    Log("temporary BCD entry removed");
}

void RemoveSelfAndRunOnce() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, kRunOnceName);
        RegCloseKey(k);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!MoveFileExW(exe, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        // 延迟删除失败也不影响清理结果；下次可手工删除。
        Log("self delete schedule failed");
    }
}

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return false;
    TOKEN_ELEVATION te{};
    DWORD n = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &n);
    CloseHandle(tok);
    return ok && te.TokenIsElevated;
}

bool SelfElevate(const std::wstring& logDir) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = L"--elevated";
    sei.lpDirectory = logDir.c_str();
    sei.nShow = SW_HIDE;
    SetLastError(0);
    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        Log("elevation request failed err=" + std::to_string(e) +
            (e == ERROR_CANCELLED ? " (user cancelled UAC)" : ""));
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}

void WriteDoneMarker() {
    const std::wstring p = ProgramData() + L"\\SysRecover\\boot-cleanup.done";
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("done marker write failed");
        return;
    }
    const char k[] = "done\r\n";
    DWORD wr = 0;
    WriteFile(h, k, (DWORD)sizeof(k) - 1, &wr, nullptr);
    CloseHandle(h);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool elevatedArg =
        argc > 1 && wcscmp(argv[1], L"--elevated") == 0;
    const std::wstring pd = ProgramData() + L"\\SysRecover";
    CreateDirectoryW((ProgramData()).c_str(), nullptr);
    CreateDirectoryW(pd.c_str(), nullptr);
    gLog = pd + L"\\boot-cleanup.log";

    // 先告知，再请求 UAC：本进程是普通权限（manifest=asInvoker），
    // 对话框结束后才 ShellExecute(runas)，所以 UAC 不会抢在提示之前。
    if (!elevatedArg && !IsElevated()) {
        Log("non-elevated start: show notice before UAC");
        const wchar_t* msg =
            L"本次还原使用了一次性启动菜单。\n"
            L"接下来会清理临时引导项，需要管理员权限。\n\n"
            L"点“确定”后，请在 Windows 的 UAC 提示中选择“是”。\n"
            L"如果取消，系统会在下次登录时再次提示。";
        int r = MessageBoxW(nullptr, msg, L"恢复引导菜单",
                            MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND |
                                MB_TOPMOST);
        if (r != IDOK) {
            Log("user cancelled notice (no UAC requested)");
            return 0;
        }
        if (!SelfElevate(pd)) return 1;
        return 0;
    }

    Log("restore-boot cleanup start (elevated=" +
        std::string(elevatedArg ? "arg" : "token") + ")");
    EnableEnvPrivilege();

    const int temp = FindUefiEntry(kTempEntryDesc);
    const bool menuUefi = FindUefiEntry(kMenuEntryDesc) >= 0;
    RemoveUefiEntry(temp);
    RemoveBcdTemp();

    const bool menu = menuUefi || BcdMenuExists();
    if (menu) {
        Log("persistent menu present -> keep menu files/entry");
    } else {
        RemoveEspRescue();
        wchar_t win[MAX_PATH] = {};
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring root = win;
        size_t p = root.find(L":\\");
        if (p != std::wstring::npos) root = root.substr(0, p + 2);
        for (const wchar_t* nm : {L"grldr", L"grldr.mbr", L"menu.lst"}) {
            std::wstring f = root + nm;
            SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(f.c_str());
        }
        Log("temporary boot files removed");
    }
    WriteDoneMarker();
    RemoveSelfAndRunOnce();
    Log("restore-boot cleanup done (menu=" + std::string(menu ? "1" : "0") + ")");
    return 0;
}
