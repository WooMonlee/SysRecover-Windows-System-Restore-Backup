// 主窗口实现。Phase 5：CLI 逻辑经 app/ops 共享层桥接到 GUI。
// 注意：所有 C++ 标准头已通过 main_form.h 在 StdAfx.h 之前 include（PIT-012）。
#include "main_form.h"

#include <shobjidl.h>
#include <shellapi.h>

#include "gui/ui_skin.h"
#include "gui/confirm_dlg.h"
#include "app/ops.h"
#include "app/safety.h"
#include "boot/bcd.h"
#include "boot/grub.h"
#include "boot/uefi.h"
#include "common/logger.h"
#include "common/process.h"
#include "common/sysinfo.h"
#include "common/progress.h"
#include "disk/disk.h"
#include "wim/wim.h"

using namespace sysrecover;

namespace {

std::wstring U2W(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring ws(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &ws[0], n);
    return ws;
}

std::string W2U(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring FormatSize(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes < 1024ULL * 1024 * 1024)
        swprintf(buf, 64, L"%.0f MB", (double)bytes / (1024.0 * 1024.0));
    else
        swprintf(buf, 64, L"%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// 分区行容量列用整数 GB（老界面观感："28 / 89 GB"、"可用 61 GB"）
std::wstring FormatGb(uint64_t bytes) {
    wchar_t buf[48];
    swprintf(buf, 48, L"%.0f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::wstring TimestampName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[40];
    swprintf(buf, 40, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// 只有日期：YYYYMMDD（备份的镜像信息与默认文件名用，用户规格 2026-09-21）。
std::wstring TimestampDate() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    return buf;
}

// 未写完的备份产物路径 —— 必须与 wim.cpp::Capture 的原子写一致
// （它先写 `<目标>.tmp`，成功后 MoveFileEx 改名；中途失败自己会删 tmp）。
// 备份走的是 Capture（req.append=false），所以"半成品"永远是这一个 `.tmp`；
// 最终路径上的文件要么是旧的有效镜像、要么是刚改名完成的成品，**都不能删**。
std::wstring IncompletePath(const std::wstring& dest) {
    return dest.empty() ? std::wstring() : dest + L".tmp";
}

// 文件被 worker 持有（或删除失败）时，登记为"下次开机删除"——需要管理员权限，
// 本程序已提权。用于「终止任务并退出」时清掉当下删不掉的半成品镜像。
void RegisterPendingDelete(const std::wstring& path) {
    if (path.empty())
        return;
    ::MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

}  // namespace

// 「网站」链接（用户规格 2026-09-23）：初期指向无忧论坛的这个帖子；
// 成熟后换成我们自己的站点 + 报错上报（用网站收集日志）。**只改这一处**。
const wchar_t* kSiteUrl =
    L"https://bbs.wuyou.net/forum.php?mod=viewthread&tid=453579";

// ────────────────── 基础 ──────────────────

CMainForm::CMainForm() : m_lastProgressPost(std::chrono::steady_clock::now()) {}
CMainForm::~CMainForm() {
    if (m_worker.joinable()) {
        m_cancel = true;
        m_worker.join();
    }
}

CDuiString CMainForm::GetSkinFolder() { return _T("skin\\"); }
CDuiString CMainForm::GetSkinFile()  { return _T("main.xml"); }
LPCTSTR CMainForm::GetWindowClassName() const { return _T("SysRecoverUI"); }

void CMainForm::InitWindow() {
    std::wstring exeDir = ExeDir();
    std::wstring logsDir = exeDir + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    LogInit(logsDir);
    ProgressInit(logsDir);
    LogInfo("GUI InitWindow");
    // 支持把 .esd/.wim 直接拖进窗口（第一步的输入框）。原生 EDIT 子窗口由
    // CSkinEditUI 转投到这里（见 ui_skin.cpp::EnsureDropTarget），窗口空白处
    // 则由本窗口直接接收。
    ::DragAcceptFiles(m_hWnd, TRUE);
    // 本 exe 是 requireAdministrator（高完整性），而拖放源「资源管理器」是
    // 普通权限 → UIPI 会拦下拖放相关消息，表现为「拖进去没反应」。按微软
    // 官方做法放行这三个消息（0x0049 = WM_COPYGLOBALDATA，OLE 拖放用）。
    ::ChangeWindowMessageFilterEx(m_hWnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(m_hWnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(m_hWnd, 0x0049, MSGFLT_ALLOW, nullptr);
    m_sysDesc = sysrecover::DescribeRunningSystem();  // 备份信息/默认文件名要用
    PopulatePartitions();
    ApplyModeUi();
    RefreshBootMenuBtn();
    SetTimer(m_hWnd, 2, 500, nullptr);  // 路径输入兜底同步（见 WM_TIMER/wParam==2）
}

// ────────────────── HandleMessage（worker 线程安全回调） ──────────────────

LRESULT CMainForm::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND && HIWORD(wParam) == EN_CHANGE) {
        // 输入框内容变化 —— 原生 EDIT 的 EN_CHANGE 会送到父窗口（=主窗口，Duilib
        // 控件本身不是窗口）。用户 2026-09-23 反馈：**手动输入**镜像路径时
        // "开始备份系统"按钮一直是灰的（以前只有"浏览…"/拖入才会刷新按钮）。
        // 这里把当前文本同步进 m_wimPath 并刷新按钮；还原模式下若指向存在的文件，
        // 顺手解析子镜像（用 m_lastLoadedWim 防止每敲一个键就重解析一遍）。
        CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
        if (pEdit) {
            // 手动输入/粘贴的内容在**原生 EDIT 子窗口**里，控件的 GetText() 可能
            // 还是旧的 → 直接读子窗口（双保险，配合 CSkinEditUI::SyncTextFromNative）。
            std::wstring p;
            HWND hNative =
                static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
            if (hNative) {
                int n = ::GetWindowTextLengthW(hNative);
                if (n > 0) {
                    std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
                    ::GetWindowTextW(hNative, &buf[0], n + 1);
                    buf.resize(static_cast<size_t>(n));
                    p = buf;
                }
            }
            if (p.empty())
                p = pEdit->GetText().GetData();
            if (p != m_wimPath) {
                m_wimPath = p;
                if (!m_backupMode && !m_wimPath.empty() &&
                    m_wimPath != m_lastLoadedWim &&
                    GetFileAttributesW(m_wimPath.c_str()) !=
                        INVALID_FILE_ATTRIBUTES) {
                    m_lastLoadedWim = m_wimPath;
                    LoadWimImages(m_wimPath);
                }
                UpdateMainAction();
            }
        }
        return 0;
    }
    if (msg == WM_TIMER && wParam == 2) {
        // 兜底同步：不管路径是手打/粘贴/拖入/浏览进来的，每 500ms 回读一次原生 EDIT，
        // 有变化就更新 m_wimPath、必要时解析镜像、刷新主按钮。只靠 EN_CHANGE 不够稳
        // （焦点/时序/事件路径都可能漏，用户 2026-09-23 实测按钮仍不亮）。
        SyncImagePathFromUi();
        return 0;
    }
    if (msg == WM_TIMER && wParam == 1) {
        if (m_busy)
            SetStatus(m_lastStage + L"    已用 " + ElapsedText());
        return 0;
    }
    if (msg == WM_PROGRESS_UPDATE) {
        OnProgressUpdate((int)wParam, *reinterpret_cast<std::wstring*>(lParam));
        delete reinterpret_cast<std::wstring*>(lParam);
        return 0;
    }
    if (msg == WM_TASK_COMPLETE) {
        OnTaskComplete((int)wParam);
        return 0;
    }
    if (msg == WM_SR_QUERY_BUSY) {
        // 新实例问「你在忙吗」——忙则不让它关掉本进程（见 instance_dlg.cpp）。
        return m_busy ? 1 : 0;
    }
    if (msg == WM_DROPFILES) {
        HandleDroppedFiles(wParam);
        return 0;
    }
    if (msg == WM_DESTROY) {
        // 关键：经典 Duilib 的 WindowImplBase 销毁窗口后**不会**结束消息循环
        // （它只做 m_pm 的清理），必须自己 PostQuitMessage，否则
        // CPaintManagerUI::MessageLoop() 永不返回 → 进程变成「没有窗口但还活着」
        // 的僵尸：任务栏看不见、但单实例互斥量仍被占用，于是下次启动只会走到
        // 「已有程序在运行」流程，且等满 WaitForSingleObject 的 5s 超时才继续。
        // 修复前实测：PostMessage(WM_CLOSE) 后窗口立刻消失、进程 6s 后仍在。
        ::PostQuitMessage(0);
    }
    if (msg == WM_CLOSE && m_busy) {
        // 关闭请求来自别处（新实例的 PostMessage(WM_CLOSE)、Alt+F4、任务栏菜单等）。
        // 任务执行中直接销毁窗口会连 worker 一起杀掉，留下半成品镜像 / 中断的暂存
        // → 不能默默关，也不能只会拒绝：问一次，允许「终止并退出」。
        if (m_cancelling)
            return 0;
        if (!AskBusyClose())
            return 0;            // 「继续等待」：不关窗，任务继续
        CancelAndExit();         // 「终止并退出」：中止 + 清理 + 关窗
        return 0;
    }
    if (msg == WM_NCHITTEST) {
        // 老界面是「无系统外框」的自绘窗口：顶栏（caption 52px）空白处必须
        // 能拖动，而按钮 / Tab / 勾选框 / 下拉 / 输入框必须拿到鼠标消息。
        // 基类 OnNcHitTest 只看 caption 高度，分不清这两者，故在此自行判定：
        // 先看命中的控件是否「可交互」，是 → HTCLIENT；否则落在顶栏 → HTCAPTION。
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        ::ScreenToClient(m_hWnd, &pt);
        for (CControlUI* q = m_PaintManager.FindControl(pt); q; q = q->GetParent()) {
            LPCTSTR cls = q->GetClass();
            if (_tcscmp(cls, _T("WinBtn")) == 0 || _tcscmp(cls, _T("TabOption")) == 0 ||
                _tcscmp(cls, _T("GlyphCheck")) == 0 || _tcscmp(cls, _T("Combo")) == 0 ||
                _tcscmp(cls, _T("Edit")) == 0 || _tcscmp(cls, _T("Button")) == 0 ||
                _tcscmp(cls, _T("SkinButton")) == 0 || _tcscmp(cls, _T("SkinEdit")) == 0 ||
                _tcscmp(cls, _T("PartItem")) == 0 || _tcscmp(cls, _T("TextItem")) == 0 ||
                _tcscmp(cls, _T("ListContainerElement")) == 0)
                return HTCLIENT;
        }
        if (pt.y >= 0 && pt.y < 52) return HTCAPTION;
        return HTCLIENT;
    }
    return WindowImplBase::HandleMessage(msg, wParam, lParam);
}

// 自绘控件注入点：未知 UIClass 由 UIDlgBuilder 回调到这里（见 ui_skin.h 头部注释）
CControlUI* CMainForm::CreateControl(LPCTSTR pstrClass) {
    return CreateSkinControl(pstrClass);
}

// wimlib 阶段名 → 中文（进度条左侧显示，用户要求别出现英文 "write"）
static std::wstring StageCn(const std::wstring& s) {
    if (s.compare(0, 5, L"write") == 0) return L"写入";
    if (s.compare(0, 7, L"extract") == 0) return L"写入";
    if (s.compare(0, 4, L"scan") == 0)
        return s.size() > 5 ? L"扫描 " + s.substr(5) : L"扫描";
    return s;
}

void CMainForm::OnProgressUpdate(int pct, const std::wstring& stage) {
    SetProgress(pct);
    m_lastStage = StageCn(stage);
    SetStatus(m_lastStage + L"    已用 " + ElapsedText());
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) {
        wchar_t b[16];
        swprintf(b, 16, L"%d%%", pct);
        pPct->SetText(b);
    }
}

std::wstring CMainForm::ElapsedText() const {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now() - m_start)
                 .count();
    wchar_t b[40];
    if (s >= 3600)
        swprintf(b, 40, L"%lld:%02lld:%02lld", (long long)(s / 3600),
                 (long long)((s % 3600) / 60), (long long)(s % 60));
    else
        swprintf(b, 40, L"%02lld:%02lld", (long long)(s / 60),
                 (long long)(s % 60));
    return b;
}

void CMainForm::OnTaskComplete(int rc) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    if (m_cancel) {
        // 用户选了「终止并退出」：不要再弹任何"失败"提示（窗口马上要关掉），
        // 只留一条日志；半成品镜像由 CancelAndExit → CleanupIncompleteOutput 清。
        LogInfo("task aborted by user (cancel-and-exit)");
        return;
    }
    SetProgress(rc == 0 ? 100 : 0);
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) pPct->SetText(rc == 0 ? _T("100%") : _T(""));
    UpdateMainAction();
    if (rc == 4) {
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, U2W(last_err_).c_str(), L"安全门禁拒绝", MB_OK | MB_ICONERROR);
    } else if (rc != 0) {
        // P8：把"下一步怎么办"一起给用户（空间不足/BitLocker/坏镜像等都有对应建议）
        std::string advice = sysrecover::ErrorAdvice(rc, last_err_);
        std::wstring box = U2W(last_err_) + U2W(advice);
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, box.c_str(),
                    m_backupMode ? L"备份失败" : L"暂存失败", MB_OK | MB_ICONERROR);
    } else if (m_backupMode) {
        m_imageOk = true;  // 产物就是刚写的有效镜像 → 切到还原页主按钮即可用
        SetStatus(L"备份完成（用时 " + ElapsedText() + L"）");
        if (!IsSilent())
            MessageBoxW(m_hWnd,
                        (L"系统备份已完成。\n用时 " + ElapsedText() + L"。")
                            .c_str(),
                        L"备份成功", MB_OK | MB_ICONINFORMATION);
    } else if (m_needReboot) {
        // 需要重启（还原运行中的系统盘）：静默或用户已选「退出并重启」→ 直接重启
        SetStatus(L"暂存完成（用时 " + ElapsedText() + L"），正在重启...");
        RebootNow();
    } else {
        // 就地还原完成（PE 里 / 还原到非系统盘）：不重启，直接报完成。
        // ⚠️ 文案（用户 2026-09-23 反馈）：原来写"无需重启"，用户会理解成
        // "现在就能用还原好的系统了" ✗ —— 其实必须**重启**才能进入还原的系统
        // （我们只是没有自动重启而已）。所以明确写"重启后即可进入"。
        SetStatus(L"还原完成（用时 " + ElapsedText() + L"）");
        if (!IsSilent())
            MessageBoxW(m_hWnd,
                        (L"系统还原已完成，用时 " + ElapsedText() +
                         L"。\n重启后即可进入恢复的系统。")
                            .c_str(),
                        L"还原成功", MB_OK | MB_ICONINFORMATION);
    }
}

