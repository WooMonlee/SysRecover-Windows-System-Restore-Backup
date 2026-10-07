// 主窗口实现。Phase 5：CLI 逻辑经 app/ops 共享层桥接到 GUI。
// 注意：所有 C++ 标准头已通过 main_form.h 在 StdAfx.h 之前 include（PIT-012）。
#include "common/i18n.h"
#include "main_form.h"

#include <shobjidl.h>
#include <shellapi.h>

#include "gui/ui_skin.h"
#include "gui/confirm_dlg.h"
#include "gui/imgsearch.h"
#include "app/ops.h"
#include "app/safety.h"
#include "app/selfdiag.h"
#include "boot/bcd.h"
#include "boot/grub.h"
#include "boot/uefi.h"
#include "common/cpucap.h"
#include "common/logger.h"
#include "common/process.h"
#include "common/relocate.h"
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

// 路径是否"像镜像文件"（以 .wim/.esd/.swm 结尾，忽略大小写）。用**手写**比较，
// 不引 locale（避免 <cwctype> 依赖）。
bool LooksLikeImagePath(const std::wstring& p) {
    if (p.size() < 4) return false;
    size_t n = p.size();
    auto eq = [&](const wchar_t* ext) {
        for (int i = 0; i < 4; ++i) {
            wchar_t a = p[n - 4 + i];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (a != ext[i]) return false;
        }
        return true;
    };
    return eq(L".wim") || eq(L".esd") || eq(L".swm");
}

// 镜像路径是否"值得尝试打开"。
// ⚠️ 不要只用 GetFileAttributesW 预筛：GUI 带 requireAdministrator 清单（提权进程），
// 受 **UAC 会话隔离**影响，提权后的登录会话对 UNC 路径（\\server\share\...）常拿不到
// 属性 → 返回 INVALID → 以前直接跳过 LoadWimImages → 主按钮**一直灰且无任何报错**
// （用户 2026-09-23 实测：本地路径能变蓝，网络/UNC 路径不变蓝）。改为：只要路径
// 像镜像文件就交给 wimlib 去开 —— 开得了按钮变蓝，开不了也会给出明确错误。
bool ImagePathReady(const std::wstring& p) {
    return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ||
           LooksLikeImagePath(p);
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
// 方案 C（ESP 并入）另有中转文件 `<目标>.stage` / `.stage.tmp`（见
// ops.cpp::RunBackup 的 stage 流程），删除白名单见 CleanupIncompleteOutput。
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

// 「讨论」链接（用户规格 2026-09-23）：初期指向无忧论坛的这个帖子；
// 成熟后换成我们自己的站点 + 报错上报（用网站收集日志）。**只改这一处**。
const wchar_t* kSiteUrl =
    L"https://bbs.wuyou.net/forum.php?mod=viewthread&tid=453597";

// ────────────────── 基础 ──────────────────

CMainForm::CMainForm() : m_lastProgressPost(std::chrono::steady_clock::now()) {}
CMainForm::~CMainForm() {
    if (m_tipHwnd) { ::DestroyWindow(m_tipHwnd); m_tipHwnd = NULL; }
    if (m_worker.joinable()) {
        m_cancel = true;
        m_worker.join();
    }
}

CDuiString CMainForm::GetSkinFolder() { return _T("skin\\"); }
CDuiString CMainForm::GetSkinFile()  {
    // 交给 builder 的是**已按词典翻译的整段 XML**（builder 认 "<" 开头的字符串，
    // 见 UIDlgBuilder.cpp）；读不到/无译文时回退原文件名，行为与改动前一致。
    std::wstring xml = sysrecover::LoadSkinXml(L"main.xml");
    if (!xml.empty()) return CDuiString(xml.c_str());
    return _T("main.xml");
}
LPCTSTR CMainForm::GetWindowClassName() const { return _T("SysRecoverUI"); }

void CMainForm::InitWindow() {
    // 日志根（PIT-104/105）：一般=程序目录；软件在系统盘时=<数据盘>\ZJRESTORE
    //（还原/重装系统盘后日志仍留存）。
    std::wstring logsDir = sysrecover::LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    LogInit(logsDir);
    ProgressInit(logsDir);
    // 自诊断与痕迹收集（用户 2026-10-03 规格）：启动即把 diag.txt/list.txt 写进 logs，
    // 并收集各盘上我们留下的部署文件 —— 用户排错只需发 logs 文件夹。
    sysrecover::WriteDiagFiles(logsDir);
    sysrecover::CollectDeployArtifacts(logsDir + L"\\collected", 0);
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
    m_tipHwnd = ::CreateWindowEx(0, TOOLTIPS_CLASS, NULL,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hWnd, NULL, ::GetModuleHandle(NULL), NULL);
    if (m_tipHwnd) {
        // cbSize 必须用 V1 尺寸：本 exe 无 comctl32 v6 清单（PIT-018）→ 跑 v5，
        // 而 MinGW 的 TOOLINFOW 多带一个 v6 才有的 void* lpReserved（sizeof=72），
        // v5 只认 V1/V2 尺寸 → 传 sizeof(TOOLINFO) 会被拒，TTM_ADDTOOL 返回 FALSE。
        m_tipInfo.cbSize = TTTOOLINFO_V1_SIZE;
        m_tipInfo.uFlags = 0;
        m_tipInfo.hwnd = m_hWnd;
        m_tipInfo.uId = 1;
        m_tipInfo.lpszText = const_cast<LPTSTR>(_T(""));
        m_tipInfo.rect = {0, 0, 0, 0};
        ::SendMessage(m_tipHwnd, TTM_ADDTOOL, 0, (LPARAM)&m_tipInfo);
        ::SendMessage(m_tipHwnd, TTM_SETMAXTIPWIDTH, 0, 300);
    }
    // 救援层失败回执检查（用户 2026-10-04 规格 2）：异步（等窗口完全建好再弹）
    ::PostMessage(m_hWnd, WM_CHECK_RESCUE, 0, 0);
}

// ────────────────── HandleMessage（worker 线程安全回调） ──────────────────

LRESULT CMainForm::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_MOUSELEAVE) {
        ::SendMessage(m_tipHwnd, TTM_POP, 0, 0);
    }
    if (msg == WM_MOUSEMOVE) {
        POINT pt = {(short)LOWORD(lParam), (short)HIWORD(lParam)};
        CControlUI* pControl = m_PaintManager.FindControl(pt);
        if (pControl && !pControl->GetToolTip().IsEmpty()) {
            m_tipText = pControl->GetToolTip().GetData();
            m_tipInfo.rect = pControl->GetPos();
            m_tipInfo.lpszText = const_cast<LPTSTR>(m_tipText.c_str());
            ::SendMessage(m_tipHwnd, TTM_SETTOOLINFO, 0, (LPARAM)&m_tipInfo);
        } else {
            // 移到无说明区域：清空矩形并收起，避免 TTM_RELAYEVENT 沿用旧矩形再弹。
            m_tipInfo.rect = {0, 0, 0, 0};
            m_tipInfo.lpszText = const_cast<LPTSTR>(_T(""));
            ::SendMessage(m_tipHwnd, TTM_SETTOOLINFO, 0, (LPARAM)&m_tipInfo);
            ::SendMessage(m_tipHwnd, TTM_POP, 0, 0);
        }
        MSG msgStruct = {};
        msgStruct.hwnd = m_hWnd;
        msgStruct.message = msg;
        msgStruct.wParam = wParam;
        msgStruct.lParam = lParam;
        msgStruct.time = ::GetTickCount();
        msgStruct.pt = pt;
        ::SendMessage(m_tipHwnd, TTM_RELAYEVENT, 0, (LPARAM)&msgStruct);
    }
    if (msg == WM_COMMAND && HIWORD(wParam) == EN_SETFOCUS) {
        // 点击/Tab 进入镜像输入框（还原模式）→ 若上次「搜索」有结果，在输入框
        // 下方弹下拉列表供选择（用户 2026-09-30 规格：点输入框可下拉选择某一个）。
        // 只认 ImagePath 的原生 EDIT：NoteInput 等其它 EDIT 也发 EN_SETFOCUS，
        // 用 GetFocus() 精确比对目标子窗口。
        // ⚠️ 不能在这里直接开菜单（PIT-097）：EN_SETFOCUS 是在原生 EDIT 的
        // WM_LBUTTONDOWN 处理链里同步发出的，紧接着 TrackPopupMenu 会捕获鼠标、
        // 吃掉这次点击的 WM_LBUTTONUP → EDIT 内部卡在"左键按住"状态，表现为
        // "选完镜像文字跟着鼠标拖选、必须再点一下才恢复"。改为投递延迟消息，
        // 在编辑框这次点击完整结束（左键抬起）后再开菜单。
        if (!m_backupMode && !m_busy && !m_pickGuard && !m_searchHits.empty()) {
            CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
            HWND hNative =
                pCtl ? static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND() : NULL;
            if (hNative && ::GetFocus() == hNative)
                ::PostMessage(m_hWnd, WM_OPEN_PICK_MENU, 0, 0);
        }
        return 0;
    }
    if (msg == WM_OPEN_PICK_MENU) {
        // 「点输入框弹搜索结果下拉」的延迟执行（PIT-097）：必须等这次点击的
        // 左键抬起再开菜单，否则模态菜单吃掉 EDIT 的 WM_LBUTTONUP，EDIT 会
        // 永远停在"按住拖选"状态。等不到就 15ms 后再投一次（菜单打开期间
        // 用户不会一直按着左键，通常一两次就过）。
        if (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            ::Sleep(15);
            ::PostMessage(m_hWnd, WM_OPEN_PICK_MENU, 0, 0);
            return 0;
        }
        if (!m_backupMode && !m_busy && !m_searchHits.empty()) {
            CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
            HWND hEdit =
                pCtl ? static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND() : NULL;
            // 焦点已移走（用户这会儿点了别处）就不再弹了
            if (hEdit && ::GetFocus() == hEdit) ShowImagePickMenu();
        }
        return 0;
    }
    if (msg == WM_COMMAND && HIWORD(wParam) == EN_CHANGE) {
        // 输入框内容变化 —— 原生 EDIT 的 EN_CHANGE 会送到父窗口（=主窗口，Duilib
        // 控件本身不是窗口）。用户 2026-09-23 反馈：**手动输入**镜像路径时
        // "开始备份"按钮一直是灰的（以前只有"浏览…"/拖入才会刷新按钮）。
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
                    m_wimPath != m_lastLoadedWim && ImagePathReady(m_wimPath)) {
                    m_lastLoadedWim = m_wimPath;
                    LoadWimImages(m_wimPath);
                }
                SyncFormatFromPath();  // 备份：路径带 .wim/.esd → 自动定格式
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
            SetStatus(m_lastStage + Tr(L"    已用 ") + ElapsedText());
        return 0;
    }
    if (msg == WM_PROGRESS_UPDATE) {
        // 防御（PIT-096）：lParam 是 worker new 的 std::wstring*，正常永不为 0；
        // 为 0 = 消息撞车/异常投递，必须丢弃 —— 解引用空指针就是 2026-09-30
        // 用户实测的崩溃点（read address 0x8）。已另有消息重编号根治撞车。
        if (lParam) {
            OnProgressUpdate((int)wParam, *reinterpret_cast<std::wstring*>(lParam));
            delete reinterpret_cast<std::wstring*>(lParam);
        } else {
            LogWarn("progress msg with null lParam ignored (msg=" +
                    std::to_string((unsigned)msg) + " wParam=" +
                    std::to_string((unsigned long long)wParam) + ")");
        }
        return 0;
    }
    if (msg == WM_TASK_COMPLETE) {
        OnTaskComplete((int)wParam);
        return 0;
    }
    if (msg == WM_SUPPORT_DONE) {
        OnSupportDone(reinterpret_cast<sysrecover::SupportBundle*>(lParam));
    }
    if (msg == WM_CHECK_RESCUE) {
        ShowRescueFailureNotice();
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
    if (s.compare(0, 5, L"write") == 0) return Tr(L"写入");
    if (s.compare(0, 7, L"extract") == 0) return Tr(L"写入");
    if (s.compare(0, 4, L"scan") == 0)
        return s.size() > 5 ? Tr(L"扫描 ") + s.substr(5) : Tr(L"扫描");
    // 收官阶段（PIT-098）：100% 后的静默期靠这些名字在状态栏"报活"。
    // 都按前缀匹配 —— 带速度后缀（"  42.3 MB/s  ETA 1:23"）也能命中。
    if (s.compare(0, 3, L"esp") == 0) return Tr(L"备份ESP");
    if (s.compare(0, 6, L"verify") == 0) return Tr(L"校验镜像");
    if (s.compare(0, 5, L"probe") == 0) return Tr(L"完整性检查");
    if (s.compare(0, 7, L"ea-scan") == 0) return Tr(L"文件属性检查");
    return s;
}

