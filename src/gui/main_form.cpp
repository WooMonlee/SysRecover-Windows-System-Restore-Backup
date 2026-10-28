// 主窗口实现。Phase 5：CLI 逻辑经 app/ops 共享层桥接到 GUI。
// 注意：所有 C++ 标准头已通过 main_form.h 在 StdAfx.h 之前 include（PIT-012）。
#include "common/i18n.h"
#include "main_form.h"

#include <shobjidl.h>
#include <shellapi.h>

#include "gui/ui_skin.h"
#include "gui/confirm_dlg.h"
#include "gui/passwd_dlg.h"
#include "gui/imgsearch.h"
#include "app/ops.h"
#include "app/imgsrc.h"
#include "app/universal.h"
#include "app/safety.h"
#include "app/selfdiag.h"
#include "app/swapleft.h"
#include "boot/bcd.h"
#include "boot/grub.h"
#include "boot/uefi.h"
#include "common/bioskey.h"
#include "common/cpucap.h"
#include "common/logger.h"
#include "common/process.h"
#include "common/relocate.h"
#include "common/sysinfo.h"
#include "common/progress.h"
#include "disk/disk.h"
#include "disk/partinit.h"
#include "gui/initdisk_dlg.h"
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
    return eq(L".wim") || eq(L".esd") || eq(L".swm") || eq(L".zje");
}

// 镜像路径是否"值得尝试打开"。
// ⚠️ 不要只用 GetFileAttributesW 预筛：GUI 带 requireAdministrator 清单（提权进程），
// 受 **UAC 会话隔离**影响，提权后的登录会话对 UNC 路径（\\server\share\...）常拿不到
// 属性 → 返回 INVALID → 以前直接跳过 LoadWimImages → 主按钮**一直灰且无任何报错**
// （用户 2026-09-23 实测：本地路径能变蓝，网络/UNC 路径不变蓝）。改为：只要路径
// 像镜像文件就交给 wimlib 去开 —— 开得了按钮变蓝，开不了也会给出明确错误。
bool ImagePathReady(const std::wstring& p) {
    // ⚠️ 目录不是镜像：UNC 共享根（\\server\share\）在输入过程中会先变成
    // 合法目录，若当成镜像去开，会同步阻塞并触发文本同步/光标跳动
    //（2026-10-21 VM 实测：输入汉字跑到最前面）。
    const DWORD a = ::GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return LooksLikeImagePath(p);
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
    // 启动后置工作线程（问题 2）：它只做 diag/收集/枚举，无取消位，等它收手
    //（正常毫秒级；插了慢速 USB2 盘时最坏等于原来 InitWindow 阻塞的那段）。
    if (m_startWorker.joinable()) m_startWorker.join();
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
    //（还原/重装系统盘后日志仍留存）。LogInit 必须在第一条 StartupPhase 之前
    //（阶段耗时日志靠它落盘）。
    std::wstring logsDir = sysrecover::LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    LogInit(logsDir);
    ProgressInit(logsDir);
    // 每次启动都归纳日志（PIT-156，2026-10-18 起不再只限备返克隆首启）：先把
    // 救援层/旧版本留在数据盘上的日志搬进日志根，随后 StartupLazyWork 的收集器
    // 才收 `source.txt` 等痕迹、清理器才能**整个**删掉还原残留的 C:\ZJRESTORE
    // 壳（分类保护程序的家/启动菜单，见 relocate.cpp::ClassifyZjrResidue）。
    // 必须放在 StartupLazyWork（收痕迹 + 清散件）**之前** —— 否则收集器先收
    // 进 logs\collected 并清掉源目录，搬运"无源可搬"（顺序由 WM_STARTUP_LAZY
    // 在消息循环里才被处理保证）。守卫（PE / 暂存待还原）见 relocate.cpp。
    // 失败只记日志不挡流程（拿不到日志也只是日志少几行）。
    {
        const std::string mig = sysrecover::MigrateLogsToLogHome();
        if (!mig.empty()) sysrecover::LogInfo(mig);
    }
    sysrecover::StartupPhase("InitWindow");
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
    // ⚠️ 问题 2（用户 2026-10-12 规格："插了 USB2 盘要等 20 秒才出界面，
    // 检查请挪到界面显示之后"）：**这里不再做任何可能被慢盘卡住的事** ——
    // 写 diag/list、收部署痕迹、清理散件、枚举磁盘全部搬到界面显示之后的
    // 后台线程（WM_STARTUP_LAZY），只留"立即出界面"必需的初始化。
    // 分区下拉在后台枚举完成前显示占位文案（下面 WM_PARTS_READY 回填）。
    if (CControlUI* pLoad = m_PaintManager.FindControl(_T("PartLoadingText")))
        pLoad->SetVisible(true);
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
    // 救援层失败回执检查（用户 2026-10-04 规格 2）：**等后台收集跑完再查**
    //（它要扫 logs\collected 里的回执，早了会漏）—— 见 StartupLazyWork 末尾。
    sysrecover::StartupPhase("InitWindow-ui");
    ::PostMessage(m_hWnd, WM_STARTUP_LAZY, 0, 0);
}

// ────────────────── 启动后置工作（问题 2，2026-10-12）──────────────────
// 用户规格：插着 USB2 盘时启动要等 20 秒才出界面 → 把 diag/list 写入、部署
// 痕迹收集、散件清理、磁盘枚举**全部挪到界面显示之后**，且在后台线程做。
// 界面侧只做"必须立刻有"的事；分区下拉期间显示 PartLoadingText 占位。
void CMainForm::StartupLazyWork() {
    const std::wstring logsDir = sysrecover::LogBaseDir() + L"\\logs";
    // 磁盘表**只枚举一次**，diag/list/收集/填下拉共用 —— 一次枚举要对每个
    // PhysicalDriveN 开句柄发 IOCTL，慢盘上每次都能卡几秒，过去这里
    // diag(1) + list(1) + 收集(每盘符 1) + 填下拉(1) 各来一遍。
    sysrecover::StartupPhase("lazy-start");
    std::vector<DiskInfo> disks = sysrecover::EnumerateDisks();
    sysrecover::StartupPhase("enumerate-disks");  // 问题 2 的关键计时点
    sysrecover::WriteDiagFiles(logsDir, &disks);
    sysrecover::CollectDeployArtifacts(logsDir + L"\\collected", 0, &disks);
    sysrecover::StartupPhase("diag+collect");
    // PIT-137（用户 2026-10-08 规格）：每次运行"收完即清"——黑匣子已进
    // logs\collected，各盘根的散件立即清掉。契约/引导文件不动。
    sysrecover::CleanupStrayLogs();
    sysrecover::StartupPhase("cleanup-stray");
    // 回主线程填分区下拉（lParam 所有权交给 handler，由它 delete）。
    auto* out = new std::vector<DiskInfo>(std::move(disks));
    if (!::PostMessage(m_hWnd, WM_PARTS_READY, 0, (LPARAM)out)) {
        delete out;
        return;
    }
    // 收集完成后再查救援层失败回执（依赖 logs\collected）。
    ::PostMessage(m_hWnd, WM_CHECK_RESCUE, 0, 0);
}

// ────────────────── pre-message filter：Ctrl+Tab 切模式 ──────────────────
// WindowImplBase::OnCreate 已把 this 注册为 pre-message filter
//（WinImplBase.cpp:275），虚分派先到本覆写；这里只新增拦 Ctrl+Tab，
// 其余原样转交基类（保持 ESC 关窗等既有行为）。
// 裸 Tab 不拦（保持焦点循环）；忙碌时只吞不切 —— Notify 对忙时
// SELECTCHANGED 早退（main_form.cpp:737），切了会造成「Tab 变了、模式没变」。
LRESULT CMainForm::MessageHandler(UINT uMsg, WPARAM wParam, LPARAM lParam,
                                  bool& bHandled) {
    bHandled = false;
    if (uMsg == WM_KEYDOWN && wParam == VK_TAB &&
        (::GetKeyState(VK_CONTROL) & 0x8000)) {
        bHandled = true;  // 吞掉 → PreMessageHandler 提前返回，不再 SetNextTabControl
        if (!m_busy) {
            CControlUI* p = m_PaintManager.FindControl(
                m_backupMode ? _T("ModeRestore") : _T("ModeBackup"));
            if (p)
                static_cast<COptionUI*>(p)->Selected(true);  // → SELECTCHANGED → ApplyModeUi
        }
        return 0;
    }
    return WindowImplBase::MessageHandler(uMsg, wParam, lParam, bHandled);
}

// ────────────────── HandleMessage（worker 线程安全回调） ──────────────────