// ────────────────── 忙时关闭 / 终止并退出（2026-09-20 用户规格） ──────────────────
//
// 以前：任务执行中点关闭**完全没有反应**（Notify 的 m_busy 早退把点击吞了），
// 唯一出路是干等任务跑完。现在：默认「继续等待」，也可选「终止并退出」——
// 立即中止任务并删除未写完的镜像。
// 中止走既有取消链路：m_cancel=true → 进度回调返回 ABORT（wim.cpp:109）→
// wimlib 在**下一次进度回调**就收手（实测亚秒级），不会留下"写入中"的坏镜像。

bool CMainForm::AskBusyClose() {
    if (!m_busy)
        return true;
    std::wstring msg = L"任务正在执行中（已用 " + ElapsedText() + L"）。\n"
                       L"选择「继续等待」：任务照常进行，不受影响。\n"
                       L"选择「终止并退出」：立即中止任务，并删除未写完的镜像文件。";
    // 默认项 = 左按钮 =「继续等待」：回车/ESC 都落在安全项上，防误取消。
    int r = CConfirmDlg::Ask2(m_hWnd, L"任务执行中", msg, L"继续等待",
                              L"终止并退出", /*defaultIsRight=*/false);
    return r != 0;  // 0 = 继续等待
}

// BitLocker 提醒（用户规格 2026-09-23）：有加密卷时提醒"没密钥则数据无法恢复"，
// 用户可选继续/退出（默认**退出**=安全项）。返回 true = 继续。
// 静默模式跳过弹框（无人值守），但仍写一条日志。
bool CMainForm::AskBitLockerWarning(const std::vector<std::wstring>& vols) {
    if (vols.empty())
        return true;
    std::wstring list;
    for (size_t i = 0; i < vols.size(); ++i) {
        if (i)
            list += L"、";
        list += vols[i];
    }
    LogInfo("BitLocker volumes detected: " + W2U(list));
    if (IsSilent())
        return true;
    // 文案按用户 2026-09-23 的反馈断行（原第 2 行太长被右边裁掉）：
    // 在"这些卷的数据在还原后"处加逗号换行，末尾挪到下一行。
    std::wstring msg = L"检测到本机有 BitLocker 加密的卷：" + list + L"\n" +
                       L"如果你没有对应的密码 / 恢复密钥，这些卷的数据在还原后，\n" +
                       L"将无法恢复。是否继续还原？";
    return CConfirmDlg::Ask2(m_hWnd, L"BitLocker 提醒", msg, L"退出", L"继续",
                             /*defaultIsRight=*/false) == 1;
}