void CMainForm::OnProgressUpdate(int pct, const std::wstring& stage) {
    // 进度条随**第一条进度**出现（不是点按钮那一刻）：校验/暂存阶段没有进度，
    // 整行状态栏留给状态文字；真开始跑进度了，文字才收在进度条左边。
    ShowProgress(true);
    SetProgress(pct);
    m_lastStage = StageCn(stage);
    SetStatus(m_lastStage + Tr(L"    已用 ") + ElapsedText());
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
        ShowProgress(false);  // 收起进度条，状态栏回到整行可用
        return;
    }
    // 任务结束立刻收起进度条：完成/失败消息（英文可达 453px）需要整行，
    // 而进度条一在，状态文字就被卡在 179px 里。
    ShowProgress(false);
    SetProgress(rc == 0 ? 100 : 0);
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) pPct->SetText(rc == 0 ? _T("100%") : _T(""));
    UpdateMainAction();
    if (rc == 4) {
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, U2W(last_err_).c_str(), Tr(L"安全门禁拒绝"), MB_OK | MB_ICONERROR);
    } else if (rc != 0) {
        // P8：把"下一步怎么办"一起给用户（空间不足/坏镜像等都有对应建议）。
        // M1（PLAN §14）：按**建议码**取词，不再匹配消息文本（消息一翻译关键词就失配）。
        std::string advice = sysrecover::ErrorAdvice(rc, last_adv_);
        std::wstring box = U2W(last_err_) + U2W(advice);
        SetStatus(U2W(last_err_));
        MessageBoxW(m_hWnd, box.c_str(),
                    m_backupMode ? Tr(L"备份失败") : Tr(L"暂存失败"), MB_OK | MB_ICONERROR);
    } else if (m_backupMode) {
        m_imageOk = true;  // 产物就是刚写的有效镜像 → 切到还原页主按钮即可用
        SetStatus(Tr(L"备份完成（用时 ") + ElapsedText() + Tr(L"）"));
        if (!IsSilent())
            MessageBoxW(m_hWnd,
                        (Tr(L"系统备份已完成。\n用时 ") + ElapsedText() + Tr(L"。"))
                            .c_str(),
                        Tr(L"备份成功"), MB_OK | MB_ICONINFORMATION);
    } else if (m_needReboot) {
        // 需要重启（还原运行中的系统盘）：静默或用户已选「退出并重启」→ 直接重启
        SetStatus(Tr(L"暂存完成（用时 ") + ElapsedText() + Tr(L"），正在重启..."));
        RebootNow();
    } else {
        // 就地还原完成（PE 里 / 还原到非系统盘）：不重启，直接报完成。
        // ⚠️ 文案（用户 2026-09-23 反馈）：原来写"无需重启"，用户会理解成
        // "现在就能用还原好的系统了" ✗ —— 其实必须**重启**才能进入还原的系统
        // （我们只是没有自动重启而已）。所以明确写"重启后即可进入"。
        SetStatus(Tr(L"还原完成（用时 ") + ElapsedText() + Tr(L"）"));
        if (!IsSilent())
        MessageBoxW(m_hWnd,
                    (Tr(L"系统还原已完成，用时 ") + ElapsedText() +
                     Tr(L"。\n重启后即可进入恢复的系统。"))
                        .c_str(),
                    Tr(L"还原成功"), MB_OK | MB_ICONINFORMATION);
    }
}

// ── 「日志」按钮：一键取证（支持包） ─────────────────────────────────────
// 用户规格（2026-10-03）：把"让用户做的事"全部自动化 —— 收集各盘日志 + 抓
// explorer 转储 + 导出事件日志 → 打包到桌面；结果框里再提供"重启进入安全模式"。
void CMainForm::ExportSupportFlow() {
    if (m_busy)
        return;
    m_busy = true;
    m_lastStage = Tr(L"正在收集日志与诊断信息…");
    SetStatus(m_lastStage);
    UpdateMainAction();
    if (m_worker.joinable())
        m_worker.join();
    m_worker = std::thread([this] {
        auto* r = new sysrecover::SupportBundle(
            sysrecover::BuildSupportBundle(std::wstring()));
        ::PostMessage(m_hWnd, WM_SUPPORT_DONE, 0, (LPARAM)r);
    });
}