LRESULT CMainForm::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 回车不再触发「开始」（用户 2026-10-11 二轮规格，替代 2026-10-10 的
    // 「全局回车 = 主按钮」）：开始改走 **Alt+E**（main.xml MainAction
    // shortcut="E"）。这里只在「焦点恰好是主按钮」时吞掉回车 —— 否则
    // CButtonUI::DoEvent 对 VK_RETURN 会 Activate，回车仍能误启备份/还原；
    // 焦点在别处时回车交给标准控件语义（如 TAB 到「删除菜单」按回车 =
    // 点击它，且有确认框兜底）。焦点在原生 EDIT 子窗口时 WM_KEYDOWN 直接
    // 发给子窗口、根本到不了这里（天然只在非输入文本时生效）。
    if (msg == WM_KEYDOWN && wParam == VK_RETURN) {
        CControlUI* pFocus = m_PaintManager.GetFocus();
        if (pFocus && _tcscmp(pFocus->GetName(), _T("MainAction")) == 0)
            return 0;
    }
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
            if (p.empty() && !hNative)
                return 0;
            std::wstring& active = ActivePath();
            if (p != active) {
                active = p;
                if (!m_backupMode && !active.empty() &&
                    active != m_lastLoadedWim && ImagePathReady(active)) {
                    m_lastLoadedWim = active;
                    LoadWimImages(active);
                }
                SyncFormatFromPath();  // 备份：路径带 .wim/.esd 时自动定格
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
    if (msg == WM_TIMER && wParam == 3) {
        if (m_postBusy) {
            m_postPulse = (m_postPulse + 5) % 100;
            CRoundProgressUI* p = static_cast<CRoundProgressUI*>(
                m_PaintManager.FindControl(_T("PostProgress")));
            if (p) p->SetValue(m_postPulse);
        }
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
    if (msg == WM_STARTUP_LAZY) {
        // 起后台线程做启动后置工作（diag/收集/清理/枚举 —— 可能被慢速 USB2
        // 盘卡住，故绝不能在 InitWindow 里同步跑，见 StartupLazyWork）。
        if (!m_startWorker.joinable())
            m_startWorker = std::thread([this] { StartupLazyWork(); });
        return 0;
    }
    if (msg == WM_PARTS_READY) {
        // 后台枚举完成 → 回主线程填分区下拉（handler 接管指针所有权）。
        auto* disks = reinterpret_cast<std::vector<DiskInfo>*>(lParam);
        if (disks) {
            PopulatePartitions(*disks);
            delete disks;
        }
        return 0;
    }
    if (msg == WM_RETRY_DONE) {
        // P3（PIT-148）一键重试完成：成功且需重启 → 自动 shutdown（与正常
        // 还原暂存一致，PIT-110）；失败 → 弹错误（静默模式也弹，fail-safe）。
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        UpdateMainAction();
        int rc = (int)wParam;
        bool reboot = lParam != 0;
        if (rc == 0) {
            SetStatus(reboot ? Tr(L"重试暂存完成，正在重启...")
                             : Tr(L"重试完成，无需重启"));
            if (reboot)
                RebootNow();
            else
                MessageBoxW(m_hWnd, Tr(L"重试完成。重启后即可进入恢复的系统。"),
                            Tr(L"一键重试"), MB_OK | MB_ICONINFORMATION);
        } else {
            std::wstring e = m_retryErr.empty() ? Tr(L"重试失败（详见日志）")
                                                : U2W(m_retryErr);
            SetStatus(Tr(L"重试失败"));
            MessageBoxW(m_hWnd, e.c_str(), Tr(L"一键重试"),
                        MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    if (msg == WM_MENU_DONE) {
        // 装/删菜单 worker 完成（2026-10-18）：wParam=rc, lParam=install(0/1)。
        OnMenuDone((int)wParam, lParam != 0);
        return 0;
    }
    if (msg == WM_MENU_SCAN_DONE) {
        // 「安装到启动」扫描完成（2026-10-28）：wParam=1 表示可在救援层直接还原。
        OnMenuScanDone(wParam != 0);
        return 0;
    }
    if (msg == WM_SKIN_EMPTY_PARTITION) {
        // 分区下拉为空时被点击（2026-10-20）：直接打开发分区向导。
        if (!m_busy && !m_backupMode) OpenInitDiskWizard();
        return 0;
    }
    if (msg == WM_PART_INIT_DONE) {
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        if (m_worker.joinable()) m_worker.join();
        const int rc = (int)wParam;
        if (rc == 0) {
            PopulatePartitions(EnumerateDisks());
            SetStatus(Tr(L"新硬盘分区已创建，请选择镜像后开始还原"));
            MessageBoxW(m_hWnd,
                        Tr(L"分区已创建完成。\n请选择系统镜像，再选择新的系统分区开始还原。"),
                        Tr(L"创建分区"), MB_OK | MB_ICONINFORMATION);
        } else {
            SetStatus(Tr(L"创建分区失败"));
            MessageBoxW(m_hWnd,
                        m_initErr.empty() ? Tr(L"创建分区失败，请打开日志查看详情。")
                                          : m_initErr.c_str(),
                        Tr(L"创建分区"), MB_OK | MB_ICONERROR);
        }
        UpdateMainAction();
        return 0;
    }
    if (msg == WM_FIXBOOT_DONE) {
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        if (m_worker.joinable()) m_worker.join();
        const int rc = (int)wParam;
        LogInfo("GUI repair-boot done rc=" + std::to_string(rc) + " / " + m_fixMsg);
        SetStatus(rc == 0 ? Tr(L"引导已修复") : Tr(L"修复引导未完全成功"));
        // 2026-10-28（用户规格）：**被安全软件拦写 → 弹框让用户退出防护后点「重试」**，
        // 不再让用户自己猜"为什么没修好"。重试直接开跑（不再重复确认框）。
        if (m_fixBlocked &&
            CConfirmDlg::Ask2(
                m_hWnd, Tr(L"修复引导未完成"),
                Tr(L"写入启动项/引导区被安全软件拦截\n（如火绒/360 的「写磁盘保留扇区」）。\n请退出该防护或把本程序加入信任，再点「重试」。"),
                Tr(L"关闭"), Tr(L"重试"), /*defaultIsRight=*/true) == 1) {
            UpdateMainAction();
            StartRepairBoot(/*confirm=*/false);
            return 0;
        }
        std::wstring text = m_fixMsg.empty() ? Tr(L"修复引导失败，请打开日志查看详情。")
                                             : U2W(m_fixMsg);
        MessageBoxW(m_hWnd, text.c_str(), Tr(L"修复引导"),
                    rc == 0 ? (MB_OK | MB_ICONINFORMATION)
                            : (MB_OK | MB_ICONWARNING));
        UpdateMainAction();
        return 0;
    }
    if (msg == WM_VERIFY_DONE) {
        // 「镜像检查」worker 完成（2026-10-27）：wParam=rc(0=通过)。
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        if (m_worker.joinable()) m_worker.join();
        const int rc = (int)wParam;
        if (m_cancel) {
            SetStatus(Tr(L"镜像检查已取消"));
            UpdateMainAction();
            return 0;
        }
        if (rc == 0) {
            SetStatus(Tr(L"镜像校验通过"));
            MessageBoxW(m_hWnd, Tr(L"镜像校验通过：文件完整，可以放心还原。"),
                        Tr(L"镜像检查"), MB_OK | MB_ICONINFORMATION);
        } else {
            SetStatus(Tr(L"镜像校验失败"));
            std::wstring e = m_verifyMsg.empty() ? Tr(L"镜像校验失败，请打开日志查看详情。")
                                                 : U2W(m_verifyMsg);
            MessageBoxW(m_hWnd, e.c_str(), Tr(L"镜像检查"),
                        MB_OK | MB_ICONERROR);
        }
        UpdateMainAction();
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
    // 异机还原（0.9.33）：apply 100% 后的驱动注入阶段，状态栏必须继续报活。
    if (s.compare(0, 9, L"universal") == 0) {
        if (s.find(L"inbox-done") != std::wstring::npos)
            return Tr(L"内置驱动已启用");
        if (s.find(L"inbox") != std::wstring::npos)
            return Tr(L"正在启用内置存储驱动");
        if (s.find(L"drivers") != std::wstring::npos)
            return Tr(L"正在注入驱动包");
        if (s.find(L"done") != std::wstring::npos)
            return Tr(L"驱动注入完成");
        return Tr(L"正在准备异机还原");
    }
    // 引导层部署（2026-10-18）：ops 在 apply 后上报的收官阶段（暂存与菜单
    // 安装都经过；菜单安装的"无响应窗"正是发生在这里的 28MB 载荷写入）。
    if (s.compare(0, 4, L"boot") == 0) return Tr(L"写入引导文件");
    // 新硬盘分区向导（2026-10-27 用户规格：每一步都要看得见，不能只写一句
    // "正在创建分区"——diskpart 建分区+格式化能跑几十秒）。阶段名来自
    // partscript.cpp：每一步**一次独立 diskpart 调用**，见 BuildDiskpartSteps。
    // 状态栏可用宽度只有约 179px（进度条一出现就收窄），所以每条压到 6~8 字。
    if (s.compare(0, 8, L"partinit") == 0) {
        if (s.find(L"letters") != std::wstring::npos) return Tr(L"正在分配盘符");
        if (s.find(L"script") != std::wstring::npos) return Tr(L"正在生成脚本");
        if (s.find(L"clean") != std::wstring::npos) return Tr(L"正在清除旧分区");
        if (s.find(L"convert") != std::wstring::npos) return Tr(L"正在转换分区表");
        if (s.find(L"esp") != std::wstring::npos) return Tr(L"正在创建ESP");
        if (s.find(L"msr") != std::wstring::npos) return Tr(L"正在创建MSR");
        if (s.find(L"system") != std::wstring::npos) return Tr(L"正在创建系统盘");
        if (s.find(L"data") != std::wstring::npos) return Tr(L"正在创建数据盘");
        if (s.find(L"bootsect") != std::wstring::npos) return Tr(L"正在写入引导代码");
        if (s.find(L"verify") != std::wstring::npos) return Tr(L"正在校验分区");
        if (s.find(L"done") != std::wstring::npos) return Tr(L"分区创建完成");
        return Tr(L"正在创建分区");
    }
    return s;
}

void CMainForm::OnProgressUpdate(int pct, const std::wstring& stage) {
    // 进度条随**第一条进度**出现（不是点按钮那一刻）：校验/暂存阶段没有进度，
    // 整行状态栏留给状态文字；真开始跑进度了，文字才收在进度条左边。
    ShowProgress(true);
    const bool universal =
        stage.compare(0, 9, L"universal") == 0;
    if (universal) {
        // 主进度已走完（100%），这里不再把它打回 0；驱动注入耗时另用
        // PostProgress 小进度条循环提示"仍在工作"。
        SetProgress(100);
        const bool done = stage.find(L"done") != std::wstring::npos;
        ShowPostProgress(!done);
    } else {
        ShowPostProgress(false);
        SetProgress(pct);
    }
    m_lastStage = StageCn(stage);
    SetStatus(m_lastStage + Tr(L"    已用 ") + ElapsedText());
    CControlUI* pPct = m_PaintManager.FindControl(_T("PercentText"));
    if (pPct) {
        wchar_t b[16];
        swprintf(b, 16, L"%d%%", universal ? 100 : pct);
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
        m_imageOk = true;  // 备份完成即写盘有效；切到还原页后按钮可以点亮
        // 增量备份：补一句"现在是第 N 个时间点，合计 X"（用户 2026-10-28 规格）。
        std::wstring tp;
        if (m_workerAppend && m_backupTimepoints > 0) {
            wchar_t buf[256] = {};
            swprintf(buf, 256,
                     Tr(L"\n现在是第 %d 个时间点，合计 %.1f GB。"),
                     m_backupTimepoints,
                     static_cast<double>(m_backupTotalBytes) /
                         (1024.0 * 1024.0 * 1024.0));
            tp = buf;
        }
        if (!m_skippedFiles.empty()) {
            const int n = static_cast<int>(m_skippedFiles.size());
            wchar_t head[256];
            swprintf(head, 256,
                     Tr(L"备份已完成，但有 %d 个文件未能读取，未写入镜像：\n\n"),
                     n);
            std::wstring msg = head;
            const int shown = n < 20 ? n : 20;
            for (int i = 0; i < shown; ++i) {
                msg += m_skippedFiles[static_cast<size_t>(i)];
                msg += L"\n";
            }
            if (n > shown) {
                wchar_t more[256];
                swprintf(more, 256,
                         Tr(L"\n… 另有 %d 个文件，完整清单见日志：\n"),
                         n - shown);
                msg += more;
            } else {
                msg += Tr(L"\n完整清单见日志：\n");
            }
            msg += LogBaseDir() + L"\\logs";
            wchar_t status[256];
            swprintf(status, 256, Tr(L"备份完成，但有 %d 个文件未备份"),
                     n);
            SetStatus(std::wstring(status) + tp);
            MessageBoxW(m_hWnd, (msg + tp).c_str(), Tr(L"备份不完整"),
                        MB_OK | MB_ICONWARNING);
        } else {
            SetStatus(Tr(L"备份完成，用时 ") + ElapsedText() + Tr(L"。"));
            if (!IsSilent())
                MessageBoxW(m_hWnd,
                            (Tr(L"系统备份已完成。\n用时 ") + ElapsedText() +
                             Tr(L"秒") + tp)
                                .c_str(),
                            Tr(L"备份成功"), MB_OK | MB_ICONINFORMATION);
        }
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
        // Q1 settle 警告（2026-10-18）：程序目录在目标盘上但没找到安全分区
        // → 还原后程序可能被格式化掉。弹框让用户知道（fail-safe，静默模式也弹）。
        {
            const auto& sr = sysrecover::LastSettleResult();
            if (sr.noSafeDrive)
                MessageBoxW(m_hWnd,
                            Tr(L"警告：程序目录在目标盘上且无法安全搬迁——还原后程序可能丢失。"),
                            Tr(L"警告"), MB_OK | MB_ICONWARNING);
        }
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
// P3（PIT-148，用户 2026-10-14）：A/B 类失败（未动目标/引导未验证）→ 三选框
// 「知道了 / 打开日志 / 一键重试」；C 类（已动盘）保持原二选框（只给日志路径）。
void CMainForm::ShowRescueFailureNotice() {
    RescueNotice n = CheckLastRescueFailure();
    if (n.msg.empty())
        return;
    size_t nl = n.msg.find(L'\n');
    SetStatus(nl == std::wstring::npos ? n.msg : n.msg.substr(0, nl));
    int choice;
    if (n.retryable) {
        // 左=知道了（安全默认）、中=打开日志、右=一键重试；回车/ESC 落"知道了"
        //（Ask3 的 defaultIdx=0 = 左按钮，见 confirm_dlg.h）。
        choice = CConfirmDlg::Ask3(
            m_hWnd, Tr(L"还原结果"), n.msg, Tr(L"知道了"), Tr(L"打开日志"),
            Tr(L"一键重试"), /*defaultIdx=*/0);
    } else {
        choice = CConfirmDlg::Ask2(m_hWnd, Tr(L"还原结果"), n.msg,
                                   Tr(L"知道了"), Tr(L"打开日志文件夹"),
                                   /*defaultIsRight=*/false);
        if (choice == 1) {
            std::wstring dir = LogBaseDir() + L"\\logs\\collected";
            ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                            SW_SHOWNORMAL);
        }
        return;
    }
    if (choice == 1) {  // 打开日志
        std::wstring dir = LogBaseDir() + L"\\logs\\collected";
        ::ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    } else if (choice == 2) {  // 一键重试
        // 二次确认（fail-safe）：重试会重新暂存 + 可能重启。
        std::wstring cfm = std::wstring(
                               Tr(L"将按上次任务参数重新执行还原（镜像→目标分区）。\n")) +
                           Tr(L"确认继续？");
        int r = CConfirmDlg::Ask2(m_hWnd, Tr(L"一键重试"), cfm, Tr(L"取消"),
                                  Tr(L"立即重试"),
                                  /*defaultIsRight=*/false);  // 默认取消=安全
        if (r == 1)
            StartRetryStagedRestore();
    }
}

// 一键重试（P3 / PIT-148）：读 conf → StageRestore；成功后若需重启则自动
// shutdown /r（与正常还原暂存一致）。worker 用 BELOW_NORMAL，与既有 worker
// 同款（PIT-071：别把 UI 饿死）。
void CMainForm::StartRetryStagedRestore() {
    if (m_busy)
        return;
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在重试..."));
    SetProgress(0);
    ShowProgress(true);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo("GUI retry start");
    if (m_worker.joinable())
        m_worker.join();
    m_cancel = false;
    m_retryErr.clear();
    m_worker = std::thread([this] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool reboot = false;
        std::string err;
        int rc = RetryStagedRestore(err, &reboot);
        m_retryErr = err;
        last_err_ = err;
        PostMessage(WM_RETRY_DONE, (WPARAM)rc, (LPARAM)(reboot ? 1 : 0));
    });
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
                              m_workerDest + L".stage.tmp",
                              // 加密备份的明文中转（绝不能留下明文）
                              m_workerDest + L".plain.tmp",
                              m_workerDest + L".plain.tmp.stage",
                              m_workerDest + L".plain.tmp.stage.tmp"})
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
                            m_workerDest + L".stage.tmp",
                            // 加密备份的明文中转（绝不能留下明文）
                            m_workerDest + L".plain.tmp",
                            m_workerDest + L".plain.tmp.stage",
                            m_workerDest + L".plain.tmp.stage.tmp"}) {
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
    const bool silentClick = msg.sType == DUI_MSGTYPE_CLICK && msg.pSender &&
                             msg.pSender->GetName() == _T("Silent");
    const bool windowBtn = msg.sType == DUI_MSGTYPE_CLICK && msg.pSender &&
                           (msg.pSender->GetName() == _T("MinBtn") ||
                            msg.pSender->GetName() == _T("MaxBtn"));
    if (m_busy && !cpuSel && !silentClick && !windowBtn)
        return;  // 任务执行中忽略其他点击
    if (msg.sType == DUI_MSGTYPE_CLICK) {
        CDuiString name = msg.pSender->GetName();
        if (name == _T("BrowseBtn")) {
            if (m_backupMode) BrowseSaveFile(); else BrowseWimFile();
        } else if (name == _T("SearchBtn")) {
            OnSearchImages();  // 还原模式专属（备份模式按钮隐藏）
        } else if (name == _T("MainAction")) {
            if (m_backupMode) StartBackup(); else StartRestore();
        } else if (name == _T("FixBootBtn")) {
            // 皮肤里已隐藏（2026-10-27 收进「更多功能>>」）；保留路由给旧皮肤。
            StartRepairBoot();
        } else if (name == _T("MoreBtn")) {
            ShowMoreMenu();   // 「更多功能>>」下拉（异机还原/修复引导/清除菜单/镜像检查）
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
                if (bak != m_backupMode) {
                    m_backupMode = bak;
                    ApplyModeUi();
                    LogInfo(std::string("GUI mode: ") +
                            (bak ? "backup" : "restore") +
                            " path=" + W2U(ActivePath()));
                }
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
            // 用户 2026-09-30 规格：换「类型」→ **同步改文件名后缀**。
            // 坑：Combo 发出的是 DUI_MSGTYPE_ITEMSELECT，**不是** SELECTCHANGED——
            // 之前挂在 SELECTCHANGED 分支上，所以点了「类型」文件名后缀不变。
            SyncPathExtFromFormat();
        }
    } else if (msg.sType == DUI_MSGTYPE_TEXTCHANGED) {
        if (msg.pSender && msg.pSender->GetName() == _T("ImagePath"))
            SyncImagePathFromUi();
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
        // 已安装（2026-10-27 用户规格）：按钮**只负责安装**，已装即置灰；删除入口
        // 唯一化到「更多功能>>」→「清除启动中还原菜单」（避免同一件事两个入口，
        // 也避免按钮在两种语义间跳字）。悬停仍说清"绑的是哪个镜像 → 哪个分区"。
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
            if (uefiPath) {
                // 开机键按品牌/机型识别（bioskey：表移植自 BiosKC，2026-10-18）；
                // 识别不到用中性措辞，绝不写死 F12（HP 是 F9，见客户反馈）。
                swprintf(tip, 512,
                         Tr(L"已安装：开机按 %ls 选「SysRecover」，\n把 %ls（第 %d 个镜像）还原到 %ls。\n要清除请用「更多功能>>」→「清除启动中还原菜单」\n镜像请勿移动或删除，否则该菜单无法还原"),
                         BootMenuKeyHint().c_str(), boundImg.c_str(), boundIdx,
                         tgt.c_str());
            } else
                swprintf(tip, 512,
                         Tr(L"已安装：开机菜单里选「SysRecover」即可把\n%ls（第 %d 个镜像）还原到 %ls。\n要清除请用「更多功能>>」→「清除启动中还原菜单」\n镜像请勿移动或删除，否则该菜单无法还原"),
                         boundImg.c_str(), boundIdx, tgt.c_str());
        } else {
            wcscpy_s(tip, Tr(L"已安装启动还原（旧版安装，未绑定镜像）。\n要清除请用「更多功能>>」→「清除启动中还原菜单」"));
        }
        p->SetText(Tr(L"安装到启动"));
        p->SetEnabled(false);
        p->SetBkColor(0xFFFFFFFF);
        btn->SetTextColor(ui_skin::C_TEXT);
    } else {
        // 未安装：**必须先选到可用镜像 + 目标分区**才可点（用户 2026-09-29 规格：
        // 没选镜像时按钮应为灰色，否则"装成功却不知道装了什么"）。备份模式不涉及
        // 镜像绑定 → 一律灰（该按钮只在还原模式有意义）。
        const bool can = !m_backupMode && !m_wimPath.empty() && m_imageOk &&
                         m_selPart >= 0 && !m_busy;
        if (!can && !m_busy)
            LogInfo("GUI boot menu disabled: backup=" +
                    std::to_string(m_backupMode ? 1 : 0) +
                    " pathEmpty=" + std::to_string(m_wimPath.empty() ? 1 : 0) +
                    " imageOk=" + std::to_string(m_imageOk ? 1 : 0) +
                    " part=" + std::to_string(m_selPart) + " busy=" +
                    std::to_string(m_busy ? 1 : 0));
        p->SetText(Tr(L"安装到启动"));
        p->SetEnabled(can);
        p->SetBkColor(0xFFFFFFFF);
        btn->SetTextColor(ui_skin::C_TEXT);
        wcscpy_s(tip, can
            ? Tr(L"把当前镜像写进开机菜单：重启后选「SysRecover」进恢复环境，\n自动把该镜像还原到第二步选中的目标分区\n镜像请勿移动或删除，否则该菜单无法还原")
            : Tr(L"请先选择镜像与目标分区，再安装到启动"));
    }
    p->SetToolTip(tip);
    p->Invalidate();
}

void CMainForm::ToggleBootMenu() {
    // 「安装到启动」= **只负责安装**（2026-10-27 用户规格）：已安装时按钮由
    // RefreshBootMenuBtn 置灰，删除入口唯一化到「更多功能>>」→「清除启动中还原
    // 菜单」（ClearBootMenuEntry）。这里不再保留"已安装就走删除"的分支。
    if (m_busy) return;
    // ── 安装（用户 2026-09-29 规格）：把**当前镜像 → 第二步选中的目标分区**写成
    //    常驻任务契约，并装常驻启动项；开机菜单里选「SysRecover」即执行该还原。
    if (m_wimPath.empty() || m_selPart < 0 ||
        m_selPart >= (int)m_parts.size()) {
        SetStatus(Tr(L"请先选择镜像与目标分区，再安装到启动"));
        return;
    }
    // 加密镜像（docs/30 §9）：常驻菜单在**开机阶段**执行，那一刻没人能输密码 →
    // 「安装到启动」一律拒绝（A 引擎的「解压并安装」也不允许，机器重启后没人给口令）。
    if (sysrecover::ImageIsEncrypted(m_wimPath)) {
        SetStatus(Tr(L"加密镜像不能安装到启动菜单"));
        MessageBoxW(
            m_hWnd,
            std::wstring(Tr(L"这是加密镜像：开机阶段无法输入密码，\n"
                            L"因此不能安装到启动菜单。\n\n"
                            L"请改用「开始恢复」（解压并安装 / 还原后重启），或在 PE 中就地还原。"))
                .c_str(),
            Tr(L"安装到启动"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    {
        const PartitionInfo& part = m_parts[m_selPart];
        // ⚠️ CConfirmDlg 的正文只显示**最多 3 行**、且是**单行标签**（PIT-039），
        // 超长/超行会被静默裁掉（行宽预算 376px/fs13 ≈ 28 全角字）—— 刻意压成
        // 3 行短句：第 1 行 = 用法（按引导链分措辞：BIOS 是 bootmgr 启动菜单 /
        // UEFI 是固件启动项，键名按品牌/机型识别（bioskey），识别不到用中性
        // 「快捷启动键」——不能写"BIOS菜单"会把用户误导进 BIOS 设置，也不能
        // 写死 F12（HP 是 F9，2026-10-18 客户反馈）；第 2 行只显示文件名（路径
        // 可能很长，裁到 18 字符）；第 3 行**必须保留"会格式化"的危险告知**
        //（§11 二次确认红线）。
        std::wstring name = m_wimPath;
        size_t sl = name.find_last_of(L"\\/");
        if (sl != std::wstring::npos)
            name = name.substr(sl + 1);
        if (name.size() > 18)
            name = name.substr(0, 18) + L"...";
        wchar_t msg[512];
        if (UseUefiBootFor(part))
            swprintf(msg, 512,
                     Tr(L"重启后按 %ls 选「SysRecover」还原\n"
                        L"镜像：%ls（第 %d 个）\n"
                        L"目标：%ls: 盘 —— 选中即格式化该分区"),
                     BootMenuKeyHint().c_str(), name.c_str(), m_selImageIndex,
                     part.letter.empty() ? L"?" : part.letter.c_str());
        else
            swprintf(msg, 512,
                     Tr(L"重启后在启动菜单选「SysRecover」还原\n"
                        L"镜像：%ls（第 %d 个）\n"
                        L"目标：%ls: 盘 —— 选中即格式化该分区"),
                     name.c_str(), m_selImageIndex,
                     part.letter.empty() ? L"?" : part.letter.c_str());
        // 与"确认还原"一致：右按钮（动作）高亮且为回车默认项，ESC 一律取消。
        // 2026-10-28（用户规格）：「安装到启动」给两个选项 ——
        //   「扫描后安装」= 只检查镜像能否**只靠救援层**还原（block 引擎，不另占空间），
        //                  通过就直接装启动项；
        //   「解压并安装」= 现在就解压到 C:\~new 再装启动项（A 引擎；EFS/特殊重解析点必须走这条）。
        // 最后一行括号里的容量 = 镜像**未压缩内容大小**（即解压后实际写入 ~new 的量），
        // 由 QuerySwapSpace 现算（只读元数据）；算不出来就退回不带数字的说法。
        double needGb = 0.0;
        {
            sysrecover::SwapSpaceEstimate est;
            if (sysrecover::QuerySwapSpace(m_wimPath, m_selImageIndex, part, est) &&
                est.content > 0)
                needGb = (double)est.content / (1024.0 * 1024.0 * 1024.0);
        }
        std::wstring full = msg;
        if (needGb > 0.0) {
            // 自适应单位：≥1G 用 G（1 位小数），否则用 M（整数），再小用 K ——
            // 避免小镜像显示 "0.0G"/"0M"。
            wchar_t cap[32] = {};
            if (needGb >= 1.0)
                swprintf(cap, 32, L"%.1fG", needGb);
            else if (needGb * 1024.0 >= 1.0)
                swprintf(cap, 32, L"%.0fM", needGb * 1024.0);
            else
                swprintf(cap, 32, L"%.0fK", needGb * 1024.0 * 1024.0);
            wchar_t line[320] = {};
            swprintf(line, 320,
                     Tr(L"\n「扫描」= 检查镜像条件并安装恢复选项（不另占空间）\n「解压」= 解压到 C:\\~new 并安装恢复选项（占 C 盘 %ls）"),
                     cap);
            full += line;
        } else {
            full += Tr(L"\n「扫描」= 检查镜像条件并安装恢复选项（不另占空间）\n「解压」= 解压到 C:\\~new 并安装恢复选项（占 C 盘空间）");
        }
        const int pick = CConfirmDlg::Ask3(
            m_hWnd, Tr(L"安装到启动"), full, Tr(L"扫描后安装"), Tr(L"解压并安装"),
            Tr(L"取消"), /*defaultIdx=*/0, /*midDisabled=*/false, /*escIdx=*/2,
            /*centerTitle=*/true);
        if (pick == 2) return;
        if (pick == 1) {
            LogInfo("GUI menu install: user chose 解压 (engine=swap)");
            StartMenuToggle(/*install=*/true, /*engine=*/"swap");
            return;
        }
        LogInfo("GUI menu install: user chose 扫描");
        StartMenuScan();
        return;
    }
    RefreshBootMenuBtn();
    UpdateMainAction();
}

// 装/删菜单的异步执行（2026-10-18，用户实测"点了没反应"的修复）：
// 之前 StageRestoreMenu 在 UI 线程同步跑（写 ~28MB 救援载荷 + 逐文件双读
// CRC 校验 ≈ 56MB I/O），Windows 5 秒无消息泵即判定"无响应" → 幽灵窗。
// 现在与备份/还原同款：worker 线程 + WM_PROGRESS_UPDATE 进度 + 完成消息。
void CMainForm::StartMenuToggle(bool install, const std::string& engine) {
    if (m_busy) return;
    // 安装的请求在此定格（镜像/分区/子镜像索引在 busy 期间不可改）；
    // 删除不用请求，但统一传一份默认值，worker 里按 install 分支取用。
    RestoreRequest req;
    req.image = m_wimPath;
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        req.disk = (int)m_parts[m_selPart].diskIndex;
        req.part = (int)m_parts[m_selPart].partNumber;
    }
    req.index = m_selImageIndex;
    req.repairBoot = true;
    // 2026-10-28（用户规格）：engine 由「扫描/解压」决定 —— 扫描通过用 block
    // （救援层直接还原，安装时不解压），否则 swap（现在就解压到 C 盘）。
    req.engine = engine;
    m_busy = true;
    m_menuInstall = install;
    m_menuErr.clear();
    m_menuBlocked = false;
    LogInfo("GUI menu " + std::string(install ? "install" : "remove") +
            " request: image=" + W2U(req.image) + " disk=" +
            std::to_string(req.disk) + " part=" + std::to_string(req.part) +
            " index=" + std::to_string(req.index) +
            " engine=" + (install ? req.engine : std::string("-")) +
            " keepOld=" + std::to_string(req.keepOld ? 1 : 0));
    last_adv_ = sysrecover::ADV_NONE;
    // m_lastStage 给 WM_TIMER(1) 每秒刷新「已用」用（状态栏文字 + 流逝时间）。
    m_lastStage = install ? Tr(L"正在安装启动还原菜单（写入契约 + 部署救援环境）...")
                          : Tr(L"正在删除启动还原菜单...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    RefreshBootMenuBtn();
    LogInfo(std::string("GUI boot menu toggle: ") +
            (install ? "install" : "remove"));
    if (m_worker.joinable()) { m_worker.join(); }
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::MenuToggleAsync, this, install, req);
    } catch (...) {
        m_busy = false;
        UpdateMainAction();
        RefreshBootMenuBtn();
        SetStatus(Tr(L"线程创建失败"));
        MessageBoxW(m_hWnd, Tr(L"无法创建工作线程，请重试"), Tr(L"错误"),
                    MB_OK | MB_ICONERROR);
    }
}

// 「安装到启动」→「扫描」（2026-10-28 用户规格）：只读元数据，判断镜像能否
// **只靠救援层**还原（block 引擎）。能 → 直接免解压安装；不能 → 提示改用「解压」。
void CMainForm::StartMenuScan() {
    if (m_busy) return;
    m_busy = true;
    m_scanBad = 0;
    m_scanEnc = 0;
    m_lastStage = Tr(L"正在检查镜像能否由救援层直接还原...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    RefreshBootMenuBtn();
    const std::wstring image = m_wimPath;
    const int index = m_selImageIndex;
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    m_worker = std::thread([this, image, index] {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        size_t bad = 0, enc = 0;
        std::string err;
        const bool ok =
            sysrecover::CanRestoreInLinuxOnly(image, index, &bad, &enc, err);
        m_scanBad = bad;
        m_scanEnc = enc;
        LogInfo(std::string("GUI menu scan: linux_only=") +
                (ok ? "yes" : "no") + " reparse=" + std::to_string(bad) +
                " efs=" + std::to_string(enc) +
                (err.empty() ? "" : (" / " + err)));
        ::PostMessageW(m_hWnd, WM_MENU_SCAN_DONE, ok ? 1 : 0, 0);
    });
}

void CMainForm::OnMenuScanDone(bool linuxOnly) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    ShowProgress(false);
    if (m_worker.joinable()) m_worker.join();
    if (m_cancel) {
        UpdateMainAction();
        return;
    }
    SetProgress(0);
    if (linuxOnly) {
        // 能在救援层直接还原 → 免解压安装（安装时只写契约 + 引导层）
        StartMenuToggle(/*install=*/true, /*engine=*/"block");
        return;
    }
    // 不能 → 说明为什么，并把「解压并安装」放到默认项（回车=解压、ESC=取消）
    std::wstring why;
    if (m_scanBad > 0)
        why = Tr(L"镜像含特殊重解析点，救援层无法写回。");
    else if (m_scanEnc > 0)
        why = Tr(L"镜像含 EFS 加密文件，救援层会还原成空文件。");
    else
        why = Tr(L"镜像无法只在救援层中还原。");
    why += Tr(L"\n要现在解压到 C 盘吗？（约需与镜像内容等量的空间）");
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"该镜像需要先解压"), why, Tr(L"取消"),
                          Tr(L"解压并安装"), /*defaultIsRight=*/true) == 1) {
        StartMenuToggle(/*install=*/true, /*engine=*/"swap");
        return;
    }
    UpdateMainAction();
}