void CMainForm::CancelAndExit() {
    if (m_cancelling)
        return;
    m_cancelling = true;
    LogInfo("GUI: user chose cancel-and-exit while busy");
    m_cancel = true;  // → 进度回调返回 true → wimlib ABORT

    // 等 worker 收手（正常 <1s）。期间保持消息泵活着，否则窗口白屏卡住。
    ::EnableWindow(m_hWnd, FALSE);
    const auto t0 = std::chrono::steady_clock::now();
    while (m_busy) {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&m);
            ::DispatchMessageW(&m);
        }
        if (!m_busy)
            break;
        if (std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0)
                .count() >= 10)
            break;
        ::Sleep(20);
    }
    if (m_busy) {
        // 兜底：优雅取消没生效（例如卡在不可中断的 IO/驱动阶段）→ 直接杀进程。
        // 此刻临时文件仍被 worker 持有，删不掉 → 登记"下次开机删除"。
        LogInfo("GUI: worker still busy after 10s -> hard kill");
        RegisterPendingDelete(IncompletePath(m_workerDest));
        ::TerminateProcess(::GetCurrentProcess(), 0);
        return;
    }
    if (m_worker.joinable())
        m_worker.join();  // 已自然结束，立即返回
    CleanupIncompleteOutput();
    ::EnableWindow(m_hWnd, TRUE);
    Close();  // → WM_CLOSE（m_busy 已清）→ 正常关窗
}