void CMainForm::OnSupportDone(sysrecover::SupportBundle* rp) {
    sysrecover::SupportBundle r = rp ? *rp : sysrecover::SupportBundle();
    delete rp;
    if (m_worker.joinable())
        m_worker.join();
    m_busy = false;
    UpdateMainAction();
    if (r.zipPath.empty()) {
        std::wstring msg = Tr(L"导出诊断包失败：") + r.error;
        SetStatus(msg);
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"日志"), MB_OK | MB_ICONERROR);
        return;
    }
    size_t pos = r.zipPath.find_last_of(L'\\');
    std::wstring file =
        pos == std::wstring::npos ? r.zipPath : r.zipPath.substr(pos + 1);
    SetStatus(Tr(L"诊断包已导出：") + file);
    LogInfo("support bundle exported: " + W2U(r.zipPath) + " (dumps=" +
            std::to_string(r.dumps) + ", events=" + std::to_string(r.eventFiles) +
            ")");
    // ⚠️ CConfirmDlg 只显示正文**前 3 行**（PIT-093）且单行有宽度上限——
    // 文案必须压成 3 行短句；文件名靠「完成」后的资源管理器选中来展示。
    bool safe = ::GetSystemMetrics(SM_CLEANBOOT) != 0;
    std::wstring msg = std::wstring(Tr(L"诊断包已导出到桌面。\n")) +
                       Tr(L"请把这个文件发给技术支持；\n");
    int choice;
    if (safe) {
        msg += Tr(L"当前在安全模式，是否退出并重启？");
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"日志"), msg, Tr(L"完成"),
                                   Tr(L"退出安全模式并重启"),
                                   /*defaultIsRight=*/false);
    } else {
        msg += Tr(L"是否重启进安全模式再测一次？");
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"日志"), msg, Tr(L"完成"),
                                   Tr(L"重启进安全模式再测"),
                                   /*defaultIsRight=*/false);
    }
    if (choice == 1) {
        std::string log;
        bool ok = sysrecover::BcdSetSafeBoot(!safe, log);
        LogInfo(std::string("safe boot toggle: ") + log);
        if (!ok) {
            MessageBoxW(m_hWnd, (Tr(L"安全模式设置失败：") + U2W(log)).c_str(),
                        Tr(L"日志"), MB_OK | MB_ICONERROR);
        } else {
            RebootNow();
        }
    } else {
        // 「完成」：在资源管理器里选中刚导出的包，方便用户直接拖进聊天窗口。
        std::wstring args = L"/select,\"" + r.zipPath + L"\"";
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                        SW_SHOWNORMAL);
    }
}

// 启动检查：救援层失败回执（黑匣子，用户 2026-10-04 规格 2）。任何失败都要
// "开机即报"，而不是让用户回忆/录像。回执由救援层写到每个分区根/ESP，启动
// 收集时已收进 logs\collected；同一回执只提示一次（去重在 CheckLastRescueFailure）。
void CMainForm::ShowRescueFailureNotice() {
    std::wstring notice = sysrecover::CheckLastRescueFailure();
    if (notice.empty())
        return;
    size_t nl = notice.find(L'\n');
    SetStatus(nl == std::wstring::npos ? notice : notice.substr(0, nl));
    int choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"还原结果"), notice,
                                   Tr(L"知道了"), Tr(L"打开日志文件夹"),
                                   /*defaultIsRight=*/false);
    if (choice == 1) {
        std::wstring dir = sysrecover::LogBaseDir() + L"\\logs\\collected";
        ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
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
    std::wstring msg = Tr(L"任务正在执行中（已用 ") + ElapsedText() + Tr(L"）。\n" L"选择「继续等待」：任务照常进行，不受影响。\n" L"选择「终止并退出」：立即中止任务，并删除未写完的镜像文件。");
    // 默认项 = 左按钮 =「继续等待」：回车/ESC 都落在安全项上，防误取消。
    int r = CConfirmDlg::Ask2(m_hWnd, Tr(L"任务执行中"), msg, Tr(L"继续等待"),
                              Tr(L"终止并退出"), /*defaultIsRight=*/false);
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
            list += Tr(L"、");
        list += vols[i];
    }
    LogInfo("BitLocker volumes detected: " + W2U(list));
    if (IsSilent())
        return true;
    // 文案按用户 2026-09-23 的反馈断行（原第 2 行太长被右边裁掉）：
    // 在"这些卷的数据在还原后"处加逗号换行，末尾挪到下一行。
    std::wstring msg = Tr(L"检测到本机有 BitLocker 加密的卷：") + list + Tr(L"\n" L"如果你没有对应的密码 / 恢复密钥，这些卷的数据在还原后，\n" L"将无法恢复。是否继续还原？");
    return CConfirmDlg::Ask2(m_hWnd, Tr(L"BitLocker 提醒"), msg, Tr(L"退出"), Tr(L"继续"),
                             /*defaultIsRight=*/false) == 1;
}