void CMainForm::MenuToggleAsync(bool install, const RestoreRequest& req) {    // 同 StartRestoreAsync：让出 CPU，写盘期间窗口仍可拖动（否则拖窗发飘）。
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    sysrecover::ResetBootBlocked();
    int rc = 0;
    if (install) {
        // 进度：ops 层 StageRestoreImpl 的 Apply（菜单安装同样要完整解压
        // ~new，见 ops.cpp 注释）、引导层前有 progress(99,"boot") 收官标记。
        // 约定：回调返回 **true = 请求中止**（与还原链路一致）。
        auto progressFn = [this](int pct, const std::string& stage) -> bool {
            if (m_cancel) return true;
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - m_lastProgressPost).count();
            bool stageChanged = stage != m_lastStageRaw;
            if (ms < kProgressThrottleMs && pct < 100 && !stageChanged)
                return false;
            m_lastProgressPost = now;
            m_lastStageRaw = stage;
            PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                        (LPARAM)new std::wstring(U2W(stage)));
            return false;
        };
        m_lastProgressPost = std::chrono::steady_clock::now();
        m_lastStageRaw.clear();
        std::string err;
        rc = StageRestoreMenu(req, err, progressFn, &last_adv_);
        m_menuErr = err;
    } else {
        std::string log;
        bool ok = RemoveBootLayer(log);
        // 绑定副契约必须一起删：否则 ReadMenuBinding 仍判"已安装" → 按钮
        // 停在「删除菜单」回不到「安装菜单」（用户 2026-10-05 实测 bug）。
        DeleteMenuBinding();
        m_menuErr = log;
        rc = ok ? 0 : 1;
    }
    // 线程局部标志必须在 worker 线程取（PIT-184 更正段）
    m_menuBlocked = sysrecover::TakeBootBlocked();
    PostMessage(WM_MENU_DONE, (WPARAM)rc, install ? 1 : 0);
}