void CMainForm::CleanupIncompleteOutput() {
    // 只删**本次正在写的临时文件**（`<目标>.tmp`，见 wim.cpp::Capture 的原子写：
    // 先写 tmp、成功才 MoveFileEx 改名）。**绝不能删最终路径** —— 那里可能是用户
    // 之前就存在的有效镜像（wimlib 还没改名，它原封不动）。
    if (m_workerDest.empty())
        return;
    std::wstring tmp = IncompletePath(m_workerDest);
    DWORD a = ::GetFileAttributesW(tmp.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return;  // 正常取消时 Capture 已经删过了，这里是兜底
    ::SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (::DeleteFileW(tmp.c_str())) {
        LogInfo("cancel: removed incomplete temp " + W2U(tmp));
    } else {
        LogError("cancel: cannot remove incomplete temp (err=" +
                 std::to_string(::GetLastError()) + "), scheduled at reboot");
        RegisterPendingDelete(tmp);
    }
}

// ────────────────── 事件路由 ──────────────────

void CMainForm::Notify(TNotifyUI& msg) {
    // 关闭按钮必须**先于** m_busy 早退处理：原来那句 `if (m_busy) return;` 把
    // 关闭点击也一起吞掉了，于是下面那段按 m_busy 分流的关闭分支成了**死代码**，
    // 表现为「任务执行中点右上角叉叉完全没反应」（用户 2026-09-20 实测吐槽）。
    if (msg.sType == DUI_MSGTYPE_CLICK &&
        msg.pSender->GetName() == CDuiString(_T("CloseBtn"))) {
        if (m_cancelling)
            return;
        if (!m_busy) {
            Close();
            return;
        }
        if (AskBusyClose())
            CancelAndExit();  // 选了「继续等待」则什么都不做（任务继续）
        return;
    }
    if (m_busy) return;  // 任务执行中忽略其他点击
    if (msg.sType == DUI_MSGTYPE_CLICK) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("BrowseBtn")) {
            if (m_backupMode) BrowseSaveFile(); else BrowseWimFile();
        } else if (name == _T("MainAction")) {
            if (m_backupMode) StartBackup(); else StartRestore();
        } else if (name == _T("RepairBootBtn")) {
            // 「清除引导项」：删 BCD 条目 + 清 bootsequence + 删数据盘上的
            // grldr/grldr.mbr/menu.lst/ZJRESTORE（用户要求：要清就清干净）
            std::string log;
            bool ok = RemoveBootLayer(log);
            SetStatus(ok ? L"已清除引导项与相关文件" : L"清除引导项未完全成功");
            LogInfo(std::string("GUI remove boot layer: ") + log);
        } else if (name == _T("BootMenuBtn")) {
            ToggleBootMenu();
        } else if (name == _T("SiteLink")) {
            // 右下角「网站」链接 → 用系统默认浏览器打开。
            // （2026-09-23 曾临时改成弹 BitLocker 演示框，用户确认文案后已改回。）
            ::ShellExecuteW(nullptr, L"open", kSiteUrl, nullptr, nullptr,
                            SW_SHOWNORMAL);
        } else if (name == _T("MinBtn")) {
            SendMessage(WM_SYSCOMMAND, SC_MINIMIZE, 0);
        }
    } else if (msg.sType == DUI_MSGTYPE_SELECTCHANGED) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("ModeBackup") || name == _T("ModeRestore")) {
            // 同组 Option 无论「被选中」还是「被取消选中」都会发 SELECTCHANGED，
            // 且两者顺序不保证（实测：先发新选中项、再发旧项的取消 → 旧项会把
            // 界面又刷回上一个模式，表现为「Tab 已经切了、内容却没变」）。
            // 所以只认「当前确实处于选中态」的那一次通知。
            COptionUI* pOpt = static_cast<COptionUI*>(msg.pSender);
            if (pOpt && pOpt->IsSelected()) {
                bool bak = (name == _T("ModeBackup"));
                if (bak != m_backupMode) { m_backupMode = bak; ApplyModeUi(); }
            }
        } else if (name == _T("FormatBox")) { /* 默认文件名不重刷 */ }
    } else if (msg.sType == DUI_MSGTYPE_ITEMSELECT) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("PartitionBox")) {
            m_selPart = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            UpdateMainAction();
        } else if (name == _T("ImageIndexBox")) {
            int sel = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            if (sel >= 0 && sel < (int)m_imgIdx.size())
                m_selImageIndex = m_imgIdx[sel];
        }
    }
    WindowImplBase::Notify(msg);
}

// ────────────────── 启动还原（常驻引导模块） ──────────────────
// BIOS：GRUB4DOS(grldr/grldr.mbr/menu.lst) + BCD 实模式启动扇区条目；
// UEFI：**固件启动项**（NVRAM Boot#### + BootOrder），由固件直接加载内核。
// 装好后即使 Windows 蓝屏/引导损坏，也能从开机启动菜单进救援。

static bool BootMenuInstalled(std::string& detail) {
    if (IsUefiFirmware())
        return UefiBootEntryExists(detail);
    return !NeedsInstall(detail);  // NeedsInstall=true 表示还没装
}

void CMainForm::RefreshBootMenuBtn() {
    std::string detail;
    bool on = BootMenuInstalled(detail);
    CControlUI* p = m_PaintManager.FindControl(_T("BootMenuBtn"));
    if (p) {
        p->SetText(on ? _T("删除启动还原") : _T("安装启动还原"));
        p->Invalidate();
    }
}