// 目标盘健康警告（PIT-086，用户 2026-09-26）：还原要格式化目标分区，若目标盘已现坏道，
// 还原完系统照样可能起不来 —— 先警告，让用户决定要不要先换盘。返回 true = 继续。
// 静默模式跳过弹框（无人值守），但仍写一条日志。
bool CMainForm::AskDiskHealthWarning(int diskIndex) {
    std::string hw = sysrecover::CheckDiskHealth(diskIndex);
    if (hw.empty())
        return true;  // 健康 或 取不到 SMART（NVMe/RAID/USB）→ 放行
    LogInfo("target disk health warning: " + hw);
    if (IsSilent())
        return true;
    std::wstring msg = U2W(hw) + Tr(L"\n\n是否仍要继续还原？");
    return CConfirmDlg::Ask2(m_hWnd, Tr(L"磁盘健康警告"), msg, Tr(L"取消"), Tr(L"仍然继续"),
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
        for (const auto& p : {IncompletePath(m_workerDest),
                              m_workerDest + L".stage",
                              m_workerDest + L".stage.tmp"})
            RegisterPendingDelete(p);
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
    // 只删**本次正在写的半成品**（见 wim.cpp::Capture 的原子写与 ops.cpp::RunBackup
    // 的 stage 流程）：`<目标>.tmp`（Capture）、`<目标>.stage` + `<目标>.stage.tmp`
    // （ESP 并入中转）。**绝不能删最终路径** —— 那里可能是用户之前就存在的
    // 有效镜像，或 ESP 并入失败时已 rename 交付的主镜像。
    if (m_workerDest.empty())
        return;
    for (const auto& tmp : {IncompletePath(m_workerDest),
                            m_workerDest + L".stage",
                            m_workerDest + L".stage.tmp"}) {
        if (::GetFileAttributesW(tmp.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;  // 正常取消时 Capture/RunBackup 已经删过了，这里是兜底
        ::SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (::DeleteFileW(tmp.c_str())) {
            LogInfo("cancel: removed incomplete temp " + W2U(tmp));
        } else {
            LogError("cancel: cannot remove incomplete temp (err=" +
                     std::to_string(::GetLastError()) +
                     "), scheduled at reboot");
            RegisterPendingDelete(tmp);
        }
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
    // 任务执行中只放行一件事：改「限制CPU」—— 用户 2026-09-28 规格明确要求
    // 「点击或**备份过程中**点击」都能调（改的是 Job 速率，随时可改，安全）。
    const bool cpuSel = msg.sType == DUI_MSGTYPE_ITEMSELECT &&
                        msg.pSender && msg.pSender->GetName() == _T("CpuCapBox");
    if (m_busy && !cpuSel) return;  // 任务执行中忽略其他点击
    if (msg.sType == DUI_MSGTYPE_CLICK) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("BrowseBtn")) {
            if (m_backupMode) BrowseSaveFile(); else BrowseWimFile();
        } else if (name == _T("SearchBtn")) {
            OnSearchImages();  // 还原模式专属（备份模式按钮隐藏）
        } else if (name == _T("MainAction")) {
            if (m_backupMode) StartBackup(); else StartRestore();
        } else if (name == _T("RepairBootBtn")) {
            // 「清除引导」：删 BCD 条目 + 清 bootsequence + 删数据盘上的
            // grldr/grldr.mbr/menu.lst/ZJRESTORE（用户要求：要清就清干净）
            std::string log;
            bool ok = RemoveBootLayer(log);
            DeleteMenuBinding();  // 绑定副契约一并清（同 BootMenuBtn，2026-10-05）
            SetStatus(ok ? Tr(L"已清除引导项与相关文件") : Tr(L"清除引导项未完全成功"));
            LogInfo(std::string("GUI remove boot layer: ") + log);
            RefreshBootMenuBtn();
        } else if (name == _T("BootMenuBtn")) {
            ToggleBootMenu();
        } else if (name == _T("LogBtn")) {
            ExportSupportFlow();
        } else if (name == _T("SiteLink")) {
            // 右下角「讨论」链接 → 用系统默认浏览器打开。
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
        } else if (name == _T("FormatBox")) { /* 见下方 ITEMSELECT 分支（Combo 发的是 ITEMSELECT） */ }
    } else if (msg.sType == DUI_MSGTYPE_ITEMSELECT) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("PartitionBox")) {
            m_selPart = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            UpdateMainAction();
        } else if (name == _T("CpuCapBox")) {
            ApplyCpuCapFromUi();  // 空闲或备份中都直接生效
        } else if (name == _T("ImageIndexBox")) {
            int sel = static_cast<CComboUI*>(msg.pSender)->GetCurSel();
            if (sel >= 0 && sel < (int)m_imgIdx.size())
                m_selImageIndex = m_imgIdx[sel];
        } else if (name == _T("FormatBox")) {
            // ★ 2026-09-30 用户规格：换「类型」→ **同步改文件名后缀**。
            // 坑：Combo 发的是 DUI_MSGTYPE_ITEMSELECT，**不是** SELECTCHANGED ——
            //   之前这句挂在 SELECTCHANGED 分支上，所以点了「类型」文件名后缀不变。
            SyncPathExtFromFormat();
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
    CControlUI* p = m_PaintManager.FindControl(_T("BootMenuBtn"));
    if (!p) return;
    // 文字色在 CButtonUI 上（SkinButton 派生自它；CControlUI 没有 SetTextColor）。
    CButtonUI* btn = static_cast<CButtonUI*>(p);
    std::string detail;
    // "已安装" = 救援环境在（UEFI 启动项 / BIOS 四件套）**或**契约里有绑定
    //（后者覆盖"把救援层装到非系统盘"的情况，见 StageRestoreMenu）。
    std::wstring boundImg;
    int boundIdx = 1;
    unsigned long long boundOff = 0, boundSz = 0;
    const bool hasBinding =
        ReadMenuBinding(&boundImg, &boundIdx, &boundOff, &boundSz);
    const bool installed = BootMenuInstalled(detail) || hasBinding;

    wchar_t tip[512] = {};
    if (installed) {
        // 已安装：按钮 = 「删除菜单」；悬停说清"绑的是哪个镜像 → 哪个分区"。
        // 用户规格（2026-09-29）：要换镜像请**先删除再安装**。
        if (hasBinding) {
            std::wstring tgt;
            bool uefiPath = false;
            for (const auto& d : EnumerateDisks()) {
                for (const auto& q : d.parts) {
                    if (boundOff && q.offsetBytes == boundOff &&
                        q.sizeBytes == boundSz && !q.letter.empty()) {
                        tgt = q.letter + Tr(L": 盘");
                        uefiPath = UseUefiBootFor(q);  // UEFI=固件启动项，选法不同
                    }
                }
            }
            if (tgt.empty()) tgt = Tr(L"（原目标分区）");
            if (uefiPath)
                swprintf(tip, 512,
                         Tr(L"已安装：开机按 F12 启动菜单选「SysRecover」，\n把 %ls（第 %d 个镜像）还原到 %ls。\n点此删除（要换镜像请先删除、再安装）"),
                         boundImg.c_str(), boundIdx, tgt.c_str());
            else
                swprintf(tip, 512,
                         Tr(L"已安装：开机菜单里选「SysRecover」即可把\n%ls（第 %d 个镜像）还原到 %ls。\n点此删除（要换镜像请先删除、再安装）"),
                         boundImg.c_str(), boundIdx, tgt.c_str());
        } else {
            wcscpy_s(tip, Tr(L"已安装启动还原（旧版安装，未绑定镜像）。点此删除"));
        }
        p->SetText(Tr(L"删除菜单"));
        p->SetEnabled(!m_busy);
        p->SetBkColor(0xFFFFFFFF);
        btn->SetTextColor(ui_skin::C_TEXT);
    } else {
        // 未安装：**必须先选到可用镜像 + 目标分区**才可点（用户 2026-09-29 规格：
        // 没选镜像时按钮应为灰色，否则"装成功却不知道装了什么"）。备份模式不涉及
        // 镜像绑定 → 一律灰（该按钮只在还原模式有意义）。
        const bool can = !m_backupMode && !m_wimPath.empty() && m_imageOk &&
                         m_selPart >= 0 && !m_busy;
        p->SetText(Tr(L"安装菜单"));
        p->SetEnabled(can);
        p->SetBkColor(can ? 0xFFFFFFFF : ui_skin::C_DISABLED);
        btn->SetTextColor(can ? ui_skin::C_TEXT : ui_skin::C_DIS_TEXT);
        wcscpy_s(tip, can
            ? Tr(L"把当前镜像写进开机菜单：重启后选「SysRecover」进恢复环境，\n自动把该镜像还原到第二步选中的目标分区")
            : Tr(L"请先选择镜像与目标分区，再安装菜单"));
    }
    p->SetToolTip(tip);
    p->Invalidate();
}

void CMainForm::ToggleBootMenu() {
    std::string detail, log;
    if (BootMenuInstalled(detail)) {
        bool ok = RemoveBootLayer(log);
        // 绑定副契约必须一起删：否则 ReadMenuBinding 仍判"已安装" → 按钮
        // 停在「删除菜单」回不到「安装菜单」（用户 2026-10-05 实测 bug）。
        DeleteMenuBinding();
        SetStatus(ok ? Tr(L"已删除启动还原") : Tr(L"删除启动还原未完全成功"));
        LogInfo(std::string("GUI boot menu toggle: ") + log);
        RefreshBootMenuBtn();
        UpdateMainAction();
        return;
    }
    // ── 安装（用户 2026-09-29 规格）：把**当前镜像 → 第二步选中的目标分区**写成
    //    常驻任务契约，并装常驻启动项；开机菜单里选「SysRecover」即执行该还原。
    if (m_wimPath.empty() || m_selPart < 0 ||
        m_selPart >= (int)m_parts.size()) {
        SetStatus(Tr(L"请先选择镜像与目标分区，再安装菜单"));
        return;
    }
    {
        const PartitionInfo& part = m_parts[m_selPart];
        // ⚠️ CConfirmDlg 的正文只显示**最多 3 行**、且是**单行标签**（PIT-039），
        // 超长/超行会被静默裁掉 —— 所以这里刻意压成 3 行短句：只显示文件名
        //（路径可能很长，裁到 18 字符），并把"会格式化"这句放在第 3 行。
        std::wstring name = m_wimPath;
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos)
            name = name.substr(sl + 1);
        if (name.size() > 18)
            name = name.substr(0, 18) + L"...";
        wchar_t msg[512];
        swprintf(msg, 512,
                 Tr(L"把当前镜像写进开机启动菜单？\n"
                    L"镜像：%ls（第 %d 个）\n"
                    L"目标：%ls: 盘 —— 选该菜单项会格式化该分区"),
                 name.c_str(), m_selImageIndex,
                 part.letter.empty() ? L"?" : part.letter.c_str());
        // 与"确认还原"一致：右按钮（动作）高亮且为回车默认项，ESC 一律取消。
        if (CConfirmDlg::Ask2(m_hWnd, Tr(L"安装启动还原菜单"), msg, Tr(L"取消"),
                              Tr(L"安装"), /*defaultIsRight=*/true) != 1)
            return;
        RestoreRequest req;
        req.image = m_wimPath;
        req.disk = (int)part.diskIndex;
        req.part = (int)part.partNumber;
        req.index = m_selImageIndex;
        req.repairBoot = true;
        std::string err;
        m_busy = true;
        UpdateMainAction();
        SetStatus(Tr(L"正在安装启动还原菜单（写入契约 + 部署救援环境）..."));
        int rc = StageRestoreMenu(req, err, &last_adv_);
        m_busy = false;
        if (rc == 0)
            // 两条引导链的"进菜单"方式不同（PIT-092/094）：BIOS 在 bootmgr 的
            // 选择菜单里选；UEFI 是固件启动项，要开机按 F12。
            SetStatus(UseUefiBootFor(part)
                          ? Tr(L"启动还原菜单已安装：开机按 F12 启动菜单选「SysRecover」即可还原该镜像")
                          : Tr(L"启动还原菜单已安装：开机选「SysRecover」即可还原该镜像"));
        else if (!err.empty())
            SetStatus(U2W(err));
        else
            SetStatus(Tr(L"安装启动还原菜单失败"));
        LogInfo(std::string("GUI install boot menu rc=") + std::to_string(rc) +
                " / " + err);
    }
    RefreshBootMenuBtn();
    UpdateMainAction();
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
                                   ? (Tr(L"可用 ") + FormatGb(part.freeBytes))
                                   : Tr(L"系统保留");
            int pct = part.sizeBytes
                      ? (int)(used * 100 / part.sizeBytes) : 0;
            item->SetPart(part.letter.empty() ? Tr(L"—") : part.letter.c_str(),
                          U2W(StyleName(disk.style)).c_str(),
                          part.fs.c_str(), part.isSystem,
                          disk.model.empty() ? Tr(L"本地磁盘") : disk.model.c_str(),
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
    if (FAILED(hr) || !pDlg) { SetStatus(Tr(L"文件对话框创建失败")); return; }
    COMDLG_FILTERSPEC filters[] = {
        {Tr(L"WIM/ESD 镜像"), L"*.wim;*.esd"},
        {Tr(L"所有文件"), L"*.*"},
    };
    pDlg->SetFileTypes(2, filters);
    pDlg->SetTitle(Tr(L"选择系统镜像文件"));
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
    if (FAILED(hr) || !pDlg) { SetStatus(Tr(L"文件对话框创建失败")); return; }
    // ★ 2026-09-30 用户规格：浏览框的「保存类型」必须与界面上的「类型」下拉**一致**
    //   （同样的三项），且选中后**要生效** —— 以前只有两项、选了既不回写界面下拉、
    //   也改变不了真正决定格式的东西（文件名后缀/界面下拉），属 bug。
    //   现在三项一一对应：选完立即回写界面「类型」下拉，并把文件名后缀对齐到所选类型。
    COMDLG_FILTERSPEC filters[] = {
        {Tr(L"esd(慢)"), L"*.esd"},
        {Tr(L"wim(中)"), L"*.wim"},
        {Tr(L"wim(快)"), L"*.wim"},
    };
    pDlg->SetFileTypes(3, filters);
    pDlg->SetTitle(Tr(L"选择备份保存位置"));
    int fmtIdx = BackupFmt();                 // 0=.esd 1=.wim正常 2=.wim高速
    pDlg->SetFileTypeIndex(static_cast<UINT>(fmtIdx + 1));  // COMDLG 1-based
    const wchar_t* ext = fmtIdx == 0 ? L".esd" : L".wim";
    // 默认文件名（用户规格 2026-09-21）：`20260921Win10.19044备份.esd`
    std::wstring defName = TimestampDate() +
                           (m_sysDesc.shortTag.empty() ? TimestampName()
                                                       : m_sysDesc.shortTag) +
                           Tr(L"备份") + ext;
    pDlg->SetFileName(defName.c_str());
    if (SUCCEEDED(pDlg->Show(m_hWnd))) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pDlg->GetResult(&pItem))) {
            LPWSTR psz = nullptr;
            pItem->GetDisplayName(SIGDN_FILESYSPATH, &psz);
            if (psz) {
                std::wstring path = psz;
                CoTaskMemFree(psz);
                // 对话框里选的「保存类型」→ 生效：回写界面「类型」下拉 + 后缀对齐
                UINT fti = 0;
                if (SUCCEEDED(pDlg->GetFileTypeIndex(&fti)) && fti >= 1 && fti <= 3) {
                    int want = static_cast<int>(fti) - 1;   // 0/1/2
                    const wchar_t* wantExt = (want == 0) ? L".esd" : L".wim";
                    size_t slash = path.find_last_of(L"\\/");
                    size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
                    size_t dot = path.find_last_of(L'.');
                    if (dot != std::wstring::npos && dot > base)
                        path = path.substr(0, dot) + wantExt;  // 换掉旧后缀
                    else
                        path += wantExt;                       // 本来就没后缀
                    CComboUI* pFmt = static_cast<CComboUI*>(
                        m_PaintManager.FindControl(_T("FormatBox")));
                    if (pFmt && pFmt->GetCount() > want) pFmt->SelectItem(want, false, false);
                }
                m_wimPath = path;
                CEditUI* pEdit =
                    static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
                if (pEdit) pEdit->SetText(m_wimPath.c_str());
                SetStatus(Tr(L"备份保存至：") + m_wimPath);
            }
            pItem->Release();
        }
    }
    pDlg->Release();
    UpdateMainAction();
}