void CMainForm::OnMenuDone(int rc, bool install) {
    KillTimer(m_hWnd, 1);
    m_busy = false;
    if (m_worker.joinable()) m_worker.join();
    LogInfo(std::string("GUI boot menu ") + (install ? "install" : "remove") +
            " done rc=" + std::to_string(rc) + " / " + m_menuErr);
    if (m_cancel) {
        // 「终止并退出」流程：不要再弹提示（窗口马上关），只留日志。
        ShowProgress(false);
        return;
    }
    ShowProgress(false);
    if (rc == 0) {
        if (install) {
            // 两条引导链的"进菜单"方式不同（PIT-092/094）：BIOS 在 bootmgr 的
            // 选择菜单里选；UEFI 是固件启动项，要开机按快捷启动键（键名按品牌/
            // 机型识别，bioskey；识别不到用中性措辞，不写死 F12）。
            bool uefiPath = m_selPart >= 0 && m_selPart < (int)m_parts.size() &&
                            UseUefiBootFor(m_parts[m_selPart]);
            if (uefiPath) {
                wchar_t done[256];
                swprintf(done, 256,
                         Tr(L"启动还原菜单已安装：开机按 %ls 选「SysRecover」即可还原该镜像"),
                         BootMenuKeyHint().c_str());
                SetStatus(done);
            } else {
                SetStatus(Tr(L"启动还原菜单已安装：开机选「SysRecover」即可还原该镜像"));
            }
        } else {
            SetStatus(Tr(L"已删除启动还原"));
        }
    } else if (install) {
        // 失败必须看得见（静默模式也弹，fail-safe —— 与 OnTaskComplete 同款）。
        std::wstring e = m_menuErr.empty() ? Tr(L"安装启动还原菜单失败")
                                           : U2W(m_menuErr);
        std::string advice = sysrecover::ErrorAdvice(rc, last_adv_);
        SetStatus(e);
        // 2026-10-28（用户规格）：被安全软件拦写 → 弹「退出防护 → 重试」，
        // 用户退出火绒后点重试即可成功，不用自己去猜原因。
        if (m_menuBlocked &&
            CConfirmDlg::Ask2(
                m_hWnd, Tr(L"安装启动还原菜单未完成"),
                Tr(L"写入启动项/引导区被安全软件拦截\n（如火绒/360 的「写磁盘保留扇区」）。\n请退出该防护或把本程序加入信任，再点「重试」。"),
                Tr(L"关闭"), Tr(L"重试"), /*defaultIsRight=*/true) == 1) {
            RefreshBootMenuBtn();
            UpdateMainAction();
            StartMenuToggle(/*install=*/true);
            return;
        }
        MessageBoxW(m_hWnd, (e + U2W(advice)).c_str(),
                    rc == 4 ? Tr(L"安全门禁拒绝") : Tr(L"安装启动还原菜单失败"),
                    MB_OK | MB_ICONERROR);
    } else {
        SetStatus(Tr(L"删除启动还原未完全成功"));
    }
    RefreshBootMenuBtn();
    UpdateMainAction();
}