void CMainForm::ToggleBootMenu() {
    std::string detail, log;
    if (BootMenuInstalled(detail)) {
        bool ok = RemoveBootLayer(log);
        SetStatus(ok ? L"已删除启动还原" : L"删除启动还原未完全成功");
    } else if (IsUefiFirmware()) {
        if (IsSecureBootEnabled() && !IsMokEnrolled(ExeDir())) {
            MessageBoxW(m_hWnd,
                        L"本机开启了 Secure Boot（安全启动）。\n\n"
                        L"我们的救援内核没有微软签名，需要借开源的 shim 引导链：\n"
                        L"装好后请**重启一次**，会出现蓝底的 MokManager 界面，\n"
                        L"选择 Enroll key（或 Enroll key from disk），\n"
                        L"选中 ZJRESTORE 里的 zj-mok.cer，确认并重启。\n\n"
                        L"这一次性注册完成后，以后每次还原都能正常进救援。",
                        L"知鉴一键还原", MB_OK | MB_ICONINFORMATION);
        }
        PartitionInfo esp;
        if (!FindEspPartition(esp)) {
            MessageBoxW(m_hWnd, L"未找到 ESP 分区，无法安装启动还原。",
                        L"知鉴一键还原", MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring espRoot = MountEsp(log);
        if (espRoot.empty()) {
            MessageBoxW(m_hWnd, L"无法给 ESP 分区分配盘符（mountvol X: /s 失败）。",
                        L"知鉴一键还原", MB_OK | MB_ICONERROR);
            LogInfo(std::string("GUI install boot menu: ") + log);
            return;
        }
        bool ok = InstallUefiBootEntry(espRoot, ExeDir(), log);
        UnmountEsp(espRoot, log);
        SetStatus(ok ? L"启动还原已安装（开机启动菜单可选）"
                     : L"安装启动还原失败");
    } else {
        bool ok = InstallBootLayer(SystemDrive(), ExeDir(), log);
        SetStatus(ok ? L"启动还原已安装" : L"安装启动还原失败");
    }
    LogInfo(std::string("GUI boot menu toggle: ") + log);
    RefreshBootMenuBtn();
}

// ────────────────── 分区枚举 ──────────────────

void CMainForm::PopulatePartitions() {
    CComboUI* pCombo =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("PartitionBox")));
    if (!pCombo) return;

    pCombo->RemoveAll();
    m_parts.clear();
    m_selPart = -1;
    int selDefault = -1;
    for (const auto& disk : EnumerateDisks()) {
        for (const auto& part : disk.parts) {
            if (part.isEsp || part.isMsr || part.isRecovery || disk.isRemovable)
                continue;
            if (part.sizeBytes < 2ULL * 1024 * 1024 * 1024)
                continue;
            m_parts.push_back(part);
            // 分区行四列自绘（对齐老项目 PartitionRow.cs，见 ui_skin.cpp::PaintRow）
            CPartItemUI* item = new CPartItemUI;
            item->SetFixedHeight(43);
            wchar_t partNo[32];
            swprintf(partNo, 32, L"%u/%u", disk.index, part.partNumber);
            uint64_t used = part.sizeBytes > part.freeBytes
                            ? part.sizeBytes - part.freeBytes : 0;
            std::wstring cap  = FormatGb(used) + L" / " + FormatGb(part.sizeBytes);
            std::wstring freeTxt = part.freeBytes > 0
                                   ? (L"可用 " + FormatGb(part.freeBytes))
                                   : L"系统保留";
            int pct = part.sizeBytes
                      ? (int)(used * 100 / part.sizeBytes) : 0;
            item->SetPart(part.letter.empty() ? L"—" : part.letter.c_str(),
                          U2W(StyleName(disk.style)).c_str(),
                          part.fs.c_str(), part.isSystem,
                          disk.model.empty() ? L"本地磁盘" : disk.model.c_str(),
                          part.label.c_str(), partNo,
                          cap.c_str(), freeTxt.c_str(), pct);
            pCombo->Add(item);
            if (part.isSystem && selDefault < 0)
                selDefault = (int)m_parts.size() - 1;
        }
    }
    if (pCombo->GetCount() > 0)
        pCombo->SelectItem(selDefault >= 0 ? selDefault : 0);
    m_selPart = selDefault >= 0 ? selDefault : (pCombo->GetCount() > 0 ? 0 : -1);
    CControlUI* pLoad = m_PaintManager.FindControl(_T("PartLoadingText"));
    if (pLoad) pLoad->SetVisible(false);
    UpdateMainAction();
}

// ────────────────── 文件对话框 ──────────────────

void CMainForm::BrowseWimFile() {
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) { SetStatus(L"文件对话框创建失败"); return; }
    COMDLG_FILTERSPEC filters[] = {
        {L"WIM/ESD 镜像", L"*.wim;*.esd"},
        {L"所有文件", L"*.*"},
    };
    pDlg->SetFileTypes(2, filters);
    pDlg->SetTitle(L"选择系统镜像文件");
    if (SUCCEEDED(pDlg->Show(m_hWnd))) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDlg->GetResult(&pItem))) {
            LPWSTR psz = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
            if (psz) {
                m_wimPath = psz;
                CoTaskMemFree(psz);
                CEditUI* pEdit =
                    static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
                if (pEdit) pEdit->SetText(m_wimPath.c_str());
                LoadWimImages(m_wimPath);
            }
            pItem->Release();
        }
    }
    pDlg->Release();
}

void CMainForm::BrowseSaveFile() {
    IFileSaveDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) { SetStatus(L"文件对话框创建失败"); return; }
    COMDLG_FILTERSPEC filters[] = {
        {L"WIM 镜像（标准压比，较快）", L"*.wim"},
        {L"ESD 镜像（高压比省空间）", L"*.esd"},
    };
    pDlg->SetFileTypes(2, filters);
    pDlg->SetTitle(L"选择备份保存位置");
    int fmtIdx = BackupFmt();
    pDlg->SetFileTypeIndex(fmtIdx == 0 ? 2 : 1);  // COMDLG 1-based
    const wchar_t* ext = fmtIdx == 0 ? L".esd" : L".wim";
    // 默认文件名（用户规格 2026-09-21）：`20260921Win10.19044备份.esd`
    std::wstring defName = TimestampDate() +
                           (m_sysDesc.shortTag.empty() ? TimestampName()
                                                       : m_sysDesc.shortTag) +
                           L"备份" + ext;
    pDlg->SetFileName(defName.c_str());
    if (SUCCEEDED(pDlg->Show(m_hWnd))) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDlg->GetResult(&pItem))) {
            LPWSTR psz = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
            if (psz) {
                m_wimPath = psz;
                CoTaskMemFree(psz);
                CEditUI* pEdit =
                    static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
                if (pEdit) pEdit->SetText(m_wimPath.c_str());
                SetStatus(L"备份保存至：" + m_wimPath);
            }
            pItem->Release();
        }
    }
    pDlg->Release();
    UpdateMainAction();
}

// WM_DROPFILES：把拖进来的 .esd/.wim 填到第一步输入框并立即解析子镜像。
void CMainForm::HandleDroppedFiles(WPARAM wParam) {
    if (m_busy) return;
    HDROP hDrop = reinterpret_cast<HDROP>(wParam);
    UINT n = ::DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    if (n == 0) { ::DragFinish(hDrop); return; }
    std::wstring dropped;
    {
        UINT len = ::DragQueryFileW(hDrop, 0, nullptr, 0);  // 长度不含结尾 0
        if (len > 0) {
            std::vector<wchar_t> buf((size_t)len + 1, 0);
            ::DragQueryFileW(hDrop, 0, buf.data(), len + 1);
            dropped = buf.data();
        }
    }
    ::DragFinish(hDrop);
    if (dropped.empty()) return;
    // 只认镜像后缀：拖错文件时给明确提示，不静默失败
    std::wstring ext =
        dropped.size() >= 4 ? dropped.substr(dropped.size() - 4) : L"";
    for (auto& c : ext) c = (wchar_t)towlower(c);
    if (ext != L".esd" && ext != L".wim") {
        SetStatus(L"只支持拖入 .esd / .wim 镜像文件");
        return;
    }
    m_wimPath = dropped;
    CEditUI* pEdit =
        static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
    if (pEdit) pEdit->SetText(m_wimPath.c_str());
    if (m_backupMode) {
        SetStatus(L"备份保存至：" + m_wimPath);
        UpdateMainAction();
    } else {
        LoadWimImages(m_wimPath);  // 内部设 m_imageOk 并刷新主按钮
        SetStatus(L"已载入镜像：" + m_wimPath);
    }
}