// 「搜索」（还原模式第一步）：按 imgsearch.h 的三规则扫附近 .esd/.wim，
// 结果按修改时间新→旧，把最新一个填进输入框并立即解析子镜像。
void CMainForm::OnSearchImages() {
    if (m_busy) return;
    m_searchHits = imgsearch::Search();
    if (m_searchHits.empty()) {
        SetStatus(Tr(L"附近未找到 .esd/.wim 镜像文件"));
        return;
    }
    const std::wstring best = m_searchHits[0];
    m_wimPath = best;
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (pEdit) pEdit->SetText(best.c_str());  // 同步原生 EDIT → EN_CHANGE 闭环
    wchar_t buf[512];
    // MinGW %d=int ✓（路径等宽串在 swprintf 外拼接，避 PIT-007 的 %ls 坑）
    swprintf(buf, 512, Tr(L"找到 %d 个镜像文件，已填入最新："),
             (int)m_searchHits.size());
    SetStatus(std::wstring(buf) + best);  // 解析失败时会被 LoadWimImages 的错误覆盖
    LoadWimImages(best);
}

// 点输入框 → 在其下方弹「搜索结果」下拉（TrackPopupMenu，选中回填 + 解析）。
// 由 EN_SETFOCUS 进入（HandleMessage），m_pickGuard 防菜单关闭后 SetFocus 重入。
void CMainForm::ShowImagePickMenu() {
    CControlUI* pCtl = m_PaintManager.FindControl(_T("ImagePath"));
    if (!pCtl) return;
    HWND hEdit = static_cast<CSkinEditUI*>(pCtl)->GetNativeEditHWND();
    if (!hEdit) return;
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;
    for (size_t i = 0; i < m_searchHits.size(); ++i) {
        // 列表显示文件名（过长截断）；菜单项 ID = 下标 + 1（0 视为无效）
        std::wstring name = m_searchHits[i];
        size_t bs = name.find_last_of(L'\\');
        if (bs != std::wstring::npos) name = name.substr(bs + 1);
        if (name.size() > 48) name = name.substr(0, 45) + L"...";  // ASCII 截断符，不做词条
        UINT flags = MF_STRING;
        if (_wcsicmp(m_searchHits[i].c_str(), m_wimPath.c_str()) == 0)
            flags |= MF_CHECKED;  // 当前输入框里的那一个打勾
        ::AppendMenuW(hMenu, flags, (UINT)(i + 1), name.c_str());
    }
    RECT rc{};
    ::GetWindowRect(hEdit, &rc);
    m_pickGuard = true;
    int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN,
                               rc.left, rc.bottom + 1, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);
    if (cmd > 0 && cmd <= (int)m_searchHits.size()) {
        const std::wstring pick = m_searchHits[(size_t)cmd - 1];
        m_wimPath = pick;
        pCtl->SetText(pick.c_str());
        SetStatus(std::wstring(Tr(L"已选择镜像：")) + pick);
        LoadWimImages(pick);
        UpdateMainAction();
    }
    if (::IsWindow(hEdit)) ::SetFocus(hEdit);  // 焦点还给输入框（guard 拦下再弹）
    m_pickGuard = false;
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
        SetStatus(Tr(L"只支持拖入 .esd / .wim 镜像文件"));
        return;
    }
    m_wimPath = dropped;
    CEditUI* pEdit =
        static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
    if (pEdit) pEdit->SetText(m_wimPath.c_str());
    if (m_backupMode) {
        SetStatus(Tr(L"备份保存至：") + m_wimPath);
        SyncFormatFromPath();  // 拖进来的 .wim/.esd 同样决定格式
        UpdateMainAction();
    } else {
        LoadWimImages(m_wimPath);  // 内部设 m_imageOk 并刷新主按钮
        SetStatus(Tr(L"已载入镜像：") + m_wimPath);
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
        SetStatus(Tr(L"wimlib 初始化失败"));
        UpdateMainAction();
        return;
    }
    std::vector<ImageDesc> images;
    int rc = wim.ListImages(path, images);
    if (rc != 0 || images.empty()) {
        // 关键：读取失败（例如上次备份中途退出留下"写入未完成"的坏镜像）
        // 时必须保持 m_imageOk=false 并刷新按钮，否则主按钮的状态会停在
        // 上一次的可用态（用户看到"灰色按钮切一下模式又能点了"的怪象）。
        SetStatus(std::wstring(Tr(L"镜像不可用（可能上次备份未完成，请重新备份）: ")) +
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
        swprintf(buf, 512, Tr(L"%d - %ls（%.1f GB）"), img.index, img.name.c_str(),
                 img.sizeBytes / 1073741824.0);
        item->SetText(buf);
        pCombo->Add(item);
        m_imgIdx.push_back(img.index);
    }
    if (pCombo->GetCount() > 0) pCombo->SelectItem(0);
    m_selImageIndex = m_imgIdx.empty() ? 1 : m_imgIdx[0];
    m_imageOk = true;
    // 成功路径原先既不改状态栏、也不写日志 → 支持排查时看不出"加载过哪个镜像"，
    // 自动化测试也无法从日志判断加载成功（2026-10-07 实测）。补一行日志。
    LogInfo("GUI image loaded: " + W2U(path) + " (" +
            std::to_string(images.size()) + " subimage(s), first idx=" +
            std::to_string(m_selImageIndex) + ")");
    UpdateMainAction();
}