// ── 「更多功能>>」下拉（2026-10-27 用户规格） ─────────────────────
// 原「异机」勾选框、「修复引导」按钮、原「清除菜单」按钮 + 新增「镜像检查」
// 全部收进这个菜单：第三行原本挤了 6 个按钮，英文/高分屏下必然裁字。
// 用原生 TrackPopupMenu（与 ShowImagePickMenu 同款）：菜单项的勾选/禁用态
// 每次打开时按当前状态现算，不依赖皮肤控件（也就没有尺寸/裁剪问题）。
void CMainForm::ShowMoreMenu() {
    if (m_busy) return;
    HMENU hMenu = ::CreatePopupMenu();
    if (!hMenu) return;
    std::string detail;
    std::wstring boundImg;
    int boundIdx = 1;
    unsigned long long boundOff = 0, boundSz = 0;
    const bool installed =
        BootMenuInstalled(detail) ||
        ReadMenuBinding(&boundImg, &boundIdx, &boundOff, &boundSz);
    // 「清除」只在真装了东西时可点（否则点了也是空转，用户会以为没生效）。
    // 「镜像检查」只在镜像已解析成功时可点（对坏镜像做全量校验没有意义）。
    const bool canVerify = !m_backupMode && !m_wimPath.empty() && m_imageOk;
    // 增量备份（2026-10-28 用户规格）：只在备份模式、且保存位置是**已存在的**
    // .wim/.esd 时可点（没有镜像就无从"追加"）。
    bool canAppend = false;
    if (m_backupMode && !m_backupPath.empty() &&
        GetFileAttributesW(m_backupPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const wchar_t* dot = wcsrchr(m_backupPath.c_str(), L'.');
        canAppend = dot && (_wcsicmp(dot, L".wim") == 0 ||
                            _wcsicmp(dot, L".esd") == 0);
    }
    ::AppendMenuW(hMenu,
                  MF_STRING | (IsUniversalChecked() ? MF_CHECKED : 0u), 1,
                  Tr(L"使用异机还原"));
    ::AppendMenuW(hMenu, MF_STRING, 2, Tr(L"修复引导"));
    ::AppendMenuW(hMenu, MF_STRING | (installed ? 0u : MF_GRAYED), 3,
                  Tr(L"清除启动中还原菜单"));
    ::AppendMenuW(hMenu, MF_STRING | (canVerify ? 0u : MF_GRAYED), 4,
                  Tr(L"镜像检查（完整校验）"));
    ::AppendMenuW(hMenu, MF_STRING | (canAppend ? 0u : MF_GRAYED), 5,
                  Tr(L"增量备份（追加到当前镜像）"));
    // 加密备份（docs/30）：勾选态 = 已设置口令；只保护**下一次**备份（输出 .zje）。
    // 备份模式才可点（还原模式下设置"下次备份密码"没有意义，灰掉避免误解）。
    const bool encOn = !m_backupPassword.empty();
    ::AppendMenuW(hMenu,
                  MF_STRING | (m_backupMode ? (encOn ? MF_CHECKED : 0u) : MF_GRAYED),
                  6, Tr(L"加密备份（输出 .zje，一个密码保护整个镜像）"));

    // MoreBtn 是顶层 float 控件 → GetPos() 已是客户区坐标；换屏幕坐标再弹。
    RECT rc{};
    CControlUI* pBtn = m_PaintManager.FindControl(_T("MoreBtn"));
    if (pBtn) rc = pBtn->GetPos();
    POINT pt = {rc.left, rc.bottom + 2};
    ::ClientToScreen(m_hWnd, &pt);
    // m_pickGuard 防"菜单关闭后焦点回来又把别的下拉顶出来"（同 ShowImagePickMenu）。
    m_pickGuard = true;
    const int cmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_LEFTALIGN, pt.x,
                                     pt.y, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);
    m_pickGuard = false;
    if (cmd <= 0) return;
    if (cmd == 1) {
        COptionUI* p = static_cast<COptionUI*>(
            m_PaintManager.FindControl(_T("Universal")));
        if (p) {
            const bool on = !p->IsSelected();
            p->Selected(on, /*bTriggerEvent=*/false);  // COptionUI 的接口是 Selected()
            SetStatus(on ? Tr(L"已启用异机还原（还原时自动注入启动驱动）")
                         : Tr(L"已关闭异机还原"));
            LogInfo(std::string("GUI universal: ") + (on ? "on" : "off"));
        }
        UpdateMainAction();
    } else if (cmd == 2) {
        StartRepairBoot();
    } else if (cmd == 3) {
        ClearBootMenuEntry();
    } else if (cmd == 4) {
        StartVerifyImage();
    } else if (cmd == 5) {
        StartIncrementalBackup();
    } else if (cmd == 6) {
        if (!m_backupPassword.empty()) {
            m_backupPassword.clear();
            SetStatus(Tr(L"已关闭备份加密"));
            LogInfo("GUI backup encryption: off");
            return;
        }
        std::wstring pwd;
        if (!CPasswordDlg::Ask(
                m_hWnd, Tr(L"加密备份"),
                Tr(L"请设置备份密码（忘记后无法恢复镜像）："), pwd,
                Tr(L"启用加密")))
            return;
        m_backupPassword = pwd;
        SetStatus(Tr(L"已启用备份加密（下次备份输出 .zje，请牢记密码）"));
        LogInfo("GUI backup encryption: on");
    }
}

// 「清除启动中还原菜单」（原 RepairBootBtn 的逻辑）：确认 → **异步**删引导层 +
// 绑定副契约。异步是必须的：删除要卸载 ESP 上的 ~28MB 载荷，同步跑在慢盘上
// 会触发 Windows 的"无响应"幽灵窗（PIT-152 同款）。
void CMainForm::ClearBootMenuEntry() {
    if (m_busy) return;
    if (CConfirmDlg::Ask2(
            m_hWnd, Tr(L"清除启动中还原菜单"),
            Tr(L"将删除开机启动菜单里的 SysRecover 条目，\n"
               L"并清理目标分区上的救援文件。\n"
               L"确定继续吗？"),
            Tr(L"取消"), Tr(L"清除"),
            /*defaultIsRight=*/false) != 1)
        return;
    StartMenuToggle(/*install=*/false);
}

// 「镜像检查」= 完整校验（wimlib_verify_wim 全文件扫描，CLI 的 `verify` 命令同源）。
// 与备份收尾的 Probe（秒级快检，PIT-106）区分：这里用户显式要求，愿意等。
void CMainForm::StartVerifyImage() {
    if (m_busy) return;
    if (m_wimPath.empty() || !m_imageOk) {
        SetStatus(Tr(L"请先选择可用的镜像文件"));
        return;
    }
    // 大镜像（几十 GB）全量扫描要十几分钟，必须先讲清代价（默认项 = 取消）。
    // ⚠️ Ask2(owner, title, msg, ...) —— title 只能是**单行**（PIT-183）：
    // 2026-10-27 用户实测曾把整段提示写成 title，标题被裁成
    // 「将对整个镜像做完整性校验（逐字扫描）。几十GB的镜」且右边超出外框。
    if (CConfirmDlg::Ask2(
            m_hWnd, Tr(L"镜像检查"),
            Tr(L"将对整个镜像做完整性校验（逐字节扫描）。\n"
               L"几十 GB 的镜像可能需要十几分钟。\n"
               L"确定现在校验吗？"),
            Tr(L"取消"), Tr(L"开始校验"),
            /*defaultIsRight=*/false) != 1)
        return;
    m_busy = true;
    m_verifyMsg.clear();
    m_lastStageRaw.clear();
    m_lastStage = Tr(L"正在校验镜像...");
    SetStatus(m_lastStage);
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    UpdateMainAction();
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    try {
        m_worker = std::thread(&CMainForm::VerifyImageAsync, this);
    } catch (...) {
        m_busy = false;
        KillTimer(m_hWnd, 1);
        ShowProgress(false);
        UpdateMainAction();
        SetStatus(Tr(L"无法启动校验线程"));
    }
}

void CMainForm::VerifyImageAsync() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const std::wstring img = m_wimPath;  // busy 期间 UI 已锁，这里只是取快照
    auto progressFn = [this](int pct, const std::string& stage) -> bool {
        if (m_cancel) return true;   // 返回 true = 请求中止（与其它 worker 同约定）
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now - m_lastProgressPost).count();
        const bool stageChanged = stage != m_lastStageRaw;
        if (ms < kProgressThrottleMs && pct < 100 && !stageChanged) return false;
        m_lastProgressPost = now;
        m_lastStageRaw = stage;
        PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct,
                    (LPARAM)new std::wstring(U2W(stage)));
        return false;
    };
    WimEngine wim;
    int rc = -1;
    if (wim.ok()) {
        std::wstring perr;
        bool ptmp = false;
        const std::wstring plain =
            sysrecover::ResolveImagePlain(img, progressFn, perr, &ptmp);
        if (!plain.empty()) {
            rc = wim.Verify(plain, progressFn);
            if (ptmp)
                sysrecover::ReleaseImagePlain(img);
        } else {
            m_verifyMsg = W2U(perr);
        }
    }
    if (rc == 0)
        m_verifyMsg.clear();
    else
        m_verifyMsg = W2U(WimEngine::ErrorString(rc));
    LogInfo("GUI verify rc=" + std::to_string(rc) +
            (m_verifyMsg.empty() ? "" : (" / " + m_verifyMsg)));
    ::PostMessageW(m_hWnd, WM_VERIFY_DONE, (WPARAM)rc, 0);
}