void CMainForm::LoadWimImages(const std::wstring& path) {
    m_imageOk = false;
    CComboUI* pCombo =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("ImageIndexBox")));
    if (!pCombo) { UpdateMainAction(); return; }
    pCombo->RemoveAll();
    m_imgIdx.clear();
    WimEngine wim;
    if (!wim.ok()) {
        SetStatus(L"wimlib 初始化失败");
        UpdateMainAction();
        return;
    }
    std::vector<ImageDesc> images;
    int rc = wim.ListImages(path, images);
    if (rc != 0 || images.empty()) {
        // 关键：读取失败（例如上次备份中途退出留下"写入未完成"的坏镜像）
        // 时必须保持 m_imageOk=false 并刷新按钮，否则主按钮的状态会停在
        // 上一次的可用态（用户看到"灰色按钮切一下模式又能点了"的怪象）。
        SetStatus(std::wstring(L"镜像不可用（可能上次备份未完成，请重新备份）: ") +
                  WimEngine::ErrorString(rc));
        UpdateMainAction();
        return;
    }
    for (const auto& img : images) {
        // 纯文字条目：必须走 CTextItemUI（重写了 DrawItemText，否则 Combo
        // 收起框空白，见 ui_skin.h 注释）
        CTextItemUI* item = new CTextItemUI;
        item->SetFixedHeight(30);
        wchar_t buf[512];
        // 注意：MinGW 下 swprintf 的 %s 当**窄**字符串用（PIT-007）—— 传 wchar_t*
        // 会在第一个字符的高字节 0x00 处截断（症状：镜像名只剩首字母 "1 - W"）。
        // 宽字符串必须用 %ls。
        swprintf(buf, 512, L"%d - %ls（%.1f GB）", img.index, img.name.c_str(),
                 img.sizeBytes / 1073741824.0);
        item->SetText(buf);
        pCombo->Add(item);
        m_imgIdx.push_back(img.index);
    }
    if (pCombo->GetCount() > 0) pCombo->SelectItem(0);
    m_selImageIndex = m_imgIdx.empty() ? 1 : m_imgIdx[0];
    m_imageOk = true;
    UpdateMainAction();
}

// ────────────────── 备份格式 combo 辅助 ──────────────────

int CMainForm::BackupFmt() const {
    CComboUI* pFmt =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("FormatBox")));
    int fmt = pFmt ? pFmt->GetCurSel() : 1;
    return (fmt < 0 || fmt > 2) ? 1 : fmt;
}

// ────────────────── 还原（UI 校验 + worker 线程暂存） ──────────────────

void CMainForm::StartRestore() {
    if (m_wimPath.empty()) { SetStatus(L"请先选择镜像文件"); return; }
    if (m_selPart < 0 || m_selPart >= (int)m_parts.size()) {
        SetStatus(L"请选择目标分区"); return;
    }
    const PartitionInfo& part = m_parts[m_selPart];
    // BitLocker 提醒（用户规格 2026-09-23）：**只要系统里有加密卷**就提醒。
    if (!AskBitLockerWarning(sysrecover::BitLockerVolumes()))
        return;
    // 非静默模式：只弹一个选择框（退出 / 退出并重启）——选定后暂存并自动重启，
    // 不再有额外的成功提示框，也不弹关机通知。静默模式：不弹框，直接暂存并重启。
    if (!IsSilent()) {
        // 提示文案必须与**真实行为**一致（PIT-072）：PE 里 / 还原到非系统盘走的是
        // "就地还原、不重启"，以前这里一律写"重启后还原"，用户 2026-09-21 在 PE 里
        // 实测被误导。判断复用 ops 层同一个函数（CanRestoreInPlace），两边不漂移。
        std::string why;
        bool needReboot = !CanRestoreInPlace(part, why);
        wchar_t confirm[640];
        if (needReboot) {
            // 把**判定原因**也显示出来（用户 2026-09-23：PE 里系统盘是 X:，还原 C:
            // 本不该重启 → 需要一眼看出到底卡在哪个条件）。文案保持短，避免被
            // 确认框右侧裁掉（PIT-074 的教训）。
            std::wstring shortWhy =
                why.find("系统盘") != std::string::npos
                    ? L"目标是正在运行的系统盘"
                    : L"目标分区当前被占用";
            swprintf(confirm, 640,
                     L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                     L"该分区上的所有数据将被覆盖！\n"
                     L"原因：%ls；将暂存任务并在重启后执行。",
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber, shortWhy.c_str());
            if (CConfirmDlg::Ask2(m_hWnd, L"确认还原", confirm, L"退出",
                                  L"退出并重启", /*defaultIsRight=*/true) != 1)
                return;
        } else {
            swprintf(confirm, 640,
                     L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                     L"该分区上的所有数据将被覆盖！\n"
                     L"目标分区当前未被占用，将立即就地还原，不需要重启。",
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber);
            if (CConfirmDlg::Ask2(m_hWnd, L"确认还原", confirm, L"取消",
                                  L"开始还原", /*defaultIsRight=*/true) != 1)
                return;
        }
    }
    // 软件在目标盘（会被格式化）或只读介质上 → 日志要回退到数据盘，先让用户确认
    {
        std::wstring ed = ExeDir();
        wchar_t exL = ed.empty() ? 0 : ed[0];
        wchar_t tgL = part.letter.empty() ? 0 : part.letter[0];
        wchar_t exRoot[4] = {exL, L':', L'\\', 0};
        bool onTarget = exL && tgL && towupper(exL) == towupper(tgL);
        bool fixedDisk = exL && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if ((onTarget || !fixedDisk) && !IsSilent()) {
            // 用户 2026-09-23 反馈：只说"数据盘"不知道是哪个盘 → 把盘符带上
            std::wstring dataDrive = FindDataDrive();
            std::wstring where = dataDrive.empty()
                                     ? std::wstring(L"数据盘的 ZJRESTORE 目录")
                                     : (dataDrive.substr(0, 1) + L": 盘的 ZJRESTORE 目录");
            if (MessageBoxW(m_hWnd,
                    (L"程序所在目录在要还原的系统盘上（或只读盘），\n"
                     L"本次还原的日志将改放到 " + where + L"。\n是否继续？")
                        .c_str(),
                    L"提示", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
                return;
        }
    }
    m_busy = true;
    UpdateMainAction();
    SetStatus(L"正在校验镜像并暂存还原任务...");
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::StartRestoreAsync, this);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        SetStatus(L"线程创建失败");
        MessageBoxW(m_hWnd, L"无法创建工作线程，请重试", L"错误", MB_OK | MB_ICONERROR);
    }
}