// ────────────────── 备份格式 combo 辅助 ──────────────────

int CMainForm::BackupFmt() const {
    CComboUI* pFmt =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("FormatBox")));
    int fmt = pFmt ? pFmt->GetCurSel() : 1;
    return (fmt < 0 || fmt > 2) ? 1 : fmt;
}

// 按第一步的文件后缀定「格式」（用户 2026-09-28 规格）：路径里输了 `.wim` 就自动
// 选中「.wim(正常大小)」、`.esd` 选「.esd(慢速小体积)」，不必再手点一次下拉。
// 只在**路径变化**时同步 —— 手动换格式不会改路径，所以不会被反复顶回去。
void CMainForm::SyncFormatFromPath() {
    if (!m_backupMode) return;
    std::wstring p = m_wimPath;
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"' || p.back() == L'\''))
        p.pop_back();
    size_t slash = p.find_last_of(L"\\/");
    std::wstring fn = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
    if (fn.size() < 4) return;  // 后缀还没输完，别乱动
    std::wstring ext = fn.substr(fn.size() - 4);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext != L".wim" && ext != L".esd") return;
    int want = (ext == L".wim") ? 1 : 0;
    CComboUI* pFmt =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("FormatBox")));
    if (pFmt && pFmt->GetCount() > want && pFmt->GetCurSel() != want)
        pFmt->SelectItem(want, false, false);  // 程序选择：不触发 SELECTCHANGED（免回环）
}

// ★ 2026-09-30 用户规格：换「类型」→ **同步改文件名后缀**（只改后缀，文件名其余不动）。
// 例：`D:\x\20260921Win10备份.esd` 选「wim(中)」→ `...备份.wim`。
// 与 SyncFormatFromPath 互为反向：那个是「后缀→下拉」，这个是「下拉→后缀」。
void CMainForm::SyncPathExtFromFormat() {
    if (!m_backupMode || m_wimPath.empty()) return;
    const wchar_t* wantExt = (BackupFmt() == 0) ? L".esd" : L".wim";
    std::wstring p = m_wimPath;
    size_t slash = p.find_last_of(L"\\/");
    size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t dot = p.find_last_of(L'.');
    std::wstring np = (dot != std::wstring::npos && dot > base)
                          ? p.substr(0, dot) + wantExt   // 换掉旧后缀
                          : p + wantExt;                 // 本来就没后缀
    if (np == m_wimPath) return;
    m_wimPath = np;
    CEditUI* pEdit =
        static_cast<CEditUI*>(m_PaintManager.FindControl(_T("ImagePath")));
    if (pEdit) pEdit->SetText(m_wimPath.c_str());  // 同步原生 EDIT（EN_CHANGE 闭环）
}

// 「限制CPU」下拉 → 上限百分比（下标 0/1/2/3 = 不限/25/50/75，见 skin/main.xml）。
// 备份中改了立即生效：改的是 Job 对象的速率，不打断正在跑的 wimlib。
void CMainForm::ApplyCpuCapFromUi() {
    CComboUI* p =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("CpuCapBox")));
    if (!p) return;
    int sel = p->GetCurSel();
    if (sel < 0) sel = 0;
    m_cpuCap = sel * 25;
    std::wstring msg = Tr(L"CPU 不限制");
    if (m_cpuCap != 0) {
        wchar_t buf[64];
        swprintf(buf, 64, Tr(L"CPU 限制：%d%%"), m_cpuCap);
        msg = buf;
    }
    if (m_busy) {
        std::string why;
        if (!SetCpuCap(m_cpuCap, &why)) {
            LogWarn("cpu cap change failed: " + why);
            SetStatus(U2W(why));
            return;
        }
    }
    SetStatus(msg);
}

// ────────────────── 还原（UI 校验 + worker 线程暂存） ──────────────────

