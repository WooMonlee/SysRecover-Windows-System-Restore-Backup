#include "vss.h"
#include <windows.h>
#include "logger.h"

using sysrecover::LogError;
using sysrecover::LogInfo;

namespace vss {
namespace {

struct SvcSpec {
    const wchar_t* name;   // 服务名（sc 命令原样用）
    const wchar_t* cn;     // 提示用中文名
    const wchar_t* disp;   // services.msc 里的显示名
    const char* log;       // 日志用窄名
    bool* was_running;
};

bool QueryState(SC_HANDLE h, DWORD* state) {
    SERVICE_STATUS_PROCESS s = {};
    DWORD need = 0;
    if (!QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO,
                              reinterpret_cast<LPBYTE>(&s), sizeof(s), &need))
        return false;
    *state = s.dwCurrentState;
    return true;
}

bool WaitState(SC_HANDLE h, DWORD want, DWORD timeout_ms) {
    for (DWORD waited = 0; waited <= timeout_ms; waited += 250) {
        DWORD st = 0;
        if (!QueryState(h, &st)) return false;
        if (st == want) return true;
        Sleep(250);
    }
    return false;
}

std::wstring FixHint(const SvcSpec& s) {
    std::wstring t = L"处理（任选其一，在管理员窗口操作）：\r\n";
    t += L"  1) Win+R 输入 services.msc → 找到「";
    t += s.disp;
    t += L"」→ 启动类型改为「手动」→ 点「启动」\r\n";
    t += L"  2) 命令行执行：sc config ";
    t += s.name;
    t += L" start= demand && sc start ";
    t += s.name;
    return t;
}

std::wstring Head() { return L"备份前检查失败："; }

const SvcSpec* SpecList(bool* a, bool* b) {
    static SvcSpec k[2];
    k[0] = {L"VSS", L"卷影复制服务(VSS)", L"Volume Shadow Copy", "vss", a};
    k[1] = {L"swprv", L"影子副本提供程序(swprv)",
            L"Microsoft Software Shadow Copy Provider", "swprv", b};
    return k;
}

}  // namespace

bool BackupGuard::Ensure(std::wstring& err) {
    err.clear();

    // ① WoW64：wimlib.h 明文 VSS 快照不能在 WOW64 下运行（64 位系统要 64 位程序）。
    //    正常路径 selfarch 已自举成 x64；走到这里说明 x64\SysRecover.exe 缺失/没起来。
    BOOL wow = FALSE;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) {
        err = Head() + L"当前以 32 位程序运行在 64 位 Windows 上。\r\n"
              L"wimlib 的卷影快照不支持 WOW64 模式，热备份必然失败（rc=89）。\r\n"
              L"请改用安装目录 x64\\ 下的 64 位程序后重试。";
        LogError("vss precheck: 32-bit process on 64-bit OS (WoW64), snapshot unsupported");
        return false;
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        err = Head() + L"无法打开服务控制管理器（错误 " +
              std::to_wstring(GetLastError()) + L"）。";
        return false;
    }

    bool ok = true;
    const SvcSpec* k = SpecList(&vss_was_running_, &swprv_was_running_);
    for (int i = 0; i < 2 && ok; i++) {
        const SvcSpec& s = k[i];
        SC_HANDLE h = OpenServiceW(scm, s.name,
                                   SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
        if (!h) {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_DOES_NOT_EXIST)
                err = Head() + L"系统缺少服务「" + s.cn +
                      L"」（服务不存在），无法创建卷影快照。\r\n"
                      L"（精简/封装版 Windows 可能删除了它；不修复则备份必报 "
                      L"rc=89: Unable to create a filesystem snapshot。）";
            else
                err = Head() + L"无法打开服务「" + s.cn + L"」（错误 " +
                      std::to_wstring(e) + L"）。";
            ok = false;
            break;
        }
        DWORD st = 0;
        if (!QueryState(h, &st)) {
            err = Head() + L"读取服务「" + s.cn + L"」状态失败（错误 " +
                  std::to_wstring(GetLastError()) + L"）。";
            CloseServiceHandle(h);
            ok = false;
            break;
        }
        // 记录原状态：只在亲眼看到「停止/待停」时标 false（= 备份后才允许停回）
        *s.was_running = (st == SERVICE_RUNNING || st == SERVICE_START_PENDING);
        if (!*s.was_running) {
            if (!StartServiceW(h, 0, nullptr)) {
                DWORD e = GetLastError();
                if (e != ERROR_SERVICE_ALREADY_RUNNING) {
                    if (e == ERROR_SERVICE_DISABLED)
                        err = Head() + L"服务「" + s.cn +
                              L"」已被禁用，无法创建卷影快照。\r\n"
                              L"（热备份必需；不修复则备份必报 "
                              L"rc=89: Unable to create a filesystem snapshot。）\r\n" +
                              FixHint(s);
                    else
                        err = Head() + L"启动服务「" + s.cn + L"」失败（错误 " +
                              std::to_wstring(e) + L"）。\r\n" + FixHint(s);
                    CloseServiceHandle(h);
                    ok = false;
                    break;
                }
            }
            if (!WaitState(h, SERVICE_RUNNING, 30000)) {
                err = Head() + L"服务「" + s.cn +
                      L"」启动超时（30 秒未进入运行状态），可能被安全软件或策略拦截。\r\n" +
                      FixHint(s);
                CloseServiceHandle(h);
                ok = false;
                break;
            }
            LogInfo(std::string("vss precheck: started ") + s.log +
                    " (was stopped, will stop after backup)");
        } else {
            LogInfo(std::string("vss precheck: ") + s.log + " already running");
        }
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
    // 失败也返回 false 交给析构收尾：上面已启动的服务（was=false 且现运行）会被停回，
    // 不留"检查失败却把服务开在那里"的残留。
    return ok;
}

BackupGuard::~BackupGuard() {
    if (vss_was_running_ && swprv_was_running_) return;  // 全程没动过 → 快速返回
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    const SvcSpec* k = SpecList(&vss_was_running_, &swprv_was_running_);
    for (int i = 0; i < 2; i++) {  // 顺序：先停协调方 VSS，再停提供程序 swprv
        const SvcSpec& s = k[i];
        if (*s.was_running) continue;  // 备份前就在跑 → 不动（只关我们开的）
        SC_HANDLE h = OpenServiceW(scm, s.name, SERVICE_QUERY_STATUS | SERVICE_STOP);
        if (!h) continue;
        DWORD st = 0;
        if (QueryState(h, &st) && st != SERVICE_STOPPED &&
            st != SERVICE_STOP_PENDING) {
            SERVICE_STATUS ss = {};
            if (ControlService(h, SERVICE_CONTROL_STOP, &ss))
                WaitState(h, SERVICE_STOPPED, 10000);
            LogInfo(std::string("vss precheck: stopped ") + s.log +
                    " after backup (was stopped before we started it)");
        }
        CloseServiceHandle(h);
    }
    CloseServiceHandle(scm);
}

}  // namespace vss