void CMainForm::StartRestoreAsync() {
    // 让出 CPU 给界面：任务吃满所有核时界面会明显发飘（"点了半天没反应"）。
    // 降到 BELOW_NORMAL 后交互明显跟手，任务耗时几乎不变（只让出空闲时间片）。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    RestoreRequest req;
    req.image = m_wimPath;
    req.disk  = m_selPart >= 0 && m_selPart < (int)m_parts.size()
                ? (int)m_parts[m_selPart].diskIndex : 0;
    req.part  = m_selPart >= 0 && m_selPart < (int)m_parts.size()
                ? (int)m_parts[m_selPart].partNumber : 1;
    req.index = m_selImageIndex;
    req.repairBoot = true;
    std::string err;
    bool needReboot = true;
    int rc = StageRestore(req, err, &needReboot);
    last_err_ = err;
    m_needReboot = needReboot;
    PostMessage(WM_TASK_COMPLETE, (WPARAM)rc, 0);
}

// ────────────────── 备份（UI 校验 + worker 线程热备） ──────────────────

void CMainForm::StartBackup() {
    int fmt = BackupFmt();
    const char* compress = fmt == 0 ? "recovery" : (fmt == 1 ? "maximum" : "fast");
    // 目标路径
    std::wstring dest = m_wimPath;
    if (dest.empty()) {
        std::wstring dataDrive = FindDataDrive();
        if (dataDrive.empty()) {
            SetStatus(L"未找到数据盘，请用浏览指定保存位置"); return;
        }
        std::wstring dir = dataDrive + kRecoveryDir;
        CreateDirectoryW(dir.c_str(), nullptr);
        dest = dir + L"\\" + TimestampDate() +
               (m_sysDesc.shortTag.empty() ? TimestampName()
                                           : m_sysDesc.shortTag) +
               L"备份" +
               (fmt == 0 ? L".esd" : L".wim");
    }
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (MessageBoxW(m_hWnd, L"目标镜像文件已存在，覆盖？", L"确认备份",
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }
    // 子镜像名
    std::wstring name = L"Backup";
    CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
    if (pNote) {
        CDuiString txt = pNote->GetText();
        if (!txt.IsEmpty()) name = txt.GetData();
    }
    m_busy = true;
    UpdateMainAction();
    SetStatus(L"正在备份...");
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo(std::string("GUI backup start: ") + W2U(dest));    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    // 源路径：使用选中分区的盘符（备份模式下用户已选分区）
    std::wstring source = L"C:/";
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        const auto& part = m_parts[m_selPart];
        if (!part.letter.empty()) {
            source = part.letter + L":/";
        }
    }
    m_workerSource   = source;
    m_workerDest    = dest;
    m_workerCompress = compress;
    m_workerName    = name;
    m_lastProgressPost = std::chrono::steady_clock::now();
    try {
        m_worker = std::thread(&CMainForm::StartBackupAsync, this);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        SetStatus(L"线程创建失败");
        MessageBoxW(m_hWnd, L"无法创建工作线程，请重试", L"错误", MB_OK | MB_ICONERROR);
    }
}

void CMainForm::StartBackupAsync() {
    // 同 StartRestoreAsync：备份线程降到 BELOW_NORMAL，保证界面点击/弹框跟手。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    BackupRequest req;
    req.source  = m_workerSource;
    req.dest    = m_workerDest;
    req.compress = m_workerCompress;
    req.name    = m_workerName;
    req.append  = false;
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;  // 取消信号
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressPost).count();
        if (elapsed < kProgressThrottleMs && pct < 100) return false;  // 节流：100ms 内不重复发（完成时 100% 必发）
        m_lastProgressPost = now;
        std::wstring* pStage = new std::wstring(U2W(stage));
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct, (LPARAM)pStage);
        return false;
    };
    std::string err;
    int rc = RunBackup(req, progressFn, err);
    last_err_ = err;
    PostMessage(WM_TASK_COMPLETE, (WPARAM)rc, 0);
}

// ────────────────── 模式切换 / 可用态 ──────────────────

void CMainForm::ApplyModeUi() {
    auto Vis = [this](const TCHAR* name, bool vis) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetVisible(vis);
    };
    auto Text = [this](const TCHAR* name, const TCHAR* t) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetText(t);
    };
    auto Pos = [this](const TCHAR* name, int l, int t, int r, int b) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) { RECT rc = { l, t, r, b }; p->SetPos(rc); }
    };
    if (m_backupMode) {
        Text(_T("BrowseBtn"),   _T("浏览保存位置…"));
        Text(_T("MainAction"),  _T("开始备份系统"));
        Text(_T("NoteLabel"),   _T("备份备注："));
        Vis(_T("ImageIndexBox"), false);
        Vis(_T("NoteText"),     false);
        Vis(_T("NoteInput"),    true);
        Vis(_T("FormatLabel"),  true);
        Vis(_T("FormatBox"),    true);
        // FormatBox 的条目是 XML 里静态写的，首次显示前没有任何选中项 →
        // 收起框空白（老界面里显示「.esd（高压比省空间）」）。仅在未选中时补默认值，
        // 这样用户在两个模式间来回切换时不会丢失已选格式。
        {
            CComboUI* pFmt = static_cast<CComboUI*>(
                m_PaintManager.FindControl(_T("FormatBox")));
            if (pFmt && pFmt->GetCurSel() < 0 && pFmt->GetCount() > 0)
                pFmt->SelectItem(0);
        }
        // 镜像信息（用户规格 2026-09-21）：在文件名下方给出「日期 + 系统类型 + 备份」，
        // 例如 `20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`。
        // 只在用户尚未填写时预填，不覆盖手输内容；这个值同时会成为 WIM 里的子镜像名。
        {
            CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
            if (pNote && pNote->GetText().IsEmpty() && !m_sysDesc.full.empty()) {
                std::wstring info =
                    TimestampDate() + L" " + m_sysDesc.full + L" 备份";
                pNote->SetText(info.c_str());
            }
        }
        Vis(_T("RepairBootBtn"),true);
        Vis(_T("BootMenuBtn"),  true);
        Text(_T("PartTitle"),   _T("备份源分区"));
        Text(_T("PartHint"),    _T("选择要备份为镜像的源分区（移动盘已隐藏）"));
        // 第三步右侧：备份模式 = 静默模式 + 「格式：」+ 下拉；引导按钮两模式共用
        // 同一位置（XML 定）。整窗收窄到 745 后，右侧一组整体左移：
        //   静默 248..340 / 格式：348..396 / 下拉 402..524 / 清除 530..612 / 菜单 618..706
        Pos(_T("Silent"),        248, 302, 340, 326);
        Pos(_T("FormatLabel"),   350, 302, 388, 326);
        Pos(_T("FormatBox"),     388, 294, 510, 332);
        // 注：状态栏固定「执行进度」（老界面如此），模式名不写进状态栏；
        // 进度文字仍由 SetStatus 在任务执行中写入。
    } else {
        Text(_T("BrowseBtn"),   _T("浏览系统镜像文件"));
        Text(_T("MainAction"),  _T("开始恢复系统"));
        Text(_T("NoteLabel"),   _T("镜像说明："));
        Vis(_T("ImageIndexBox"), true);
        Vis(_T("NoteText"),     true);
        Vis(_T("NoteInput"),    false);
        Vis(_T("FormatLabel"),  false);
        Vis(_T("FormatBox"),    false);
        Vis(_T("RepairBootBtn"),true);
        Vis(_T("BootMenuBtn"),  true);
        Text(_T("PartTitle"),   _T("系统安装位置"));
        Text(_T("PartHint"),    _T("选择要安装恢复镜像的目标分区（移动盘已隐藏）"));
        // 还原模式几何（切回时必须复位第三步右侧那三个；与 XML 默认一致）
        Pos(_T("Silent"),        420, 302, 520, 326);
        Pos(_T("FormatLabel"),   350, 302, 388, 326);
        Pos(_T("FormatBox"),     388, 294, 510, 332);
    }
    UpdateMainAction();
}

