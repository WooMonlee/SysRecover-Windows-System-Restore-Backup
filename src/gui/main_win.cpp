// SysRecoverUI 入口（经典 Duilib，纯 Win32，MinGW 原生支持）。
// 注意：C++ 标准头已在 main_form.h 中先于 StdAfx.h 引入（PIT-012）。
#include "../common/i18n.h"
#include "main_form.h"

#include "../boot/grub.h"  // sysrecover::ExeDir()（启动器布局下的应用根目录）
#include "../common/crash.h"     // 崩溃处理（minidump + 可读日志）
#include "../common/logger.h"    // LogInfo（启动耗时自检）
#include "../common/process.h"   // MsSinceProcessStart
#include <commctrl.h>  // 删除进度条（msctls_progress32）
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

// ── 删除进度窗口（启动期 AskUndo 用；主窗尚未创建，状态栏不可用）──────────
// 极简 Win32 窗口：标题 + 一行文本（「正在删除旧系统… 已删 N 个文件」）。
// 删除线程 PostMessage 更新文本，完成后 PostQuitMessage 退出模态循环。
namespace {

#define WM_DEL_PROGRESS (WM_APP + 20)  // wParam=ok, lParam=fail
#define WM_DEL_DONE     (WM_APP + 21)  // wParam=stillThere (0/1)

const wchar_t* kDelProgClass = L"SysRecoverDelProg";
HFONT g_delFont = nullptr;
HWND g_delBar = nullptr;
ULONGLONG g_delStart = 0;

LRESULT CALLBACK DelProgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_delStart = GetTickCount64();
            g_delBar = CreateWindowExW(
                0, PROGRESS_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 16, 96, 388, 18, hwnd,
                (HMENU)100, GetModuleHandleW(nullptr), nullptr);
            if (g_delBar && !SendMessageW(g_delBar, PBM_SETMARQUEE, TRUE, 30))
                SetTimer(hwnd, 7, 120, nullptr);  // 无 v6 控件时退回定时器
            CreateWindowExW(0, L"BUTTON", Tr(L"在后台继续"),
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 16, 126, 170,
                            30, hwnd, (HMENU)1, GetModuleHandleW(nullptr),
                            nullptr);
            return 0;
        }
        case WM_TIMER:
            if (wp == 7 && g_delBar) {
                static int pos = 0;
                pos = (pos + 7) % 100;
                SendMessageW(g_delBar, PBM_SETPOS, pos, 0);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 1) {  // 「在后台继续」
                ShowWindow(hwnd, SW_HIDE);
                // 重新启用主窗，否则隐藏进度窗后主界面"点不动"
                HWND ow = (HWND)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (ow) EnableWindow(ow, TRUE);
                sysrecover::LogInfo(
                    "swap leftover delete UI: hidden by user (continue in "
                    "background)");
            }
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(0xf7, 0xf9, 0xfc));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0x1b, 0x24, 0x32));
            if (g_delFont) SelectObject(hdc, g_delFont);
            wchar_t buf[256];
            int len = GetWindowTextW(hwnd, buf, 256);
            RECT tr = {16, 10, rc.right - 16, 88};
            DrawTextW(hdc, buf, len, &tr, DT_LEFT | DT_WORDBREAK);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            // 2026-10-30：以前直接 return 0 → 用户点 X "完全没反应"（本次
            // 客户投诉的就是这个）。现在改成**隐藏窗口、删除转后台继续**：
            // 既能给用户反馈，又不会半途中止。
            ShowWindow(hwnd, SW_HIDE);
            {
                HWND ow = (HWND)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (ow) EnableWindow(ow, TRUE);
            }
            sysrecover::LogInfo(
                "swap leftover delete UI: user closed -> continue in background");
            return 0;
        case WM_DEL_PROGRESS: {
            wchar_t t[256];
            // 格式串含中文 → 必须 Tr()（i18n 门禁 C1）
            const unsigned long long el =
                (GetTickCount64() - g_delStart) / 1000;
            if (el >= 5) {
                // 2026-10-30 用户规格：预计要删很久时，明确告诉用户可以先去忙
                swprintf(t, 256,
                         Tr(L"正在删除旧系统…\n已删除 %ld 个文件，失败 %ld 个，已用 %llu 秒\n"
                            L"文件较多，可能要几分钟：可点「在后台继续」去做别的，删完会提示"),
                         (long)wp, (long)lp, el);
            } else {
                swprintf(t, 256,
                         Tr(L"正在删除旧系统…\n已删除 %ld 个文件，失败 %ld 个，已用 %llu 秒"),
                         (long)wp, (long)lp, el);
            }
            SetWindowTextW(hwnd, t);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DEL_DONE:
            DestroyWindow(hwnd);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// 模态运行删除：起工作线程 → 消息循环收进度 → 完成后返回「~old 是否仍在」。
// 返回 true=~old 仍在（未删完/失败），false=已删完（dir 已消失）。
// ⚠️ 两处失败路径（建窗/CreateThread 失败）必须 return true ——
// 返回 false 会被调用方当成"已删成功"，提示误导用户。

// 0.7.54 诊断：真实删除工作放在独立函数。
// 现场证据（1016-4）：日志停在 tree delete begin、无 crash dump、窗口一闪消失
// = 进程在 DeleteRec 内死亡。MinGW 不支持 MSVC 的 __try/__except →
// 改用 AddVectoredExceptionHandler：异常发生时先把异常码用纯 Win32 写进
// 主日志，再继续搜索（最终仍会被未处理过滤器/系统终止，但留了线索）。
static LONG WINAPI DelWorkerVeh(PEXCEPTION_POINTERS ep) {
    static LONG busy = 0;
    if (InterlockedExchange(&busy, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;  // 防重入（异常处理里再崩就放行）
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    // 只记录"看起来致命"的异常码，避免 C++ 内部 EH（0xE06D7363 等）噪音
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION ||
        code == EXCEPTION_IN_PAGE_ERROR) {
        char buf[96];
        int n = snprintf(buf, sizeof(buf),
                         "[VEH] delete worker exception code=0x%08lX addr=%p\r\n",
                         (unsigned long)code, addr);
        if (n > 0)
            sysrecover::LogSehNote(buf, (unsigned long)n);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void DoDeleteWork(const sysrecover::SwapLeftInfo& info, HWND hwnd) {
    // 0.7.55：C++ 异常兜底 —— std::wstring 分配失败/实现定义异常不该带走进程。
    // （访问违例是 SEH 不是 C++ 异常，由上方 VEH 捕获；这里只拦得住 C++ 层。）
    try {
        sysrecover::DeleteSwapLeftover(
            info, [hwnd](long ok, long fail, long) {
                PostMessageW(hwnd, WM_DEL_PROGRESS, (WPARAM)ok, (LPARAM)fail);
            });
    } catch (const std::exception& e) {
        sysrecover::LogWarn(std::string("swap leftover delete: exception: ") +
                            e.what());
    } catch (...) {
        sysrecover::LogWarn("swap leftover delete: unknown C++ exception");
    }
}

// 程序自删（用户 2026-10-20 规格：删除旧系统后自删）。
// 正在运行的 exe/DLL 不能当场删除 —— 起一个**分离的** cmd：等本进程退出后
// 反复 rmdir（最多 30 次），把整个程序目录（含 logs、libwim-15.dll）清掉。
// 失败只记日志（不阻塞用户；用户可手动删）。
void ScheduleProgramSelfDelete(const std::wstring& dir) {
    if (dir.size() < 4)
        return;
    std::wstring cmdline =
        L"cmd.exe /c ping -n 5 127.0.0.1 >nul & for /l %i in (1,1,30) do "
        L"@(rmdir /s /q \"" + dir + L"\" 2>nul & if not exist \"" + dir +
        L"\" exit /b)";
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                       &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        sysrecover::LogInfo("self-delete scheduled (program dir will be removed "
                            "after exit)");
    } else {
        sysrecover::LogWarn("self-delete spawn failed err=" +
                            std::to_string(GetLastError()));
    }
}

bool RunDeleteWithProgress(HWND owner, const sysrecover::SwapLeftInfo& it) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DelProgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kDelProgClass;
    RegisterClassExW(&wc);

    // 字体：微软雅黑 14
    if (!g_delFont)
        g_delFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    // 屏幕居中，420x160
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kDelProgClass,
        Tr(L"删除旧系统"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        (sw - 420) / 2, (sh - 200) / 2, 420, 200, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        sysrecover::LogWarn("swap leftover delete UI: CreateWindowExW failed err=" +
                            std::to_string(GetLastError()));
        return true;  // 失败 = ~old 仍在（不能报成"已删成功"）
    }

    // 2026-10-30（客户实测）：删除期间**禁用主窗**。进度窗是 TOPMOST，
    // 主窗若还能点，用户点「日志」会弹出一个被进度窗挡住的确认框 → 看起来
    // "按钮点不动"。把 owner 记到窗口里，隐藏进度窗时再恢复。
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)owner);
    if (owner) ::EnableWindow(owner, FALSE);
    // 初始文案
    SetWindowTextW(hwnd, Tr(L"正在删除旧系统…\n准备中…"));
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    sysrecover::LogInfo("swap leftover delete UI: progress window shown, dir=" +
                        std::string(1, (char)it.drive) +
                        (it.isNew ? ":\\~new" : ":\\~old"));

    // 工作线程：删除 + PostMessage 进度
    struct Ctx {
        sysrecover::SwapLeftInfo info;
        HWND hwnd;
    };
    auto* ctx = new Ctx{it, hwnd};
    // 0.7.55 栈加固：DeleteRec 递归删整棵 Windows 树（WinSxS/AppData 深路径），
    // 每帧 WIN32_FIND_DATAW≈592B + 两个 wstring ≈700B；默认 1MB 栈在极深树
    // 可栈溢出（EXCEPTION_STACK_OVERFLOW，VEH 0.7.54 可见 code=0xC00000FD）。
    // 提到 8MB —— 8MB/700B ≈ 12000 层，远超真实 Windows 树深度。
    HANDLE hThread = CreateThread(
        nullptr, 8 * 1024 * 1024,
        [](LPVOID p) -> DWORD {
            auto* c = static_cast<Ctx*>(p);
            // 2026-10-30：删 2.7 万文件会把磁盘打满、explorer 都卡（客户现场
            // 两个 explorer dump）。用后台模式让前台优先（Win7+ 有效）；
            // 代价是删得稍慢，但界面/桌面不再"假死"。
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            // VEH 兜底（0.7.54）：删除线程内致命异常先落一条再放行 —
            // 崩溃过滤器若没写出 dump 也留线索。线程结束时移除。
            PVOID veh = AddVectoredExceptionHandler(1, DelWorkerVeh);
            DoDeleteWork(c->info, c->hwnd);
            if (veh)
                RemoveVectoredExceptionHandler(veh);
            // 删完复核目录是否还在
            std::wstring dir =
                std::wstring(1, c->info.drive) +
                (c->info.isNew ? L":\\~new" : L":\\~old");
            bool still =
                GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
            PostMessageW(c->hwnd, WM_DEL_DONE, still ? 1 : 0, 0);
            delete c;
            return 0;
        },
        ctx, 0, nullptr);
    if (!hThread) {
        sysrecover::LogWarn("swap leftover delete UI: CreateThread failed err=" +
                            std::to_string(GetLastError()));
        DestroyWindow(hwnd);
        if (owner) ::EnableWindow(owner, TRUE);  // 别把主窗永远禁着
        return true;  // 失败 = ~old 仍在（不能报成"已删成功"）
    }
    CloseHandle(hThread);

    // 模态消息循环（直到 WM_DEL_DONE → DestroyWindow → PostQuitMessage）
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) ::EnableWindow(owner, TRUE);
    sysrecover::LogInfo("swap leftover delete UI: message loop exited");
    // 从 WM_QUIT 的 wParam 取不出 stillThere（已 destroy）→ 用目录复核
    std::wstring dir = std::wstring(1, it.drive) +
                       (it.isNew ? L":\\~new" : L":\\~old");
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

