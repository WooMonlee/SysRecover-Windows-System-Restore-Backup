// SysRecoverUI 入口（经典 Duilib，纯 Win32，MinGW 原生支持）。
// 注意：C++ 标准头已在 main_form.h 中先于 StdAfx.h 引入（PIT-012）。
#include "main_form.h"

#include "../boot/grub.h"  // sysrecover::ExeDir()（启动器布局下的应用根目录）
#include "../common/crash.h"     // 崩溃处理（minidump + 可读日志）
#include "../common/logger.h"    // LogInfo（启动耗时自检）
#include "../common/process.h"   // MsSinceProcessStart
#include "../common/selfarch.h"  // 位数自举：32 位程序在 64 位系统上换成 x64\同名
#include "instance_dlg.h"

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
}  // namespace

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 位数自举：32 位 GUI 在 64 位 Windows 上换成 <ExeDir>\x64\SysRecoverUI.exe 再跑
    {
        int reexecCode = 0;
        if (sysrecover::ReexecX64IfNeeded(&reexecCode, /*wait=*/false))
            return reexecCode;
    }
    // 崩溃处理：装在最前面（真实进程），崩溃时把 dump+文本写到 <exeDir>\logs\crash\。
    sysrecover::InstallCrashHandler(sysrecover::ExeDir());
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
                              L"之前的程序正在执行任务，无法关闭。\n"
                              L"已为你切回该窗口，请等任务结束后再重新打开。",
                              L"九转还原", MB_OK | MB_ICONINFORMATION);
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
    HWND h = pWnd->Create(nullptr, _T("九转还原"),
                          WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                          WS_EX_APPWINDOW, 0, 0, kWndW, kWndH);
    if (!h) {
        delete pWnd;
        ::CoUninitialize();
        return 2;
    }
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