void CMainForm::UpdateMainAction() {
    CButtonUI* p =
        static_cast<CButtonUI*>(m_PaintManager.FindControl(_T("MainAction")));
    if (!p) return;
    // 备份模式同样要求「已选保存位置 + 已选源分区」才转蓝；还原模式还要求
    // 镜像确实可读（m_imageOk），否则主按钮保持灰色（避免用坏镜像去还原）。
    bool ok = !m_wimPath.empty() && m_selPart >= 0 && !m_busy &&
              (m_backupMode || m_imageOk);
    p->SetEnabled(ok);
    // 经典 Duilib 的 CButtonUI 没有状态图时 DoPaint 继承自 CControlUI，
    // 只会画 bkcolor/border → 可用态必须在代码里换底色和文字色（老界面：
    // 可用 = 蓝底白字 #8CA0DE，禁用 = 灰底灰字 #E2E6EE/#9AA3B2）
    p->SetBkColor(ok ? ui_skin::C_PRIMARY : ui_skin::C_DISABLED);
    p->SetTextColor(ok ? 0xFFFFFFFF : ui_skin::C_DIS_TEXT);
    p->Invalidate();
    // 按钮为什么是灰的？在状态栏说清楚（用户 2026-09-23：手输路径后按钮不亮，
    // 完全无从判断缺哪一步）。只在空闲且确实不可用时提示，避免覆盖任务中的状态。
    if (!ok && !m_busy) {
        if (m_wimPath.empty())
            SetStatus(m_backupMode ? L"请先选择保存位置（或手动输入 / 拖入）"
                                   : L"请先选择镜像文件（或手动输入 / 拖入）");
        else if (m_selPart < 0)
            SetStatus(m_backupMode ? L"请选择第二步的源分区"
                                   : L"请选择第二步的目标分区");
        else if (!m_backupMode && !m_imageOk)
            SetStatus(L"镜像不可用（解析失败或文件不存在）");
    }
}

// ────────────────── UI 辅助 ──────────────────

// 从界面回读镜像路径（原生 EDIT 才是"真身"，见 PIT-077），同步状态并刷新按钮。
void CMainForm::SyncImagePathFromUi() {
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (!pEdit)
        return;
    std::wstring p;
    HWND hNative = static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
    if (hNative) {
        int n = ::GetWindowTextLengthW(hNative);
        if (n > 0) {
            std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
            ::GetWindowTextW(hNative, &buf[0], n + 1);
            buf.resize(static_cast<size_t>(n));
            p = buf;
        }
    }
    if (p.empty())
        p = pEdit->GetText().GetData();
    if (p == m_wimPath)
        return;
    m_wimPath = p;
    if (!m_backupMode && !m_wimPath.empty() && m_wimPath != m_lastLoadedWim &&
        GetFileAttributesW(m_wimPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        m_lastLoadedWim = m_wimPath;
        LoadWimImages(m_wimPath);
    }
    UpdateMainAction();
}

void CMainForm::RebootNow() {
    // 不用 shutdown.exe：带 /t 时它会弹「Windows 将在一分钟后关闭」对话框
    // （用户要求关机前不要再有任何提示）。改用 ExitWindowsEx 直接触发重启，
    // 需先启用 SE_SHUTDOWN_NAME 特权（管理员进程本就有）。
    HANDLE hToken = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        if (::LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME,
                                    &tp.Privileges[0].Luid)) {
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            ::AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
        ::CloseHandle(hToken);
    }
    if (!::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                         SHTDN_REASON_MAJOR_APPLICATION |
                             SHTDN_REASON_MINOR_MAINTENANCE |
                             SHTDN_REASON_FLAG_PLANNED)) {
        // 兜底：ExitWindowsEx 因会话/策略失败时，用 /t 0 立即重启
        // （无延迟则 shutdown.exe 不弹「一分钟后关闭」对话框）
        std::string out;
        RunProcess(SysToolPath(L"shutdown.exe"), L"/r /t 0", out);
    }
}

bool CMainForm::IsSilent() {
    COptionUI* p =
        static_cast<COptionUI*>(m_PaintManager.FindControl(_T("Silent")));
    return p && p->IsSelected();
}

void CMainForm::SetStatus(const std::wstring& text) {
    // StatusText 是自绘的 CSkinLabelUI（不是 CLabelUI）
    CControlUI* p = m_PaintManager.FindControl(_T("StatusText"));
    if (p) p->SetText(text.c_str());
    LogInfo(std::string("GUI status: ") + W2U(text));
}

void CMainForm::SetProgress(int pct) {
    CRoundProgressUI* p =
        static_cast<CRoundProgressUI*>(m_PaintManager.FindControl(_T("Progress")));
    if (p) p->SetValue(pct);
    // 百分比文字一起更新 —— 用户 2026-09-23 反馈："跑的过程中少了百分比" ✓。
    // 以前只有 OnTaskComplete 里写 PercentText（所以完成时 100% 正常、中途是空的），
    // 而且上一轮的 100% 会残留下来（看着像进度条旁边一个奇怪的字符）。
    // 0 → 清空，任务一开始就把旧值抹掉。
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct)
        pPct->SetText(pct > 0 ? (std::to_wstring(pct) + L"%").c_str() : L"");
}
