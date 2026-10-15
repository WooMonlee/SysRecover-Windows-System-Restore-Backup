// SysRecoverUI 入口（经典 Duilib，纯 Win32，MinGW 原生支持）。
// 注意：C++ 标准头已在 main_form.h 中先于 StdAfx.h 引入（PIT-012）。
#include "../common/i18n.h"
#include "main_form.h"

#include "../boot/grub.h"  // sysrecover::ExeDir()（启动器布局下的应用根目录）
#include "../common/crash.h"     // 崩溃处理（minidump + 可读日志）
#include "../common/logger.h"    // LogInfo（启动耗时自检）
#include "../common/process.h"   // MsSinceProcessStart
#include "../common/selfarch.h"  // 位数自举：32 位程序在 64 位系统上换成 x64\同名
#include "../common/relocate.h"  // 运行目录搬迁（光盘/U盘：复制到本地固定分区）
#include "../app/swapleft.h"     // A 引擎残留检测与分流（0.7）
#include "../app/ops.h"           // StageReturnOld / DetectReturnOldCapability（PIT-139）
#include "../disk/disk.h"         // PartitionInfo / EnumerateDisks（按盘符定位目标）
using sysrecover::Tr;
#include "confirm_dlg.h"
#include "instance_dlg.h"

namespace { std::wstring U2W8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
} }  // namespace

using namespace DuiLib;

namespace {
// 窗口尺寸与老界面实测一致（745x410；客户区 = 整窗，无系统外框）
const int kWndW   = 745;
const int kWndH   = 410;
const int kCorner = 24;  // 圆角直径（radius 12；Duilib 的 roundcorner/borderround
                         // 都是「椭圆宽高」= 直径，见 AGENTS.md §13）

const wchar_t* kWndClass = L"SysRecoverUI";  // = CMainForm::GetWindowClassName()

// 单实例互斥体名：Global\ = 整机唯一（跨会话）。命名内核对象要
// SeCreateGlobalPrivilege —— 本 exe 已 requireAdministrator 提权，所以拿得到；
// 万一 CreateMutex 失败则降级为「不做单实例检查」，不影响主流程。
const wchar_t* kMutexName = L"Global\\SysRecoverUI_SingleInstance";

// 查旧实例是否正在执行备份/还原。查询失败（超时 / 被 UIPI 拦截）一律当作「忙」：
// 安全优先 —— 宁可不给关，也不能让用户误关掉正在写盘的进程。
bool QueryPrevBusy(HWND hPrev) {
    DWORD_PTR r = 0;
    if (!::SendMessageTimeoutW(hPrev, WM_SR_QUERY_BUSY, 0, 0, SMTO_ABORTIFHUNG, 1500, &r))
        return true;
    return r != 0;
}

// 把已有窗口带到前台（必要时还原最小化）。本进程刚被用户启动、握有前台权限，
// 因此 SetForegroundWindow 到别的进程的窗口是被允许的。
void ActivatePrev(HWND hPrev) {
    ::ShowWindow(hPrev, ::IsIconic(hPrev) ? SW_RESTORE : SW_SHOW);
    ::BringWindowToTop(hPrev);
    ::SetForegroundWindow(hPrev);
}

// 请旧实例退出，并等它真正走完（最多 5s），避免两个实例短暂并存。
void ClosePrevAndWait(HWND hPrev) {
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hPrev, &pid);
    HANDLE hp = pid ? ::OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    ::PostMessageW(hPrev, WM_CLOSE, 0, 0);
    if (hp) {
        ::WaitForSingleObject(hp, 5000);
        ::CloseHandle(hp);
    }
}

// 立即重启（与 CMainForm::RebootNow 同款实现；本块在 WinMain 里、还没有窗体）。
// 不用 shutdown.exe 的 /t：那会弹「一分钟后关闭」对话框。两条路都记返回码。
using sysrecover::LogInfo;
using sysrecover::LogWarn;
using sysrecover::RunProcess;
using sysrecover::SysToolPath;
using sysrecover::Tr;
void RebootNowG() {
    HANDLE hToken = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        if (::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME,
                                    &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            ::SetLastError(0);
            if (!::AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr,
                                         nullptr) ||
                ::GetLastError() == ERROR_NOT_ALL_ASSIGNED)
                LogWarn("reboot: enable SeShutdownPrivilege failed err=" +
                        std::to_string(::GetLastError()));
        }
        ::CloseHandle(hToken);
    }
    if (!::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                         SHTDN_REASON_MAJOR_APPLICATION |
                             SHTDN_REASON_MINOR_MAINTENANCE |
                             SHTDN_REASON_FLAG_PLANNED)) {
        DWORD e1 = ::GetLastError();
        std::string out;
        int rc = RunProcess(SysToolPath(L"shutdown.exe"), L"/r /t 0", out);
        LogWarn("reboot: ExitWindowsEx failed err=" + std::to_string(e1) +
                "; shutdown.exe rc=" + std::to_string(rc) + " out=" + out);
    } else {
        LogInfo("reboot: ExitWindowsEx ok");
    }
}

