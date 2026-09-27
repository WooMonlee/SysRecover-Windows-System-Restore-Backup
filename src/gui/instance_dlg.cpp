#include "common/i18n.h"
#include "instance_dlg.h"

using sysrecover::Tr;
#include "ui_skin.h"

namespace {
// 与 skin/instance.xml 的 <Window size> 一致；圆角直径同主窗口（24 = 半径 12）
const int kWndW   = 460;
const int kWndH   = 176;
const int kCorner = 24;
}  // namespace

CDuiString CInstanceDlg::GetSkinFolder() { return _T("skin\\"); }
CDuiString CInstanceDlg::GetSkinFile() {
    // 同 CConfirmDlg::GetSkinFile：交给 builder 的是已翻译的整段 XML。
    std::wstring xml = sysrecover::LoadSkinXml(L"instance.xml");
    if (!xml.empty()) return CDuiString(xml.c_str());
    return _T("instance.xml");
}
LPCTSTR   CInstanceDlg::GetWindowClassName() const { return _T("SysRecoverUI.Instance"); }

CControlUI* CInstanceDlg::CreateControl(LPCTSTR pstrClass) {
    return CreateSkinControl(pstrClass);
}

void CInstanceDlg::InitWindow() {
    m_alive = true;
    if (!m_prevBusy)
        return;
    // 旧实例正在备份/还原 → 灰掉「使用现在的程序」，不让用户把它关掉。
    // CSkinButtonUI 不会自动按 enabled 换色（见 CMainForm::UpdateMainAction
    // 的先例），必须显式设底色与文字色。
    CButtonUI* b = static_cast<CButtonUI*>(m_PaintManager.FindControl(_T("BtnCur")));
    if (b) {
        b->SetEnabled(false);
        b->SetBkColor(ui_skin::C_DISABLED);
        b->SetTextColor(ui_skin::C_DIS_TEXT);
        b->Invalidate();
    }
}

void CInstanceDlg::Finish(int result) {
    m_result = result;
    Close();  // → WM_CLOSE → DestroyWindow → WM_DESTROY 置 m_alive=false
}

void CInstanceDlg::Notify(TNotifyUI& msg) {
    if (msg.sType != DUI_MSGTYPE_CLICK)
        return;
    CDuiString name = msg.pSender->GetName();
    if (name == _T("BtnPrev"))       Finish(kUsePrevious);
    else if (name == _T("BtnCur"))   Finish(kUseCurrent);
    else if (name == _T("BtnQuit"))  Finish(kQuit);
    else if (name == _T("DlgClose")) Finish(kQuit);  // 右上角 ✕ 等同「退出」
}

LRESULT CInstanceDlg::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 键盘兜底（在基类之前拦截，不依赖焦点落在哪个按钮上）：
    // 回车 = 默认项「使用之前的程序」，ESC = 退出。
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) { Finish(kQuit);  return 0; }
        if (wParam == VK_RETURN) { Finish(m_result); return 0; }
    }
    if (msg == WM_DESTROY)
        m_alive = false;
    return WindowImplBase::HandleMessage(msg, wParam, lParam);
}

int CInstanceDlg::Ask(bool prevBusy) {
    CInstanceDlg* dlg = new CInstanceDlg();
    dlg->m_prevBusy = prevBusy;

    // 无系统外框（WS_POPUP）+ 不进任务栏；父窗口传 nullptr —— 此时主窗口还没建。
    HWND h = dlg->Create(nullptr, Tr(L"九转还原"), WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                         WS_EX_TOOLWINDOW, 0, 0, kWndW, kWndH);
    if (!h) {
        delete dlg;
        return kQuit;  // 建窗失败就别再让两个实例并存
    }
    // 圆角：与 main_win.cpp 同因（roundcorner 只在 WM_SIZE 生效，而 XML 是
    // WM_CREATE 期解析的），建窗后手动补一次，幂等。
    HRGN hRgn = ::CreateRoundRectRgn(0, 0, kWndW + 1, kWndH + 1, kCorner, kCorner);
    if (::SetWindowRgn(h, hRgn, TRUE) == 0)
        ::DeleteObject(hRgn);  // 成功时 region 归系统所有，不可再删

    dlg->CenterWindow();
    dlg->ShowWindow(true);
    ::SetForegroundWindow(h);

    // 局部消息循环：此刻本线程只有这一个窗口（主窗口尚未创建），
    // 所以直接取本线程全部消息即可，窗口销毁后 m_alive=false 退出。
    MSG m;
    while (dlg->m_alive && ::GetMessageW(&m, nullptr, 0, 0)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    int r = dlg->m_result;
    delete dlg;  // 窗口已销毁，等价于 main_win.cpp 里 MessageLoop 后 delete pWnd
    return r;
}