// ────────────────── 分区枚举 ──────────────────

void CMainForm::PopulatePartitions(const std::vector<DiskInfo>& disks) {
    auto* pCombo = static_cast<CPartitionComboUI*>(
        m_PaintManager.FindControl(_T("PartitionBox")));
    if (!pCombo) return;

    pCombo->RemoveAll();
    m_parts.clear();
    m_partStyles.clear();
    m_selPart = -1;
    int selDefault = -1;
    for (const auto& disk : disks) {
        for (const auto& part : disk.parts) {
            if (part.isEsp || part.isMsr || part.isRecovery || disk.isRemovable)
                continue;
            if (part.sizeBytes < 2ULL * 1024 * 1024 * 1024)
                continue;
            m_parts.push_back(part);
            m_partStyles.push_back(disk.style);
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
    // 空列表：收起框显示提示；点击时（CPartitionComboUI::DoEvent）直接打开分区向导。
    pCombo->SetEmptyHint(Tr(L"点击此处为新硬盘创建分区…"));
    CControlUI* pLoad = m_PaintManager.FindControl(_T("PartLoadingText"));
    if (pLoad) pLoad->SetVisible(false);
    UpdateMainAction();
}

// ────────────────── 新硬盘分区向导（2026-10-20） ──────────────────

void CMainForm::OpenInitDiskWizard() {
    if (m_busy) return;
    auto empties = sysrecover::FindEmptyDisks();
    if (empties.empty()) {
        const auto disks = sysrecover::EnumerateDisks();
        std::wstring msg =
            disks.empty()
                ? Tr(L"未检测到内置硬盘（磁盘可能未联机）。\n"
                     L"请先在磁盘管理中联机后再试。")
                : Tr(L"没有可用的系统分区。\n"
                     L"若磁盘已经分区，请先用磁盘管理调整分区。");
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"创建分区"),
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (empties.size() > 1) {
        MessageBoxW(m_hWnd,
                    Tr(L"检测到多块空硬盘。请只保留要安装系统的那一块后重试。"),
                    Tr(L"创建分区"), MB_OK | MB_ICONWARNING);
        return;
    }
    sysrecover::InitDiskRequest req;
    req.disk = empties[0];
    req.uefiSession = sysrecover::IsUefiBootSession();
    std::wstring low = m_wimPath;
    for (auto& ch : low) ch = (wchar_t)towlower(ch);
    req.win7Family = low.find(L"win7") != std::wstring::npos ||
                     low.find(L"windows 7") != std::wstring::npos;
    sysrecover::DiskScheme first, second;
    sysrecover::RankPartSchemes(req.uefiSession,
                                sysrecover::IsUefiFirmware(), req.win7Family,
                                req.disk.sizeBytes, first, second,
                                req.mbrAllowed, req.gptAllowed);
    req.preferred = first;
    // 2026-10-21 用户 VM 反馈：小容量磁盘按规则推荐（60GB 盘 Win10/11 → 整盘
    // 做系统盘；Win7 → 30GB + 其余数据盘），这里只给默认/建议下限。
    const auto hint = sysrecover::SuggestPartitionSizes(
        req.win7Family, first, req.disk.sizeBytes);
    req.defaultSystemGb = hint.systemGb;
    req.minSystemGb = req.win7Family ? 30 : 60;
    const auto r = sysrecover::ShowInitDiskDialog(m_hWnd, req);
    if (!r.ok) return;
    sysrecover::PartInitPlan plan;
    plan.diskIndex = req.disk.index;
    plan.scheme = r.scheme;
    plan.systemGb = r.systemGb;
    plan.win7Family = req.win7Family;
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在为新硬盘创建分区..."));
    SetProgress(0);
    // 进度条必须显式打开（2026-10-27 用户规格）：diskpart 建分区+格式化能跑
    // 几十秒，以前只有状态栏一行字、进度条根本不出现，看着像卡死。
    ShowProgress(true);
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) m_worker.join();
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    m_worker = std::thread([this, plan] {
        std::wstring err;
        std::string log;
        // 阶段 → 进度条（partinit-* 由 StageCn 翻中文）。节流与备份/还原同款：
        // 阶段名一变立刻发（否则 100ms 内首个阶段会被吞掉，PIT-098）。
        auto progressFn = [this](int pct, const std::string& stage) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - m_lastProgressPost)
                    .count();
            const bool stageChanged = stage != m_lastStageRaw;
            if (elapsed < kProgressThrottleMs && pct < 100 && !stageChanged)
                return;
            m_lastProgressPost = now;
            m_lastStageRaw = stage;
            auto* pStage = new std::wstring(U2W(stage));
            PostMessage(WM_PROGRESS_UPDATE, (WPARAM)pct, (LPARAM)pStage);
        };
        const int rc = sysrecover::ApplyPartInitPlan(plan, err, log, progressFn);
        m_initErr = err;
        m_initLog = log;
        LogInfo("partinit rc=" + std::to_string(rc) + "\n" + log);
        ::PostMessageW(m_hWnd, WM_PART_INIT_DONE, (WPARAM)rc, 0);
    });
}

void CMainForm::StartRepairBoot(bool confirm) {
    if (m_busy) return;
    if (confirm && MessageBoxW(m_hWnd,
                               Tr(L"将自动查找系统盘与 ESP，重建并校验引导文件。\n"
                                  L"确定继续吗？"),
                               Tr(L"修复引导"),
                               MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    LogInfo(std::string("GUI repair-boot start") + (confirm ? "" : " (retry)"));
    m_busy = true;
    m_fixBlocked = false;
    UpdateMainAction();
    SetStatus(Tr(L"正在修复引导..."));
    SetProgress(0);
    SetTimer(m_hWnd, 1, 1000, nullptr);
    if (m_worker.joinable()) m_worker.join();
    m_worker = std::thread([this] {
        sysrecover::ResetBootBlocked();
        std::string msg;
        const int rc = sysrecover::RepairBoot(-1, -1, msg);
        m_fixMsg = msg;
        // 必须在**同一 worker 线程**取（线程局部标志；PIT-184 更正段）
        m_fixBlocked = sysrecover::TakeBootBlocked();
        LogInfo("repair-boot rc=" + std::to_string(rc) +
                " blocked=" + std::to_string(m_fixBlocked ? 1 : 0) + ": " + msg);
        ::PostMessageW(m_hWnd, WM_FIXBOOT_DONE, (WPARAM)rc, 0);
    });
}

// ── 异机还原（2026-10-21，0.9）：只用一个勾选框 ────────────────────
// 勾选后：程序目录 `Drivers\` 存在且含 .inf → 自动注入；否则仅启用内置驱动。
bool CMainForm::IsUniversalChecked() {
    COptionUI* p = static_cast<COptionUI*>(
        m_PaintManager.FindControl(_T("Universal")));
    return p && p->IsSelected();
}

// ────────────────── 文件对话框 ──────────────────

void CMainForm::BrowseWimFile() {
    IFileOpenDialog* pDlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDlg));
    if (FAILED(hr) || !pDlg) { SetStatus(Tr(L"文件对话框创建失败")); return; }
    COMDLG_FILTERSPEC filters[] = {
        {Tr(L"WIM/ESD 镜像"), L"*.wim;*.esd;*.zje"},
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
                SetImagePathEdit(m_wimPath);
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
                m_backupPath = path;
                SetImagePathEdit(m_backupPath);
                SetStatus(Tr(L"备份保存至：") + m_backupPath);
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
    SetImagePathEdit(best);  // 同步原生 EDIT → EN_CHANGE 闭环
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
    if (ext != L".esd" && ext != L".wim" && ext != L".zje") {
        SetStatus(Tr(L"只支持拖入 .esd / .wim / .zje 镜像文件"));
        return;
    }
    std::wstring& active = ActivePath();
    active = dropped;
    SetImagePathEdit(active);
    if (m_backupMode) {
        SetStatus(Tr(L"备份保存至：") + active);
        SyncFormatFromPath();  // 拖进来的 .wim/.esd 同样决定格式
        UpdateMainAction();
    } else {
        LoadWimImages(active);  // 内部会置 m_imageOk 并刷新主按钮
        SetStatus(Tr(L"已载入镜像：") + active);
    }
}

void CMainForm::LoadWimImages(const std::wstring& path) {
    m_imageOk = false;
    CComboUI* pCombo =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("ImageIndexBox")));
    if (!pCombo) { UpdateMainAction(); return; }
    pCombo->RemoveAll();
    m_imgIdx.clear();
    // 加密镜像（docs/30）：先要口令（同一次运行内记住，不再反复问），
    // 再读容器 meta 列出子镜像 —— 不需要解密几十 GB 正文。
    if (sysrecover::ImageIsEncrypted(path) &&
        sysrecover::GetImagePassword(path).empty()) {
        std::wstring pwd;
        if (!CPasswordDlg::Ask(m_hWnd, Tr(L"加密镜像"),
                               Tr(L"该镜像已加密，请输入密码："), pwd)) {
            SetStatus(Tr(L"已取消输入密码"));
            UpdateMainAction();
            return;
        }
        sysrecover::SetImagePassword(path, pwd);
    }
    std::vector<ImageDesc> images;
    int rc = sysrecover::ImageList(path, images);
    if (rc != 0 || images.empty()) {
        // 关键：读取失败（例如上次备份中途退出留下"写入未完成"的坏镜像）
        // 时必须保持 m_imageOk=false 并刷新按钮，否则主按钮的状态会停在
        // 上一次的可用态（用户看到"灰色按钮切一下模式又能点了"的怪象）。
        SetStatus(std::wstring(Tr(L"镜像不可用（可能上次备份未完成，请重新备份）")) +
                  (sysrecover::ImageIsEncrypted(path)
                       ? Tr(L"：密码错误或加密镜像损坏")
                       : std::wstring(L": ") + WimEngine::ErrorString(rc)));
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
    std::wstring p = m_backupPath;
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
    if (!m_backupMode || m_backupPath.empty()) return;
    const wchar_t* wantExt = (BackupFmt() == 0) ? L".esd" : L".wim";
    std::wstring p = m_backupPath;
    size_t slash = p.find_last_of(L"\\/");
    size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
    size_t dot = p.find_last_of(L'.');
    std::wstring np = (dot != std::wstring::npos && dot > base)
                          ? p.substr(0, dot) + wantExt   // 换掉旧后缀
                          : p + wantExt;                 // 本来就没后缀
    if (np == m_backupPath) return;
    m_backupPath = np;
    SetImagePathEdit(m_backupPath);
}