void CMainForm::StartRestore() {
    if (m_wimPath.empty()) { SetStatus(Tr(L"请先选择镜像文件")); return; }
    if (m_selPart < 0 || m_selPart >= (int)m_parts.size()) {
        SetStatus(Tr(L"请选择目标分区")); return;
    }
    const PartitionInfo& part = m_parts[m_selPart];
    // BitLocker 提醒（用户规格 2026-09-23）：**只要系统里有加密卷**就提醒。
    if (!AskBitLockerWarning(sysrecover::BitLockerVolumes()))
        return;
    // 目标盘健康提醒（PIT-086，用户 2026-09-26）：坏盘先警告（默认取消）。
    if (!AskDiskHealthWarning((int)part.diskIndex))
        return;
    // 非静默模式：只弹一个选择框（退出 / 退出并重启）——选定后暂存并自动重启，
    // 不再有额外的成功提示框，也不弹关机通知。静默模式：不弹框，直接暂存并重启。
    if (!IsSilent()) {
        // 提示文案必须与**真实行为**一致（PIT-072）：PE 里 / 还原到非系统盘走的是
        // "就地还原、不重启"，以前这里一律写"重启后还原"，用户 2026-09-21 在 PE 里
        // 实测被误导。判断复用 ops 层同一个函数（CanRestoreInPlace），两边不漂移。
        std::string why;
        InPlaceReason whyReason = INPLACE_OK;
        bool needReboot = !CanRestoreInPlace(part, why, &whyReason);
        // 日志落点：（用户 2026-09-23）**合并进本确认框**，不要再弹第二个 MessageBox。
        // 只在"程序在目标盘或只读盘上"时才需要提这一句。
        std::wstring logNote;
        {
            std::wstring ed = ExeDir();
            wchar_t exL = ed.empty() ? 0 : ed[0];
            wchar_t tgL = part.letter.empty() ? 0 : part.letter[0];
            wchar_t exRoot[4] = {exL, L':', L'\\', 0};
            bool onTarget = exL && tgL && towupper(exL) == towupper(tgL);
            bool fixedDisk = exL && GetDriveTypeW(exRoot) == DRIVE_FIXED;
            if (onTarget || !fixedDisk) {
                std::wstring dd = FindDataDrive();
                logNote = dd.empty() ? std::wstring()
                                     : (Tr(L"（还原日志将放到 ") + dd.substr(0, 1) + Tr(L": 盘）"));
                LogInfo("logs will be placed on data drive (exe dir is target/"
                        "read-only)");
            }
        }
        wchar_t confirm[640];
        if (needReboot) {
            // 把**判定原因**也显示出来（用户 2026-09-23：PE 里系统盘是 X:，还原 C:
            // 本不该重启 → 需要一眼看出到底卡在哪个条件）。文案保持短，避免被
            // 确认框右侧裁掉（PIT-074 的教训）。
            // 按**原因码**选文案（M1：原来 `why.find("系统盘")` 是中文子串匹配，
            // 消息一翻译就恒不成立 → 永远显示"被占用"那个分支）。
            std::wstring shortWhy =
                whyReason == INPLACE_SYSTEM_DISK
                    ? Tr(L"目标是正在运行的系统盘")
                    : Tr(L"目标分区当前被占用");
            swprintf(confirm, 640,
                     Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n" L"该分区上的所有数据将被覆盖！%ls\n" L"原因：%ls；将暂存任务并在重启后执行。"),
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber, logNote.c_str(),
                     shortWhy.c_str());
            if (CConfirmDlg::Ask2(m_hWnd, Tr(L"确认还原"), confirm, Tr(L"退出"),
                                  Tr(L"退出并重启"), /*defaultIsRight=*/true) != 1)
                return;
        } else {
            swprintf(confirm, 640,
                     Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n" L"该分区上的所有数据将被覆盖！%ls\n" L"目标分区当前未被占用，将立即就地还原，不需要重启。"),
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber, logNote.c_str());
            if (CConfirmDlg::Ask2(m_hWnd, Tr(L"确认还原"), confirm, Tr(L"取消"),
                                  Tr(L"开始还原"), /*defaultIsRight=*/true) != 1)
                return;
        }
    }
    // （日志落点的提示已**合并进上面的确认框**，不再单独弹窗 —— 用户 2026-09-23
    //   反馈连续两个提示框很烦。这里只留一行日志备查。）
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在校验镜像并暂存还原任务..."));
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
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"), MB_OK | MB_ICONERROR);
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
    // 进度：把 ops 层 apply 的进度转投到界面 —— 用户 2026-09-23 反馈"还原时进度条
    // 和百分比全程不动、最后才一下跳满"，因为这条链路以前**根本没接** ✗
    //（备份那条接了、还原这条漏了）。约定：回调返回 **true = 请求中止**。
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_lastProgressPost)
                      .count();
        // 阶段名一变就发（不受 100ms 节流）：收官标记（esp/verify/probe）与
        // 上一次上报常在 100ms 内，会被吞掉 → 状态栏永远停在旧阶段（PIT-098）。
        bool stageChanged = stage != m_lastStageRaw;
        if (ms < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                    (LPARAM)new std::wstring(U2W(stage)));
        return false;
    };
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    int rc = StageRestore(req, err, &needReboot, progressFn, &last_adv_);
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
            SetStatus(Tr(L"未找到数据盘，请用浏览指定保存位置")); return;
        }
        std::wstring dir = dataDrive + kRecoveryDir;
        CreateDirectoryW(dir.c_str(), nullptr);
        dest = dir + L"\\" + TimestampDate() +
               (m_sysDesc.shortTag.empty() ? TimestampName()
                                           : m_sysDesc.shortTag) +
               Tr(L"备份") +
               (fmt == 0 ? L".esd" : L".wim");
    }
    // 「ESP」勾选（备份专属）：只读状态，起线程前定格到 m_workerEsp
    bool esp = IsEspChecked();
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring msg = Tr(L"目标镜像文件已存在，覆盖？");
        if (MessageBoxW(m_hWnd, msg.c_str(), Tr(L"确认备份"),
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
    SetStatus(Tr(L"正在备份..."));
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
    m_workerCpuCap  = m_cpuCap;  // 起线程前定格（备份中再改走 SetCpuCap 直接生效）
    m_workerEsp     = esp;
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    try {
        m_worker = std::thread(&CMainForm::StartBackupAsync, this);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"), MB_OK | MB_ICONERROR);
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
    req.cpuCap  = m_workerCpuCap;
    req.esp     = m_workerEsp;
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;  // 取消信号
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastProgressPost).count();
        // 阶段名一变就发（不受 100ms 节流），理由见还原侧同款注释（PIT-098）。
        bool stageChanged = stage != m_lastStageRaw;
        if (elapsed < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        std::wstring* pStage = new std::wstring(U2W(stage));
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct, (LPARAM)pStage);
        return false;
    };
    std::string err;
    int rc = RunBackup(req, progressFn, err, &last_adv_);
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
        Text(_T("BrowseBtn"),   Tr(L"浏览保存位置…"));
        Vis(_T("SearchBtn"),    false);  // 搜索是还原专属（2026-09-30 用户规格）
        // 「ESP」勾选占用搜索按钮在第一步卡片里的同一位置（备份专属，2026-09-30）
        Vis(_T("EspChk"),       true);
        Text(_T("MainAction"),  Tr(L"开始备份"));
        Text(_T("NoteLabel"),   Tr(L"备份备注："));
        Vis(_T("ImageIndexBox"), false);
        Vis(_T("NoteText"),     false);
        Vis(_T("NoteInput"),    true);
        // 「格式：」文字标签已由「类型」下拉自身显示（collapsedtext），不再需要
        Vis(_T("FormatLabel"),  false);
        Vis(_T("FormatBox"),    true);
        // CPU 下拉同理：**必须先显示再选默认项** —— CComboUI::SelectItem 在
        // 条目 IsVisible()==false 时直接返回 false（容器隐藏会把子控件的
        // internVisible 一起压掉），先选后显示的话收起框会是空白的。
        Vis(_T("CpuCapBox"),    true);
        // FormatBox 的条目是 XML 里静态写的，首次显示前没有任何选中项 →
        // 收起框空白（老界面里显示「.esd（高压比省空间）」）。仅在未选中时补默认值，
        // 这样用户在两个模式间来回切换时不会丢失已选格式。
        {
            CComboUI* pFmt = static_cast<CComboUI*>(
                m_PaintManager.FindControl(_T("FormatBox")));
            if (pFmt && pFmt->GetCurSel() < 0 && pFmt->GetCount() > 0)
                pFmt->SelectItem(0, false, false);  // 程序选择：不触发 SELECTCHANGED/ITEMSELECT
            CComboUI* pCpu = static_cast<CComboUI*>(
                m_PaintManager.FindControl(_T("CpuCapBox")));
            if (pCpu && pCpu->GetCurSel() < 0 && pCpu->GetCount() > 0)
                pCpu->SelectItem(0);
            SyncFormatFromPath();  // 路径里已带 .wim/.esd 时按后缀定格式
        }
        // 镜像信息（用户规格 2026-09-21）：在文件名下方给出「日期 + 系统类型 + 备份」，
        // 例如 `20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`。
        // 只在用户尚未填写时预填，不覆盖手输内容；这个值同时会成为 WIM 里的子镜像名。
        {
            CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
            if (pNote && pNote->GetText().IsEmpty() && !m_sysDesc.full.empty()) {
                std::wstring info =
                    TimestampDate() + L" " + m_sysDesc.full + Tr(L" 备份");
                pNote->SetText(info.c_str());
            }
        }
        // 备份模式不给「清除引导项」（那是还原/清理用的，用户 2026-09-28 规格），
        // 空出来的位置换成「限制CPU」下拉（见上方 Vis(CpuCapBox)）；还原模式反过来。
        Vis(_T("RepairBootBtn"),false);
        Vis(_T("BootMenuBtn"),  true);
        Vis(_T("LogBtn"),       false);  // 「日志」按钮是还原模式专属（用户 2026-10-03 规格）
        Text(_T("PartTitle"),   Tr(L"备份源分区"));
        Text(_T("PartHint"),    Tr(L"选择要备份为镜像的源分区（移动盘已隐藏）"));
        // 第三步右侧：备份模式 = 静默模式 + CPU 下拉；引导按钮两模式共用同一位置（XML 定）。
        // ★ 2026-09-30 二次调整（用户规格）：「类型」下拉移到**第一步「浏览」右侧**
        //   （与「搜索」按钮同位置同尺寸 639,77,710,115），ESP 勾选换到第三步原
        //   「格式：」处（388,296,510,329）。第三步右侧一组：
        //   静默 248..340 / ESP 388..510 / CPU 下拉 530..612 / 菜单 618..706
        Pos(_T("Silent"),        248, 302, 340, 326);
        Pos(_T("FormatBox"),     639, 77, 710, 115);   // 第一步：类型下拉
        Pos(_T("EspChk"),        388, 296, 510, 329);  // 第三步：ESP 勾选
        // 注：状态栏在空闲时显示**当前模式说明**（见本函数末尾），任务执行中
        // 显示进度文字；进度条只在跑进度时出现（ShowProgress）。
    } else {
        Text(_T("BrowseBtn"),   Tr(L"浏览系统镜像"));
        Vis(_T("SearchBtn"),    true);
        Vis(_T("EspChk"),       false);  // 还原模式没有 ESP 勾选（ESP 子镜像由救援层自动处理）
        Text(_T("MainAction"),  Tr(L"开始恢复"));
        Text(_T("NoteLabel"),   Tr(L"镜像说明："));
        Vis(_T("ImageIndexBox"), true);
        Vis(_T("NoteText"),     true);
        Vis(_T("NoteInput"),    false);
        Vis(_T("FormatLabel"),  false);
        Vis(_T("FormatBox"),    false);
        Vis(_T("CpuCapBox"),    false);
        Vis(_T("RepairBootBtn"),true);
        Vis(_T("BootMenuBtn"),  true);
        Vis(_T("LogBtn"),       true);
        Text(_T("PartTitle"),   Tr(L"系统安装位置"));
        Text(_T("PartHint"),    Tr(L"选择要安装恢复镜像的目标分区（移动盘已隐藏）"));
        // 还原模式几何（切回时必须复位第三步右侧那几个）。2026-10-03 用户规格：
        // 「日志」按钮插在 静默模式 与 清除引导 之间 → 整组左移给按钮腾位：
        //   静默 360..460 / 日志 466..524 / 清除引导 530..612 / 菜单 618..706。
        Pos(_T("Silent"),        360, 302, 460, 326);
        Pos(_T("LogBtn"),        466, 296, 524, 329);
        Pos(_T("FormatLabel"),   350, 302, 388, 326);
        Pos(_T("FormatBox"),     388, 296, 510, 329);
    }
    UpdateMainAction();
    // 空闲时状态栏 = **当前模式说明**（用户 2026-09-27 规格）：
    //   · 鼠标悬停走 tooltip，键盘/不悬停的人看这一行 —— 两层解释并存；
    //   · 只在切 Tab 时写一次，切到另一个模式自然被新的一条替换；
    //   · 期间任何 SetStatus（选文件/报错/进度）都会盖掉它，再切回来又恢复
    //     —— 没有定时器、没有“谁优先”，正好是规格里“切换即替换”的语义。
    // 放在 UpdateMainAction() **之后**：那里面在按钮不可用时会写
    // 「请先选择镜像文件」之类的提示，会把说明行冲掉。
    SetStatus(Tr(m_backupMode
                     ? L"把当前系统热备份为镜像文件（VSS 快照，可在运行中备份）"
                     : L"选择镜像与目标分区后开始；还原系统盘会重启进恢复环境，非系统盘就地还原"));
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
            SetStatus(m_backupMode ? Tr(L"请先选择保存位置") : Tr(L"请先选择镜像文件"));
        else if (m_selPart < 0)
            SetStatus(m_backupMode ? Tr(L"请选择第二步的源分区")
                                   : Tr(L"请选择第二步的目标分区"));
        else if (!m_backupMode && !m_imageOk)
            SetStatus(Tr(L"镜像不可用（解析失败或文件不存在）"));
    }
    // 「安装菜单」按钮的状态随"是否已选镜像/目标分区"变（用户 2026-09-29 规格：
    // 没选镜像时置灰）；已安装时它显示「删除菜单」并说明绑定关系。
    RefreshBootMenuBtn();
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
        ImagePathReady(m_wimPath)) {
        m_lastLoadedWim = m_wimPath;
        LoadWimImages(m_wimPath);
    }
    SyncFormatFromPath();  // 备份：路径带 .wim/.esd → 自动定格式
    UpdateMainAction();
}

