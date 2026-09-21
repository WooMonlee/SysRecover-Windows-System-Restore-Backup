#include "confirm_dlg.h"

#include "ui_skin.h"

namespace {
// 与 skin/confirm.xml 的 <Window size> 一致；圆角直径同主窗口（24 = 半径 12）
const int kWndW   = 420;
const int kWndH   = 196;
const int kCorner = 24;
}  // namespace

CDuiString CConfirmDlg::GetSkinFolder() { return _T("skin\\"); }
CDuiString CConfirmDlg::GetSkinFile()  { return _T("confirm.xml"); }
LPCTSTR   CConfirmDlg::GetWindowClassName() const { return _T("SysRecoverUI.Confirm"); }

CControlUI* CConfirmDlg::CreateControl(LPCTSTR pstrClass) {
    return CreateSkinControl(pstrClass);
}

void CConfirmDlg::InitWindow() {
    m_alive = true;
    CControlUI* t = m_PaintManager.FindControl(_T("DlgTitle"));
    if (t && !m_title.empty())
        t->SetText(m_title.c_str());
    // 按钮文案：skin/confirm.xml 里两个按钮默认写的是还原确认用的文案
    // （「退出不重启」/「重启后还原」），这里按调用方给的文案覆盖。
    // 两个按钮宽度分别是 126px / 156px，实测都能放下 5 个汉字。
    CControlUI* b1 = m_PaintManager.FindControl(_T("BtnExit"));
    if (b1 && !m_leftText.empty())
        b1->SetText(m_leftText.c_str());
    CControlUI* b2 = m_PaintManager.FindControl(_T("BtnReboot"));
    if (b2 && !m_rightText.empty())
        b2->SetText(m_rightText.c_str());
    // 消息按 '\n' 拆成最多 3 行（CSkinLabelUI 的 TextIn 是单行绘制，不认 '\n'）。
    const wchar_t* names[3] = {_T("DlgMsg1"), _T("DlgMsg2"), _T("DlgMsg3")};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        CControlUI* lbl = m_PaintManager.FindControl(names[i]);
        if (!lbl)
            continue;
        size_t nl = m_msg.find(L'\n', start);
        std::wstring line = (nl == std::wstring::npos)
                                ? m_msg.substr(start)
                                : m_msg.substr(start, nl - start);
        lbl->SetText(line.c_str());
        if (nl == std::wstring::npos)
            break;
        start = nl + 1;
    }
}

void CConfirmDlg::Finish(int result) {
    m_result = result;
    Close();  // → WM_CLOSE → DestroyWindow → WM_DESTROY 置 m_alive=false
}

void CConfirmDlg::Notify(TNotifyUI& msg) {
    if (msg.sType != DUI_MSGTYPE_CLICK)
        return;
    CDuiString name = msg.pSender->GetName();
    if (name == _T("BtnExit"))        Finish(kExit);
    else if (name == _T("BtnReboot")) Finish(kExitReboot);
    else if (name == _T("DlgClose"))  Finish(kExit);
}

LRESULT CConfirmDlg::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 键盘兜底：ESC = 左按钮（安全的那个）；回车 = 默认项（见 m_defaultRight）。
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) { Finish(kExit); return 0; }
        if (wParam == VK_RETURN) {
            Finish(m_defaultRight ? kExitReboot : kExit);
            return 0;
        }
    }
    if (msg == WM_DESTROY)
        m_alive = false;
    return WindowImplBase::HandleMessage(msg, wParam, lParam);
}

int CConfirmDlg::Ask(HWND owner, const std::wstring& title,
                     const std::wstring& msg) {
    return Ask2(owner, title, msg, std::wstring(), std::wstring(),
                /*defaultIsRight=*/true);
}

int CConfirmDlg::Ask2(HWND owner, const std::wstring& title,
                      const std::wstring& msg, const std::wstring& leftText,
                      const std::wstring& rightText, bool defaultIsRight) {
    CConfirmDlg* dlg = new CConfirmDlg();
    dlg->m_title = title;
    dlg->m_msg = msg;
    dlg->m_leftText = leftText;
    dlg->m_rightText = rightText;
    dlg->m_defaultRight = defaultIsRight;

    // 无系统外框（WS_POPUP）+ 不进任务栏；owner = 主窗口（模态禁用）。
    HWND h = dlg->Create(owner, _T("知鉴一键还原"),
                         WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                         WS_EX_TOOLWINDOW, 0, 0, kWndW, kWndH);
    if (!h) {
        delete dlg;
        return kExit;  // 建窗失败则视为退出（fail-safe，不误重启）
    }
    // 圆角：roundcorner 只在 WM_SIZE 生效，而 XML 是 WM_CREATE 期解析的，
    // 建窗后手动补一次，幂等（同 instance_dlg.cpp）。
    HRGN hRgn = ::CreateRoundRectRgn(0, 0, kWndW + 1, kWndH + 1, kCorner, kCorner);
    if (::SetWindowRgn(h, hRgn, TRUE) == 0)
        ::DeleteObject(hRgn);

    if (owner)
        ::EnableWindow(owner, FALSE);
    dlg->CenterWindow();
    dlg->ShowWindow(true);
    ::SetForegroundWindow(h);

    // 局部模态消息循环：owner 已禁用，故本线程消息只服务于本对话框。
    MSG m;
    while (dlg->m_alive && ::GetMessageW(&m, nullptr, 0, 0)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    if (owner) {
        ::EnableWindow(owner, TRUE);
        ::SetForegroundWindow(owner);
    }
    int r = dlg->m_result;
    delete dlg;
    return r;
}