// 「限制CPU」下拉 → 上限百分比。条目顺序（用户 2026-10-27 规格）：
//   不限速 / CPU 75% / CPU 50% / CPU 25%（从宽到严，见 skin/main.xml）。
// 备份中改了立即生效：改的是 Job 对象的速率，不打断正在跑的 wimlib。
void CMainForm::ApplyCpuCapFromUi() {
    // 下标 → 上限（**不是** sel*25：第 1 项是 75%，顺序已按用户要求重排）
    static const int kCapByIndex[] = {0, 75, 50, 25};
    CComboUI* p =
        static_cast<CComboUI*>(m_PaintManager.FindControl(_T("CpuCapBox")));
    if (!p) return;
    int sel = p->GetCurSel();
    if (sel < 0 || sel >= (int)(sizeof(kCapByIndex) / sizeof(kCapByIndex[0])))
        sel = 0;
    m_cpuCap = kCapByIndex[sel];
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
    const bool universal = IsUniversalChecked();
    // BitLocker 提醒（用户规格 2026-09-23）：**只要系统里有加密卷**就提醒。
    if (!AskBitLockerWarning(sysrecover::BitLockerVolumes()))
        return;
    // 目标盘健康提醒（PIT-086，用户 2026-09-26）：坏盘先警告（默认取消）。
    if (!AskDiskHealthWarning((int)part.diskIndex))
        return;
    // 静默模式默认：A 引擎 + keepOld=false（无人值守，~old 由 RunOnce 自动清理）。
    // 非静默模式下面确认框会覆盖这两个值。
    m_restoreEngine  = "swap";
    m_restoreKeepOld = false;
    // 非静默模式：需重启时弹三选（取消/还原后重启/重启后还原）；就地还原仍二选。
    // 静默模式：不弹框，直接暂存并重启（静默默认 = 还原后重启 / keepOld=false）。
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
        wchar_t confirm[1200];
        // 2026-10-20：启动方式与目标盘风格不一致时，在确认框里追加一行提示
        // （只提示、不阻止 —— 用户明确要求保留选择权；具体文案见下）。
        std::wstring bootNote;
        {
            const bool sessionUefi = sysrecover::IsUefiBootSession();
            const bool targetGpt =
                m_selPart >= 0 && m_selPart < (int)m_partStyles.size() &&
                m_partStyles[m_selPart] == sysrecover::PartitionStyle::GPT;
            if (!sessionUefi && targetGpt)
                bootNote = Tr(L"当前恢复环境以 Legacy 方式启动、目标盘为 GPT："
                              L"还原后请在 BIOS 中改为 UEFI 启动；若启动菜单里没有 "
                              L"Windows，用「修复引导」。");
            else if (sessionUefi && !targetGpt)
                bootNote = Tr(L"当前恢复环境以 UEFI 方式启动、目标盘为 MBR："
                              L"还原后请在 BIOS 中改为 Legacy/CSM 启动。");
        }
        // 2026-10-14 用户规格：确认框三选 —— 取消 / 还原后重启 / 重启后还原。
        // 还原后重启 = A 引擎（Windows 解压 + Linux 交换），可返回旧系统；
        // 重启后还原 = B 引擎（Linux 直写），C 盘内容全丢。
        // 文案按用户给定原文（Ask3 恰好 3 行）；盘符/分区号沿用现有格式化惯例。
        if (needReboot) {
            // 2026-10-19（1019 案 W3）：第 4 行前移特殊镜像提示 —— 含 EFS 加密
            // 文件/特殊重解析点的镜像走「重启后还原」会被暂存预检拒绝（W1/W2），
            // 在选择时刻就告诉用户该选哪个（CConfirmDlg 已扩到 4 行）。
            // 2026-10-20（用户规格）：空间不足时，开始前就显示「需要/可用」，
            // 并把「还原后重启」按钮变成红色「空间不足」且不可点；空间够则
            // 与以前完全一样（不追加提示）。默认回车落在「取消」（安全项）。
            sysrecover::SwapSpaceEstimate se{};
            bool spaceShort = false;
            if (sysrecover::QuerySwapSpace(m_wimPath, m_selImageIndex, part,
                                           se) &&
                se.freeBytes > 0 && !se.enough) {
                spaceShort = true;
                LogWarn("A-engine space short: need=" +
                        std::to_string(se.required) + " free=" +
                        std::to_string(se.freeBytes) +
                        " (mid button disabled)");
            }
            swprintf(confirm, 1200,
                     Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                        L"还原后重启，是解压镜像后重启修复引导，可以恢复到现在状态\n"
                        L"重启后还原，是重启格式化%ls: 盘后还原镜像，无法恢复现在状态\n"
                        L"含 EFS 加密文件或特殊重解析点的镜像，请选「还原后重启」"),
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber,
                     part.letter.empty() ? L"?" : part.letter.c_str());
            if (spaceShort) {
                wchar_t sp[192];
                swprintf(sp, 192,
                         Tr(L"A 引擎需要约 %.1f GB 可用空间，当前 %.1f GB"
                            "（已禁用）"),
                         se.required / 1073741824.0,
                         se.freeBytes / 1073741824.0);
                wcscat(confirm, L"\n");
                wcscat(confirm, sp);
            }
            if (!bootNote.empty()) {
                wcscat(confirm, L"\n");
                wcscat(confirm, bootNote.c_str());
            }
            int r = CConfirmDlg::Ask3(
                m_hWnd, Tr(L"确认还原"), confirm, Tr(L"取消"),
                spaceShort ? Tr(L"空间不足") : Tr(L"还原后重启"),
                Tr(L"重启后还原"), /*defaultIdx=*/spaceShort ? 0 : 1,
                /*midDisabled=*/spaceShort);  // 回车=还原后重启（推荐）；ESC=取消
            if (r == 0) return;  // 取消
            if (r == 2 && universal) {
                // 异机还原需要 Windows 侧 DISM；B 引擎（Linux apply）做不到。
                MessageBoxW(m_hWnd,
                            Tr(L"异机还原仅支持「还原后重启」（A 引擎），"
                               L"或在 PE 中使用就地还原；\n"
                               L"「重启后还原」在 Linux 救援层执行，无法注入启动驱动。"),
                            Tr(L"异机还原"), MB_OK | MB_ICONWARNING);
                return;
            }
            if (r == 1) {        // 还原后重启：A 引擎 swap + keepOld
                m_restoreEngine  = "swap";
                m_restoreKeepOld = true;
            } else {             // 重启后还原：B 引擎 block，内容全丢
                m_restoreEngine  = "block";
                m_restoreKeepOld = false;
            }
        } else {
            swprintf(confirm, 1200,
                     Tr(L"即将把镜像还原到 %ls: 盘（磁盘%u 分区%u）。\n"
                        L"该分区上的所有数据将丢失！%ls\n"
                        L"目标分区当前未被占用，将立即就地还原，不需要重启。"),
                     part.letter.empty() ? L"?" : part.letter.c_str(),
                     part.diskIndex, part.partNumber, logNote.c_str());
            if (!bootNote.empty()) {
                wcscat(confirm, L"\n");
                wcscat(confirm, bootNote.c_str());
            }
            if (CConfirmDlg::Ask2(m_hWnd, Tr(L"确认还原"), confirm, Tr(L"取消"),
                                  Tr(L"开始还原"), /*defaultIsRight=*/true,
                                  Tr(L"丢失！")) != 1)
                return;
            // 就地还原不走交换，引擎无意义但保持默认 swap（StageRestore 内部按需处理）
            m_restoreEngine  = "swap";
            m_restoreKeepOld = false;
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
    req.engine = m_restoreEngine;       // "swap"=还原后重启 / "block"=重启后还原
    req.keepOld = m_restoreKeepOld;     // swap 时 true=保留 ~old 可返回；block 恒 false
    req.universal = IsUniversalChecked();  // 异机还原（2026-10-21）
    // 加密镜像：口令来自本次运行输入（LoadWimImages 里问过并记在 imgsrc 表里）。
    if (sysrecover::ImageIsEncrypted(req.image))
        req.password = sysrecover::GetImagePassword(req.image);
    if (req.universal) {
        // 只认程序目录下的 Drivers\（用户放驱动包的位置）；没有 .inf 就仅内置。
        std::wstring drivers = ExeDir() + L"\\Drivers";
        int n = 0;
        std::wstring uerr;
        if (GetFileAttributesW(drivers.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (sysrecover::ValidateDriverFolder(drivers, &n, uerr))
                req.driverDir = drivers;
            else
                LogWarn("universal: Drivers folder ignored (" + W2U(uerr) + ")");
        }
    }
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
    std::wstring dest = m_backupPath;
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
    // 加密备份（docs/30）：口令非空 → 输出改成 .zje（容器），落盘后不留明文。
    const std::wstring pwd = m_backupPassword;
    if (!pwd.empty()) {
        size_t slash = dest.find_last_of(L"\\/");
        size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
        size_t dot = dest.find_last_of(L'.');
        dest = (dot != std::wstring::npos && dot > base)
                   ? dest.substr(0, dot) + L".zje"
                   : dest + L".zje";
    } else if (dest.size() >= 4 &&
               _wcsicmp(dest.c_str() + dest.size() - 4, L".zje") == 0) {
        // 关掉加密后再备份：文件名要回到 .wim/.esd，否则会出现"名叫 .zje
        // 但内容其实是明文 WIM"的怪文件（虽然按 magic 能认出来）。
        dest = dest.substr(0, dest.size() - 4) + (fmt == 0 ? L".esd" : L".wim");
    }
    m_backupPath = dest;
    SetImagePathEdit(m_backupPath);
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
    m_skippedFiles.clear();
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
    BeginBackupWorker(source, dest, compress, name, esp, /*append=*/false, pwd);
}

// ── 增量备份（追加到当前镜像，2026-10-28 用户规格 / docs/31）──────────────
// WIM/ESD 的多子镜像 = **文件级去重快照**：未变更的文件在各时间点之间共享，
// 改动过的文件整份重存；旧时间点保留（只增不减）。入口按用户要求放「更多功能>>」。
void CMainForm::StartIncrementalBackup() {
    if (m_busy) return;
    if (!m_backupMode) {
        SetStatus(Tr(L"增量备份请在「系统→文件」模式使用"));
        return;
    }
    const std::wstring dest = m_backupPath;
    if (dest.empty()) {
        SetStatus(Tr(L"请先选择已存在的镜像文件"));
        return;
    }
    if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(Tr(L"增量备份需要一个已存在的镜像文件"));
        MessageBoxW(m_hWnd, Tr(L"增量备份需要一个已存在的镜像文件。\n请先把「保存位置」指向已备份过的 .wim/.esd。"),
                    Tr(L"增量备份"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const wchar_t* dot = wcsrchr(dest.c_str(), L'.');
    if (!dot || (_wcsicmp(dot, L".wim") != 0 && _wcsicmp(dot, L".esd") != 0)) {
        SetStatus(Tr(L"增量备份只支持 .wim / .esd 镜像"));
        return;
    }
    // 秒级快检 + 现有时间点数（只读元数据、不解压数据）
    sysrecover::WimEngine eng;
    std::wstring why;
    std::vector<sysrecover::ImageDesc> imgs;
    if (!eng.ok() || eng.Probe(dest, why) != 0 ||
        eng.ListImages(dest, imgs) != 0 || imgs.empty()) {
        std::wstring msg = Tr(L"该文件不是可用的镜像：") +
                           (why.empty() ? dest : why);
        SetStatus(msg);
        MessageBoxW(m_hWnd, msg.c_str(), Tr(L"增量备份"),
                    MB_OK | MB_ICONERROR);
        return;
    }
    const int n = static_cast<int>(imgs.size());
    // 单个时间点的含义必须先讲清（避免用户以为"只存差异块"）。
    wchar_t head[768] = {};
    swprintf(head, 768,
             Tr(L"将把当前分区的新状态作为第 %d 个时间点追加到：\n%ls\n\n"
                L"未变更的文件不重复占空间；改动过的文件会整份重存。\n"
                L"旧时间点全部保留（镜像只增不减），还原时可选任一版本。\n\n"
                L"确定开始追加吗？"),
             n + 1, dest.c_str());
    if (CConfirmDlg::Ask2(m_hWnd, Tr(L"增量备份"), head, Tr(L"取消"),
                          Tr(L"开始追加"), /*defaultIsRight=*/false) != 1)
        return;

    const char* compress =
        BackupFmt() == 0 ? "recovery" : (BackupFmt() == 1 ? "maximum" : "fast");
    // 子镜像名沿用 "Backup"（wimlib 自动去重为 "Backup (N)"）。
    std::wstring name = L"Backup";
    CControlUI* pNote = m_PaintManager.FindControl(_T("NoteInput"));
    if (pNote) {
        CDuiString txt = pNote->GetText();
        if (!txt.IsEmpty()) name = txt.GetData();
    }
    std::wstring source = L"C:/";
    if (m_selPart >= 0 && m_selPart < (int)m_parts.size()) {
        const auto& part = m_parts[m_selPart];
        if (!part.letter.empty()) source = part.letter + L":/";
    }
    m_skippedFiles.clear();
    m_busy = true;
    UpdateMainAction();
    SetStatus(Tr(L"正在追加时间点..."));
    SetProgress(0);
    m_start = std::chrono::steady_clock::now();
    SetTimer(m_hWnd, 1, 1000, nullptr);
    LogInfo(std::string("GUI incremental backup start: ") + W2U(dest) +
            " (timepoint " + std::to_string(n + 1) + ")");
    if (m_worker.joinable()) m_worker.join();
    m_cancel = false;
    // ESP 子镜像不参与增量（每次追加会再塞一个 ESP 子镜像，把 esp_index 弄乱）。
    BeginBackupWorker(source, dest, compress, name, /*esp=*/false,
                      /*append=*/true, std::wstring());
}

// 备份/增量共用：起线程前把参数定格到成员（busy 期间 UI 已锁，防漂移）。
void CMainForm::BeginBackupWorker(const std::wstring& source,
                                  const std::wstring& dest,
                                  const std::string& compress,
                                  const std::wstring& name, bool esp,
                                  bool append, const std::wstring& password) {
    m_workerSource = source;
    m_workerDest = dest;
    m_workerCompress = compress;
    m_workerName = name;
    m_workerPassword = password;
    m_workerCpuCap = m_cpuCap;  // 起线程前定格（备份中再改走 SetCpuCap 直接生效）
    m_workerEsp = esp;
    m_workerAppend = append;
    m_lastProgressPost = std::chrono::steady_clock::now();
    m_lastStageRaw.clear();
    m_backupTimepoints = 0;
    m_backupTotalBytes = 0;
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
    req.append  = m_workerAppend;
    req.cpuCap  = m_workerCpuCap;
    req.esp     = m_workerEsp;
    req.password = m_workerPassword;
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
    std::vector<std::wstring> skipped;
    int rc = RunBackup(req, progressFn, err, &last_adv_, &skipped);
    m_skippedFiles = skipped;
    last_err_ = err;
    // 加密备份成功后把口令登记到进程表：切到还原页即可直接列出子镜像。
    if (rc == 0 && !m_workerPassword.empty())
        sysrecover::SetImagePassword(m_workerDest, m_workerPassword);
    // 增量完成后回主线程显示"第 N 个时间点 / 合计 X"（只读元数据，秒级）。
    if (rc == 0 && m_workerAppend) {
        sysrecover::WimEngine eng;
        std::vector<sysrecover::ImageDesc> imgs;
        if (eng.ok() && eng.ListImages(m_workerDest, imgs) == 0)
            m_backupTimepoints = static_cast<int>(imgs.size());
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(m_workerDest.c_str(), GetFileExInfoStandard,
                                 &fad))
            m_backupTotalBytes =
                (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) |
                fad.nFileSizeLow;
    }
    PostMessage(WM_TASK_COMPLETE, (WPARAM)rc, 0);
}

// ────────────────── 模式切换 / 可用态 ──────────────────

void CMainForm::SetImagePathEdit(const std::wstring& p) {
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (pEdit) pEdit->SetText(p.c_str());
}

void CMainForm::SetBusyControls(bool busy) {
    auto enable = [this, busy](const TCHAR* name, bool keep) {
        CControlUI* p = m_PaintManager.FindControl(name);
        if (p) p->SetEnabled(!busy || keep);
    };
    enable(_T("Silent"), true);
    enable(_T("CpuCapBox"), true);
    enable(_T("BrowseBtn"), false);
    enable(_T("SearchBtn"), false);
    enable(_T("ImagePath"), false);
    enable(_T("ImageIndexBox"), false);
    enable(_T("NoteInput"), false);
    enable(_T("PartitionBox"), false);
    enable(_T("FormatBox"), false);
    enable(_T("ModeRestore"), false);
    enable(_T("ModeBackup"), false);
    // 「更多功能>>」是一个按钮（内部四项：异机还原/修复引导/清除启动中还原菜单/
    // 镜像检查）——任务执行中整组禁用，避免同一时刻两路写盘（2026-10-27）。
    enable(_T("MoreBtn"), false);
    enable(_T("FixBootBtn"), false);
    enable(_T("LogBtn"), false);
    enable(_T("BootMenuBtn"), false);
    enable(_T("EspChk"), false);
    enable(_T("Universal"), false);
    enable(_T("SiteLink"), false);
    enable(_T("MainAction"), false);
    CControlUI* pEdit = m_PaintManager.FindControl(_T("ImagePath"));
    if (pEdit) {
        HWND h = static_cast<CSkinEditUI*>(pEdit)->GetNativeEditHWND();
        if (h) ::EnableWindow(h, busy ? FALSE : TRUE);
    }
}

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
        // 2026-10-11 用户规格：去掉「…」——还原页同位按钮（浏览系统镜像）本来
        // 就没有省略号，两页文字都是完整显示的，策略保持一致。
        Text(_T("BrowseBtn"),   Tr(L"浏览保存位置"));
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
        // 备份模式不给这些还原专属按钮（2026-09-28 / 2026-10-27 规格）：
        // 「安装到启动」「打包日志」只在还原模式出现；「更多功能>>」**两模式都要**
        //（2026-10-29 用户规格：加密备份/增量备份都在这个菜单里，备份模式必须有入口）。
        Vis(_T("FixBootBtn"),   false);  // 皮肤已隐藏；入口在「更多功能>>」
        Vis(_T("Universal"), false);  // 皮肤已隐藏；入口在「更多功能>>」
        Vis(_T("MoreBtn"),      true);
        Vis(_T("BootMenuBtn"),  false);
        Vis(_T("LogBtn"),       false);  // 「打包日志」是还原模式专属（用户 2026-10-03 规格）
        Text(_T("PartTitle"),   Tr(L"备份源分区"));
        Text(_T("PartHint"),    Tr(L"选择要备份为镜像的源分区（移动盘已隐藏）"));
        // 第三步右侧：备份模式 = 静默模式 + CPU 下拉；引导按钮两模式共用同一位置（XML 定）。
        // ★ 2026-09-30 二次调整（用户规格）：「类型」下拉移到**第一步「浏览」右侧**
        //   （与「搜索」按钮同位置同尺寸 639,77,710,115），ESP 勾选换到第三步原
        //   「格式：」处（388,296,510,329）。第三步右侧一组：
        //   静默 248..340 / ESP 388..510 / CPU 下拉 530..612 / 菜单 618..706
        Pos(_T("Silent"),        248, 302, 330, 326);
        Pos(_T("FormatBox"),     639, 77, 710, 115);   // 第一步：类型下拉
        Pos(_T("EspChk"),        388, 298, 510, 331);  // 第三步：ESP 勾选
        // 2026-10-29 用户规格：第三步最右侧补「更多功能>>」（加密/增量入口都在里面）。
        // 右侧组：静默 248..330 / ESP 388..510 / CPU 530..612 / 更多 618..721。
        Pos(_T("CpuCapBox"),     530, 298, 612, 331);
        Pos(_T("MoreBtn"),       618, 298, 721, 331);
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
        // 皮肤里这两行控件保持隐藏：入口已收进「更多功能>>」（2026-10-27 用户规格：
        // 第三行原本挤 6 个按钮，英文/高分屏必然裁字）。这里显式压回 false，防止
        // 皮肤改动或上一模式遗留把它们又露出来。
        Vis(_T("FixBootBtn"),   false);
        Vis(_T("Universal"),    false);
        Vis(_T("MoreBtn"),      true);
        Vis(_T("BootMenuBtn"),  true);
        Vis(_T("LogBtn"),       true);
        Text(_T("PartTitle"),   Tr(L"系统安装位置"));
        Text(_T("PartHint"),    Tr(L"选择要安装恢复镜像的目标分区（移动盘已隐藏）"));
        // 还原模式几何（切回时必须复位第三步右侧那几个）。2026-10-27 用户规格：
        //   静默 248..330 / 打包日志 340..438 / 安装到启动 446..562 / 更多功能>> 570..721
        // 四项等距 8px，右侧贴到卡片内沿；`Universal`/`FixBootBtn` 已隐藏、不再占位。
        Pos(_T("Silent"),        248, 302, 330, 326);
        Pos(_T("LogBtn"),        340, 298, 438, 331);
        Pos(_T("BootMenuBtn"),   446, 298, 562, 331);
        Pos(_T("MoreBtn"),       570, 298, 721, 331);
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
    SetImagePathEdit(ActivePath());
    SetStatus(Tr(m_backupMode
                     ? L"把当前系统热备份为镜像文件（VSS 快照，可在运行中备份）"
                     : L"选择镜像与目标分区后开始；还原系统盘会重启进恢复环境，非系统盘就地还原"));
}

void CMainForm::UpdateMainAction() {
    SetBusyControls(m_busy);
    CButtonUI* p =
        static_cast<CButtonUI*>(m_PaintManager.FindControl(_T("MainAction")));
    if (!p) return;
    // 还原模式同样需要先选好分区；备份模式还要选源分区，主按钮才转蓝；
    // 还原模式要求 m_imageOk，否则按钮保持灰色（避免用户拿不可读的镜像去还原）。
    std::wstring& active = ActivePath();
    bool ok = !active.empty() && m_selPart >= 0 && !m_busy &&
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
        if (active.empty())
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
    std::wstring& active = ActivePath();
    if (p.empty() && !hNative) {
        if (!active.empty() && pEdit->GetText().IsEmpty())
            SetImagePathEdit(active);
        return;
    }
    if (p == active)
        return;
    active = p;
    if (!m_backupMode && !active.empty() && active != m_lastLoadedWim &&
        ImagePathReady(active)) {
        m_lastLoadedWim = active;
        LoadWimImages(active);
    }
    SyncFormatFromPath();  // 备份：路径带 .wim/.esd 时自动定格
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
    if (!show) ShowPostProgress(false);
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

void CMainForm::ShowPostProgress(bool show) {
    CRoundProgressUI* p = static_cast<CRoundProgressUI*>(
        m_PaintManager.FindControl(_T("PostProgress")));
    if (p && p->IsVisible() != show) p->SetVisible(show);
    m_postBusy = show;
    if (show) {
        m_postPulse = 0;
        if (p) p->SetValue(0);
        SetTimer(m_hWnd, 3, 120, nullptr);
    } else {
        KillTimer(m_hWnd, 3);
        if (p) p->SetValue(0);
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
