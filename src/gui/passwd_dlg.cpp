#include "gui/passwd_dlg.h"

#include <commctrl.h>

#include "common/i18n.h"
#include "common/logger.h"
#include "gui/ui_skin.h"

using sysrecover::Tr;

namespace {
const int kWndW = 420;
const int kWndH = 196;
const int kCorner = 24;

// 原生 EDIT 子窗口会吃掉回车/ESC（单行 EDIT 不往上转发）→ 用户在密码框里
// 按回车"没反应"。这里子类化，把这两个键转发给对话框（真机实测踩到）。
LRESULT CALLBACK PwdEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                             UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        ::PostMessageW((HWND)ref, WM_KEYDOWN, wp, 0);
        return 0;
    }
    return ::DefSubclassProc(h, msg, wp, lp);
}
}  // namespace

CDuiString CPasswordDlg::GetSkinFolder() { return _T("skin\\"); }

CDuiString CPasswordDlg::GetSkinFile() {
    std::wstring xml = sysrecover::LoadSkinXml(L"passwd.xml");
    if (!xml.empty()) return CDuiString(xml.c_str());
    return _T("passwd.xml");
}

LPCTSTR CPasswordDlg::GetWindowClassName() const {
    return _T("SysRecoverUI.Password");
}

CControlUI* CPasswordDlg::CreateControl(LPCTSTR pstrClass) {
    return CreateSkinControl(pstrClass);
}

void CPasswordDlg::InitWindow() {
    m_alive = true;
    if (CControlUI* t = m_PaintManager.FindControl(_T("DlgTitle"))) {
        if (!m_title.empty()) t->SetText(m_title.c_str());
    }
    if (CControlUI* l1 = m_PaintManager.FindControl(_T("DlgMsg1"))) {
        if (!m_hint.empty()) l1->SetText(m_hint.c_str());
    }
    if (CControlUI* l2 = m_PaintManager.FindControl(_T("DlgMsg2"))) {
        l2->SetText(std::wstring(Tr(L"密码只用于本次操作，不会保存，也不会写入日志。")).c_str());
    }
    if (CControlUI* ok = m_PaintManager.FindControl(_T("BtnOK"))) {
        if (!m_okText.empty()) ok->SetText(m_okText.c_str());
    }
    // 原生 EDIT 子窗口：设成密码框（●），并把已有内容填进去。
    // 注意：这里可能还没建（延迟创建）→ EnsureEditSetup 会在激活时补。
    EnsureEditSetup();
}

void CPasswordDlg::EnsureEditSetup() {
    if (m_editSetup) return;
    CControlUI* e = m_PaintManager.FindControl(_T("PwdEdit"));
    if (!e) return;
    HWND h = static_cast<CSkinEditUI*>(e)->GetNativeEditHWND();
    if (!h) return;
    ::SendMessageW(h, EM_SETPASSWORDCHAR, (WPARAM)L'\x25CF', 0);
    if (!m_value.empty()) ::SetWindowTextW(h, m_value.c_str());
    ::SetWindowSubclass(h, PwdEditProc, 1, (DWORD_PTR)m_hWnd);
    ::InvalidateRect(h, nullptr, TRUE);
    m_editSetup = true;
}

std::wstring CPasswordDlg::ReadEdit() {
    EnsureEditSetup();
    CControlUI* e = m_PaintManager.FindControl(_T("PwdEdit"));
    if (!e) return {};
    if (HWND h = static_cast<CSkinEditUI*>(e)->GetNativeEditHWND()) {
        const int n = ::GetWindowTextLengthW(h);
        if (n <= 0) return {};
        std::wstring buf((size_t)n + 1, L'\0');
        ::GetWindowTextW(h, &buf[0], n + 1);
        buf.resize((size_t)n);
        return buf;
    }
    return e->GetText().GetData();
}

void CPasswordDlg::Finish(bool ok) {
    m_result = ok;
    Close();
}

void CPasswordDlg::Notify(TNotifyUI& msg) {
    if (msg.sType != DUI_MSGTYPE_CLICK) return;
    const CDuiString name = msg.pSender->GetName();
    if (name == _T("BtnOK")) {
        m_value = ReadEdit();
        Finish(true);
    } else if (name == _T("BtnCancel") || name == _T("DlgClose")) {
        Finish(false);
    }
}

LRESULT CPasswordDlg::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) {
            Finish(false);
            return 0;
        }
        if (wParam == VK_RETURN) {
            m_value = ReadEdit();
            Finish(true);
            return 0;
        }
    }
    if (msg == WM_ACTIVATE && LOWORD(wParam) != WA_INACTIVE) {
        EnsureEditSetup();
        CControlUI* e = m_PaintManager.FindControl(_T("PwdEdit"));
        if (e) {
            if (HWND h = static_cast<CSkinEditUI*>(e)->GetNativeEditHWND())
                ::SetFocus(h);
        }
    }
    if (msg == WM_DESTROY) m_alive = false;
    return WindowImplBase::HandleMessage(msg, wParam, lParam);
}

bool CPasswordDlg::Ask(HWND owner, const std::wstring& title,
                       const std::wstring& hint, std::wstring& pwd,
                       const std::wstring& okText) {
    CPasswordDlg* dlg = new CPasswordDlg();
    dlg->m_title = title;
    dlg->m_hint = hint;
    dlg->m_okText = okText;
    dlg->m_value = pwd;
    HWND h = dlg->Create(owner, Tr(L"九转还原"),
                         WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                         WS_EX_TOOLWINDOW, 0, 0, kWndW, kWndH);
    if (!h) {
        delete dlg;
        return false;
    }
    HRGN hRgn = ::CreateRoundRectRgn(0, 0, kWndW + 1, kWndH + 1, kCorner, kCorner);
    if (::SetWindowRgn(h, hRgn, TRUE) == 0) ::DeleteObject(hRgn);
    if (owner) ::EnableWindow(owner, FALSE);
    dlg->CenterWindow();
    dlg->ShowWindow(true);
    ::SetForegroundWindow(h);
    ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    // 原生 EDIT 是**首次获得焦点时**才创建的（Duilib UIExtend：CEditUI::SetFocus
    // → new CEditWnd）。所以必须用 SetFocus 触发，光 SetPos 建不出来
    //（实测：对话框里根本没有 Edit 子窗口 → 密码框点不进去）。
    if (CControlUI* e = dlg->m_PaintManager.FindControl(_T("PwdEdit"))) {
        dlg->m_PaintManager.SetFocus(e);
        dlg->EnsureEditSetup();
        if (HWND eh = static_cast<CSkinEditUI*>(e)->GetNativeEditHWND())
            ::SetFocus(eh);
    }
    MSG m;
    while (dlg->m_alive && ::GetMessageW(&m, nullptr, 0, 0)) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    if (owner) {
        ::EnableWindow(owner, TRUE);
        ::SetForegroundWindow(owner);
    }
    const bool ok = dlg->m_result && !dlg->m_value.empty();
    if (ok) pwd = dlg->m_value;
    sysrecover::LogInfo(std::string("password dlg: ") +
                        (dlg->m_result ? (ok ? "ok" : "empty") : "cancel"));
    delete dlg;
    return ok;
}