using namespace DuiLib;

namespace {
// 窗口尺寸与老界面实测一致（745x410；客户区 = 整窗，无系统外框）
const int kWndW   = 745;
const int kWndH   = 410;
const int kCorner = 24;  // 圆角直径（radius 12；Duilib 的 roundcorner/borderround
                         // 都是「椭圆宽高」= 直径，见 docs/25-PIT坑位册.md）

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
    // 临时目录兜底（PIT-192）：刚还原完的新系统可能连 %TEMP% 目录都没有，
    // 不建出来会让 wimlib / dism 的临时动作全部失败。
    sysrecover::EnsureTempDir();
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

    // （Q1「先落盘后搬家」，用户 2026-10-18：启动搬迁块已删除 —— 搬家等用户
    //  点「确定还原」后再做，避免"只想看看界面就复制一份"。SettleHome 由
    //  StageRestoreImpl 在确认后自动调用，仅还原/返回旧系统触发。）

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
                    // 首启（克隆后第一次进新系统，用户 2026-10-17 规格）：
                    // 旧日志"搬进程序目录"已在本窗体建窗期做过（main_form.cpp
                    // ::InitWindow，必须在收痕迹/清散件之前）—— 这里只负责问。
                    // 备返保留的 ~old（用户当初勾了「备返」）→ 三选：
                    // 保留（安全项，回车/ESC 都落这里）/ 返回旧系统 / 删除。
                    // 「返回旧系统」= 重启进 Linux 救援层做反向交换（PIT-139），
                    // 不在 Windows 里改名正在运行的系统树。
                    std::wstring msg3 =
                        std::wstring(Tr(L"发现备返保留的旧系统：")) + tagW + L"\n" +
                        Tr(L"保留 = 维持现状；返回旧系统 = 重启把旧系统搬回根；") +
                        L"\n" +
                        Tr(L"删除 = 永久清除旧系统，释放磁盘空间。") + L"\n" +
                        Tr(L"自删 = 删旧系统 + 启动菜单 + 本程序（不可恢复）");
                    int r = CConfirmDlg::Ask4(
                        h, Tr(L"处理备返的旧系统"), msg3,
                        Tr(L"保留不动"), Tr(L"返回旧系统"), Tr(L"删除旧系统"),
                        Tr(L"删除旧系统后自删"),
                        /*defaultIdx=*/0);
                    if (r == 1) {
                        StageReturnOldFlow(h, it.drive);
                    } else if (r == 3) {
                        // 用户 2026-10-20 规格：先删旧系统 → 删启动菜单 →
                        // 删本程序（自删）。两步确认（默认「否」）。
                        sysrecover::LogInfo("swap leftover: " + tagN +
                                            " -> delete + self-delete (user "
                                            "chose)");
                        if (MessageBoxW(
                                h,
                                Tr(L"将删除旧系统、启动菜单以及本程序，"
                                   "此操作不可恢复。\n确定继续吗？"),
                                Tr(L"删除旧系统后自删"),
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
                            IDYES)
                            continue;
                        bool stillThere = RunDeleteWithProgress(h, it);
                        if (stillThere) {
                            std::wstring m2 = Tr(
                                L"删除未完全成功，~old 仍有残留；"
                                L"为避免留下半成品，本次未删除本程序。\n"
                                L"可稍后重试，或重启后再删。");
                            if (sysrecover::LastDeleteBootCleanupScheduled())
                                m2 += Tr(L"\n\n部分文件已登记为"
                                         L"「下次开机自动清除」（重启后生效）。");
                            MessageBoxW(h, m2.c_str(), Tr(L"删除旧系统"),
                                        MB_OK | MB_ICONWARNING);
                            continue;
                        }
                        std::string blog;
                        if (!sysrecover::RemoveBootLayer(blog)) {
                            MessageBoxW(
                                h,
                                Tr(L"启动菜单清除未完全成功，已取消删除本程序。\n"
                                   "可稍后用「清除引导项」重试。"),
                                Tr(L"删除旧系统"), MB_OK | MB_ICONWARNING);
                            continue;
                        }
                        sysrecover::DeleteMenuBinding();
                        ScheduleProgramSelfDelete(sysrecover::AppDir());
                        MessageBoxW(h,
                                    Tr(L"旧系统、启动菜单与本程序已清理，"
                                       "程序即将退出。"),
                                    Tr(L"删除旧系统后自删"),
                                    MB_OK | MB_ICONINFORMATION);
                        ::DestroyWindow(h);
                        delete pWnd;
                        ::CoUninitialize();
                        return 0;
                    } else if (r == 2) {
                        sysrecover::LogInfo("swap leftover: " + tagN +
                                            " -> delete (user chose, keepold)");
                        // 删除 + 进度窗口（~old 是整棵旧系统树，可能几分钟；
                        // 主窗尚未创建，状态栏不可用 → 用临时进度窗）。
                        bool stillThere = RunDeleteWithProgress(h, it);
                        std::wstring doneMsg =
                            stillThere
                                ? std::wstring(
                                      Tr(L"删除未完全成功，~old 仍有残留。\n"))
                                  + Tr(L"部分文件可能被占用；可稍后重试，") +
                                      L"\n" +
                                      Tr(L"或重启后再删（还原前必须清干净）。") +
                                      (sysrecover::LastDeleteBootCleanupScheduled()
                                           ? Tr(L"\n\n已登记为「下次开机自动清除」"
                                                L"（重启后会自动删完）。")
                                           : std::wstring())
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

    // 0.7.55 窗口置前：启动期弹框（relocate 光盘检测 / AskUndo 残留分流 /
    // StageReturnOldFlow 内 MessageBox）是模态自绘框，关闭后主窗 Z 序仍可能在
    // explorer 文件夹窗口之后（用户反馈：确认复制后主窗"藏到文件夹后面"）。
    // 进主消息循环前强制拉到前台 —— 此时用户刚与我们的弹框交互，拥有前台权。
    ::SetForegroundWindow(h);
    ::BringWindowToTop(h);
    ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

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