// ── 「返回旧系统」（PIT-139，方案 C）──────────────────────────────────
// **先确认、后暂存**：确认框的「取消」是安全项（回车/ESC 都落这里）——
// 用户选了取消就必须**一个字节都没动**，否则 BCD 里的单次启动会在他下次
// 手动重启时把机器带回救援层（那不是他想要的）。
// 确认之后才 StageReturnOld（写契约 + 部署引导 + 单次启动），成功即重启。
// 目录反向交换（根 -> ~new 后丢弃；~old -> 根）一律在 Linux 救援层完成：
// 在 Windows 里改名正在运行的系统树不可回滚，救援层里没有进程占用。
// owner = 主窗口 HWND：PIT-140（用户 2026-10-13）——启动期弹框原来一律传
// nullptr，于是对话框与主窗口是两个互不相关的顶层窗口：既不在主窗口之上
// （用户看到「处理备返的旧系统」藏在主界面背后），也没有模态禁用（主界面
// 还能点）。带 owner 后 RunModal 会 EnableWindow(owner,FALSE) 并且**被拥有
// 窗口永远在 owner 之上**（Win32 Z 序保证），正好满足「不选择不允许做其它事」。
bool StageReturnOldFlow(HWND owner, wchar_t drive) {
    sysrecover::PartitionInfo target;
    bool found = false;
    for (const auto& d : sysrecover::EnumerateDisks())
        for (const auto& p : d.parts)
            if (!p.letter.empty() &&
                ::towupper(p.letter[0]) == ::towupper(drive)) {
                target = p;
                found = true;
            }
    if (!found) {
        ::MessageBoxW(owner, Tr(L"找不到该分区，无法返回旧系统。"),
                      Tr(L"返回旧系统"), MB_OK | MB_ICONWARNING);
        return false;
    }
    bool hasOld = false, hasRoot = false;
    std::string det;
    if (!sysrecover::DetectReturnOldCapability(target, &hasOld, &hasRoot,
                                               det) ||
        !hasOld) {
        ::MessageBoxW(owner,
                      (std::wstring(Tr(L"该分区上没有可用的旧系统（~old\\Windows）。\n")) +
                       U2W8(det))
                          .c_str(),
                      Tr(L"返回旧系统"), MB_OK | MB_ICONWARNING);
        return false;
    }
    std::wstring msg =
        std::wstring(Tr(L"将重启进入恢复环境，把旧系统搬回根目录。")) + L"\n" +
        Tr(L"当前新系统将被丢弃，无法再使用。") + L"\n" +
        Tr(L"确认后立即重启执行。");
    if (CConfirmDlg::Ask2(owner, Tr(L"返回旧系统"), msg, Tr(L"取消"),
                          Tr(L"立即重启"),
                          /*defaultIsRight=*/false) != 1) {
        LogInfo("return-old cancelled by user (nothing was written)");
        return false;
    }
    std::string err;
    bool needReboot = false;
    int rc = sysrecover::StageReturnOld(target, err, &needReboot);
    if (rc != 0) {
        ::MessageBoxW(owner,
                      (std::wstring(Tr(L"无法返回旧系统：\n")) + U2W8(err))
                          .c_str(),
                      Tr(L"返回旧系统"), MB_OK | MB_ICONWARNING);
        return false;
    }
    RebootNowG();
    return true;
}
}  // namespace

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 位数自举：32 位 GUI 在 64 位 Windows 上换成 <ExeDir>\x64\SysRecoverUI.exe 再跑
    {
        int reexecCode = 0;
        if (sysrecover::ReexecX64IfNeeded(&reexecCode, /*wait=*/false))
            return reexecCode;
    }
    // 崩溃处理：装在最前面（真实进程），崩溃时把 dump+文本写到 <日志根>\logs\crash\。
    sysrecover::InstallCrashHandler(sysrecover::LogBaseDir());
    // i18n：词典必须在任何 Tr() 与建窗之前装好（皮肤 XML 也是在建窗时翻译的）。
    // GUI 只认 SYSRECOVER_LANG 环境变量 + 系统界面语言（CLI 另支持 --lang）。
    sysrecover::InitI18n(nullptr);
    CPaintManagerUI::SetInstance(hInstance);
    // 资源根 = **应用目录**（不是 exe 目录）：启动器布局下 exe 在 <root>\x86\ 或 \x64\ 子目录里，
    // 而 skin\ 放在 <root>（与位数无关）→ 必须用 sysrecover::ExeDir()（它会自动上移一级）。
    CDuiString skinPath = sysrecover::ExeDir().c_str();
    skinPath += _T("\\skin\\");
    CPaintManagerUI::SetResourcePath(skinPath.GetData());

    HRESULT hr = ::CoInitialize(nullptr);
    if (FAILED(hr))
        return 1;

    // ── 单实例检查 ────────────────────────────────────────────────────
    // hMutex 持有到进程退出（不主动 CloseHandle）：只要本进程活着，后来者
    // 就会拿到 ERROR_ALREADY_EXISTS。
    HANDLE hMutex = ::CreateMutexW(nullptr, FALSE, kMutexName);
    if (hMutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND hPrev = ::FindWindowW(kWndClass, nullptr);
        if (hPrev) {
            int choice = CInstanceDlg::Ask(QueryPrevBusy(hPrev));
            if (choice == CInstanceDlg::kUsePrevious) {
                ActivatePrev(hPrev);
                ::CloseHandle(hMutex);
                ::CoUninitialize();
                return 0;
            }
            if (choice == CInstanceDlg::kQuit) {
                ::CloseHandle(hMutex);
                ::CoUninitialize();
                return 0;
            }
            // kUseCurrent：请旧实例退出并等它收尾
            ClosePrevAndWait(hPrev);
            // 旧实例若因正在执行任务而拒绝关闭（或没响应），此刻仍在运行。
            // 绝不硬闯成双实例 —— 把它带回前台，本进程退出。
            hPrev = ::FindWindowW(kWndClass, nullptr);
            if (hPrev) {
                ActivatePrev(hPrev);
                ::MessageBoxW(nullptr,
                              Tr(L"之前的程序正在执行任务，无法关闭。\n" L"已为你切回该窗口，请等任务结束后再重新打开。"),
                              Tr(L"九转还原"), MB_OK | MB_ICONINFORMATION);
                ::CloseHandle(hMutex);
                ::CoUninitialize();
                return 0;
            }
        }
        // 找不到窗口：旧实例正在启动/退出途中 —— 直接继续，让本次成为新实例。
    }
    (void)hMutex;  // 另有分支已 CloseHandle；此处仅表示「本进程持锁」这一语义

    CMainForm* pWnd = new CMainForm();
    // 老界面是「自绘无外框」窗口：WS_POPUP → 没有系统标题栏、没有可拉伸边框
    // （界面1 右下角无缩放拖柄）；最小化/关闭全部自绘（CWindowBtnUI）。
    // WS_EX_APPWINDOW 让无外框窗口仍然出现在任务栏。
    // 第二个参数是窗口标题 —— 无边框窗口（WS_POPUP + WS_EX_APPWINDOW）在任务栏
    // 与 Alt+Tab 里显示的就是它，必须是产品名而不是文件名/内部代号。
    HWND h = pWnd->Create(nullptr, Tr(L"九转还原"),
                          WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                          WS_EX_APPWINDOW, 0, 0, kWndW, kWndH);
    if (!h) {
        delete pWnd;
        ::CoUninitialize();
        return 2;
    }
    sysrecover::StartupPhase("create-window");
    // 窗口/任务栏图标（问题清单 E1）：exe 里嵌的资源（SysRecover.rc: `1 ICON`）。
    {
        HICON ib = (HICON)::LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       0, 0, LR_DEFAULTSIZE | LR_SHARED);
        HICON is = (HICON)::LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                       ::GetSystemMetrics(SM_CXSMICON),
                                       ::GetSystemMetrics(SM_CYSMICON), LR_SHARED);
        if (ib)
            ::SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)ib);
        if (is)
            ::SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)is);
    }
    // 圆角：WindowImplBase 只在 WM_SIZE 里套 SetWindowRgn（WinImplBase.cpp:211），
    // 而 XML（含 roundcorner）是在 WM_CREATE 期间才解析的，首次 WM_SIZE 可能更早，
    // 故建窗后手动再套一次（幂等，重复设置无副作用）。
    HRGN hRgn = ::CreateRoundRectRgn(0, 0, kWndW + 1, kWndH + 1, kCorner, kCorner);
    if (::SetWindowRgn(h, hRgn, TRUE) == 0)
        ::DeleteObject(hRgn);  // 成功时 region 归系统所有，不可再删

    pWnd->CenterWindow();
    pWnd->ShowWindow(true);
    sysrecover::StartupPhase("window-shown");  // 问题 2：到这里界面必须已经出来

    // ── 运行目录搬迁（用户 2026-10-03 规格）──────────────────────────────
    // 程序在光盘/U盘上：日志无法长期保存 → 询问后复制整包到本地固定分区再运行。
    // 目的地 = 装了 Windows 的分区之外的第一顺序可写固定分区；没有别的分区时
    // 文案说明"还原系统盘时程序与日志会被覆盖"。失败只提示并继续原地（产品永远能用）。
    // ⚠️ 问题 2（用户 2026-10-12）：本块与下面的 A 引擎残留询问**原来排在建窗之前**，
    // 于是光盘/U盘上的那次磁盘扫描、以及弹框等待都会挡在界面之前 → 现在一律挪到
    // `ShowWindow` 之后：先把界面给用户，再做这些"可以等"的检查与询问。
    {
        sysrecover::RelocatePlan rp = sysrecover::CheckRelocate();
        if (rp.needed) {
            std::wstring msg, left, right;
            if (!rp.destRoot.empty()) {
                msg = std::wstring(Tr(L"检测到程序在光盘或U盘上运行，")) + L"\n";
                msg += std::wstring(Tr(L"是否复制到 ")) + rp.destRoot +
                       std::wstring(Tr(L" 后运行？")) + L"\n";
                msg += rp.onlySystemDrive
                           ? Tr(L"注意：还原系统盘时程序与日志会被覆盖。")
                           : Tr(L"复制后日志与还原记录都会保存在那里。");
                left = Tr(L"留在原处");
                right = Tr(L"复制并运行");
            } else {
                msg = Tr(L"程序在只读介质上，且没有可写磁盘。\n"
                         L"本次运行的日志将无法保存。\n是否仍要继续？");
                left = Tr(L"退出");
                right = Tr(L"仍要运行");
            }
            if (CConfirmDlg::Ask2(h, Tr(L"九转还原"), msg, left, right,
                                  /*defaultIsRight=*/true) == 1) {
                if (!rp.destRoot.empty()) {
                    int code = 0;
                    std::string err;
                    if (sysrecover::DoRelocate(rp, /*wait=*/false, &code, &err)) {
                        delete pWnd;  // 窗口已建（问题 2 后本块排在 ShowWindow 之后）
                        ::CoUninitialize();
                        return 0;  // 新副本接手（单实例互斥由它继续持有）
                    }
                    std::wstring emsg =
                        Tr(L"复制失败：") + U2W8(err) +
                        Tr(L"\n将继续从当前位置运行，日志可能无法保存。");
                    ::MessageBoxW(h, emsg.c_str(), Tr(L"九转还原"),
                                  MB_OK | MB_ICONWARNING);
                }
            }
        }
    }

    sysrecover::StartupPhase("relocate-check");

    // ── A 引擎残留分流（0.7，用户 2026-10-09 规格；决策表 docs/23 §5）──────
    //   Delete → 后台删（不卡开窗）；Reuse/Redo → 只记日志（还原时接着处理）；
    //   AskUndo → 弹框询问（默认「保留不动」= 安全项；撤销 = 把旧系统搬回根）。
    {
        for (const auto& it : sysrecover::ScanSwapLeftovers()) {
            const sysrecover::SwapLeftVerdict v =
                sysrecover::DecideSwapLeftover(it);
            const std::string tagN =
                std::string(1, (char)it.drive) + ": " +
                (it.isNew ? "~new" : "~old");
            if (v.action == sysrecover::SwapLeftAction::Delete) {
                sysrecover::LogInfo("swap leftover: " + tagN + " -> delete (" +
                                    v.reason + ")");
                sysrecover::DeleteSwapLeftoverAsync(it);
            } else if (v.action == sysrecover::SwapLeftAction::AskUndo) {
                const std::wstring tagW =
                    std::wstring(1, it.drive) + L": " +
                    (it.isNew ? L"~new" : L"~old");
                if (it.keepOldMarker && !it.isNew) {
                    // 备返保留的 ~old（用户当初勾了「备返」）→ 三选：
                    // 保留（安全项，回车/ESC 都落这里）/ 返回旧系统 / 删除。
                    // 「返回旧系统」= 重启进 Linux 救援层做反向交换（PIT-139），
                    // 不在 Windows 里改名正在运行的系统树。
                    std::wstring msg3 =
                        std::wstring(Tr(L"发现备返保留的旧系统：")) + tagW + L"\n" +
                        Tr(L"保留 = 维持现状；返回旧系统 = 重启把旧系统搬回根；") +
                        L"\n" +
                        Tr(L"删除 = 永久清除旧系统，释放磁盘空间。");
                    int r = CConfirmDlg::Ask3(
                        h, Tr(L"处理备返的旧系统"), msg3,
                        Tr(L"保留不动"), Tr(L"返回旧系统"), Tr(L"删除旧系统"),
                        /*defaultIdx=*/0);
                    if (r == 1) {
                        StageReturnOldFlow(h, it.drive);
                    } else if (r == 2) {
                        sysrecover::LogInfo("swap leftover: " + tagN +
                                            " -> delete (user chose, keepold)");
                        // 同步删（不异步）：~old 常是整棵旧系统树，异步删
                        // 时用户立刻关窗/还原会看到「点了没反应」；同步删完
                        // 再给结果框，删不掉也当场说清。
                        {
                            HCURSOR hc = LoadCursor(nullptr, IDC_WAIT);
                            HCURSOR prev =
                                SetCursor(hc);
                            sysrecover::DeleteSwapLeftover(it);
                            SetCursor(prev);
                        }
                        const std::wstring stillDir =
                            std::wstring(1, it.drive) + L":\\~old";
                        const bool stillThere =
                            GetFileAttributesW(stillDir.c_str()) !=
                            INVALID_FILE_ATTRIBUTES;
                        std::wstring doneMsg =
                            stillThere
                                ? std::wstring(
                                      Tr(L"删除未完全成功，~old 仍有残留。\n"))
                                  + Tr(L"部分文件可能被占用；可稍后重试，") +
                                      L"\n" +
                                      Tr(L"或重启后再删（还原前必须清干净）。")
                                : std::wstring(Tr(L"旧系统已删除，释放磁盘空间。"));
                        MessageBoxW(h, doneMsg.c_str(),
                                    Tr(L"删除旧系统"),
                                    stillThere ? MB_ICONWARNING : MB_ICONINFORMATION);
                    } else {
                        sysrecover::LogInfo("swap leftover kept by user: " +
                                            tagN + " (keepold)");
                    }
                    continue;
                }
                // 中断残留（根已空缺，旧系统还在 ~old）→ 两选。
                std::wstring msg =
                    std::wstring(Tr(L"发现中断残留：")) + tagW + L"\n" +
                    Tr(L"返回旧系统 = 重启把旧系统搬回根目录；") + L"\n" +
                    Tr(L"保留 = 下次运行再问（还原前会被拦截）。");
                if (CConfirmDlg::Ask2(h, Tr(L"检测到还原残留"), msg,
                                      Tr(L"保留不动"), Tr(L"返回旧系统"),
                                      /*defaultIsRight=*/false) == 1) {
                    StageReturnOldFlow(h, it.drive);
                } else {
                    sysrecover::LogInfo("swap leftover kept by user: " + tagN +
                                        " (" + v.reason + ")");
                }
            } else {
                sysrecover::LogInfo("swap leftover: " + tagN + " -> " +
                                    v.reason);
            }
        }
    }

    sysrecover::StartupPhase("swap-scan");

    // 启动耗时自检（进程创建 → 窗口显示）：排障用，设 `SYSRECOVER_STARTUP_TIMING=1` 打开。
    if (sysrecover::StartupTimingEnabled())
        sysrecover::LogInfo("startup: " +
                            std::to_string(sysrecover::MsSinceProcessStart()) +
                            " ms (process -> window shown)");
    CPaintManagerUI::MessageLoop();
    delete pWnd;

    ::CoUninitialize();
    return 0;
}