void CMainForm::RebootNow() {
    // 不用 shutdown.exe：带 /t 时它会弹「Windows 将在一分钟后关闭」对话框
    // （用户要求关机前不要再有任何提示）。改用 ExitWindowsEx 直接触发重启，
    // 需先启用 SE_SHUTDOWN_NAME 特权（管理员进程本就有）。
    // R4（用户 2026-10-04）：两条路都**记返回码**——"说重启却没动"必须有账可查。
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
        } else {
            LogWarn("reboot: LookupPrivilegeValue(SE_SHUTDOWN_NAME) failed err=" +
                    std::to_string(::GetLastError()));
        }
        ::CloseHandle(hToken);
    } else {
        LogWarn("reboot: OpenProcessToken failed err=" +
                std::to_string(::GetLastError()));
    }
    if (!::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG,
                         SHTDN_REASON_MAJOR_APPLICATION |
                             SHTDN_REASON_MINOR_MAINTENANCE |
                             SHTDN_REASON_FLAG_PLANNED)) {
        // 兜底：ExitWindowsEx 因会话/策略失败时，用 /t 0 立即重启
        // （无延迟则 shutdown.exe 不弹「一分钟后关闭」对话框）
        DWORD e1 = ::GetLastError();
        std::string out;
        int rc = RunProcess(SysToolPath(L"shutdown.exe"), L"/r /t 0", out);
        LogWarn("reboot: ExitWindowsEx failed err=" + std::to_string(e1) +
                "; shutdown.exe rc=" + std::to_string(rc) + " out=" + out);
    } else {
        LogInfo("reboot: ExitWindowsEx ok");
    }
}

bool CMainForm::IsSilent() {
    COptionUI* p =
        static_cast<COptionUI*>(m_PaintManager.FindControl(_T("Silent")));
    return p && p->IsSelected();
}

bool CMainForm::IsEspChecked() {
    COptionUI* p =
        static_cast<COptionUI*>(m_PaintManager.FindControl(_T("EspChk")));
    return p && p->IsSelected();
}

// 状态文字的可用宽度：进度条只在任务跑进度时出现（见 ShowProgress），
// 静止时可以一直写到右下角「讨论」链接前面（约 638px）；进度条一出现就
// 必须收在它左边（179px）。
// 超宽一律「掐头 + … + 留尾」—— 尾部多是时间 / 文件名 / 错误码，信息价值
// 最高；这样任意长的路径和英文长句都不会压到进度条上（英文状态消息最长
// 453px，是中文的 1.5 倍，以前直接画穿进度条）。
static std::wstring FitStatusLine(CPaintManagerUI& pm, const std::wstring& s) {
    CControlUI* pStatus = pm.FindControl(_T("StatusText"));
    if (!pStatus) return s;
    RECT rc = pStatus->GetPos();
    int right = rc.right;
    CControlUI* pProg = pm.FindControl(_T("Progress"));
    if (pProg && pProg->IsVisible()) {
        right = pProg->GetPos().left - 4;
    } else {
        CControlUI* pLink = pm.FindControl(_T("SiteLink"));
        if (pLink) right = pLink->GetPos().left - 8;
    }
    int avail = right - rc.left;
    if (avail <= 16) return s;
    const int px = 13;  // StatusText 的 fontsize（skin/main.xml）
    HDC hdc = ::GetDC(nullptr);
    if (!hdc) return s;
    std::wstring out = s;
    if (ui_skin::TextW(hdc, s.c_str(), px, false) > avail) {
        const std::wstring ell = L"\x2026";  // …
        const size_t n = s.size();
        const size_t tailLen = (n > 8) ? 8 : 0;
        const std::wstring tail = s.substr(n - tailLen);
        size_t lo = 0, hi = n - tailLen;
        while (lo < hi) {  // 二分：前缀 + … + 尾部 尽量长
            size_t mid = (lo + hi + 1) / 2;
            std::wstring cand = s.substr(0, mid) + ell + tail;
            if (ui_skin::TextW(hdc, cand.c_str(), px, false) <= avail) lo = mid;
            else hi = mid - 1;
        }
        out = s.substr(0, lo) + ell + tail;
        if (ui_skin::TextW(hdc, out.c_str(), px, false) > avail) {  // 尾部本身就放不下
            out = ell;
            for (wchar_t c : s) {
                std::wstring t = out + c;
                if (ui_skin::TextW(hdc, t.c_str(), px, false) > avail) break;
                out.swap(t);
            }
        }
    }
    ::ReleaseDC(nullptr, hdc);
    return out;
}

void CMainForm::SetStatus(const std::wstring& text) {
    // StatusText 是自绘的 CSkinLabelUI（不是 CLabelUI）
    m_lastStatus = text;
    CControlUI* p = m_PaintManager.FindControl(_T("StatusText"));
    if (p) p->SetText(FitStatusLine(m_PaintManager, text).c_str());
    LogInfo(std::string("GUI status: ") + W2U(text));
}

void CMainForm::ShowProgress(bool show) {
    for (const TCHAR* name : {_T("Progress"), _T("PercentText")}) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p && p->IsVisible() != show) p->SetVisible(show);
    }
    // 进度条显隐会改变状态文字的可用宽度 → 把已显示的那条按新宽度重新收放
    if (!m_lastStatus.empty()) {
        CControlUI* p = m_PaintManager.FindControl(_T("StatusText"));
        if (p) p->SetText(FitStatusLine(m_PaintManager, m_lastStatus).c_str());
    }
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
