#include "common/i18n.h"
#include "confirm_dlg.h"

using sysrecover::Tr;
#include "ui_skin.h"

namespace {
// 与 skin/confirm.xml 的 <Window size> 一致；圆角直径同主窗口（24 = 半径 12）
const int kWndW   = 420;
const int kWndH   = 196;
const int kCorner = 24;
}  // namespace

CDuiString CConfirmDlg::GetSkinFolder() { return _T("skin\\"); }
CDuiString CConfirmDlg::GetSkinFile() {
    // 词典把皮肤 XML 的 UI 文案翻好再交给 builder（它认 "<" 开头的整段 XML，
    // 见 UIDlgBuilder.cpp）；读不到/无译文时回退原文件名，行为与改动前一致。
    std::wstring xml = sysrecover::LoadSkinXml(L"confirm.xml");
    if (!xml.empty()) return CDuiString(xml.c_str());
    return _T("confirm.xml");
}
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
    // （「退出」/「还原后重启」），这里按调用方给的文案覆盖。
    // 两个按钮宽度分别是 126px / 156px，实测都能放下 5 个汉字。
    CControlUI* b1 = m_PaintManager.FindControl(_T("BtnExit"));
    if (b1 && !m_leftText.empty())
        b1->SetText(m_leftText.c_str());
    CControlUI* b2 = m_PaintManager.FindControl(_T("BtnReboot"));
    if (b2 && !m_rightText.empty())
        b2->SetText(m_rightText.c_str());
    // 三选模式（Ask3）：显示中间按钮，三个按钮重排为等宽 125px（12..403）。
    CControlUI* bm = m_PaintManager.FindControl(_T("BtnMid"));
    const bool three = !m_midText.empty();
    if (three && bm && b1 && b2) {
        bm->SetText(m_midText.c_str());
        bm->SetVisible(true);
        RECT r1 = { 12, 140, 137, 178 };
        RECT rm = { 145, 140, 270, 178 };
        RECT r2 = { 278, 140, 403, 178 };
        b1->SetPos(r1);
        bm->SetPos(rm);
        b2->SetPos(r2);
    }
    // 默认项视觉高亮（用户 2026-10-09）：skin 里 BtnReboot 固定蓝底（accent），
    // 默认项落在别处时要把蓝底/白字换到默认按钮上，否则看起来像默认项（误导）。
    // 注意：FindControl 返回 CControlUI*，SetTextColor 在 CLabelUI 基类上，
    // 需向下转型到 CSkinButtonUI。
    // 三选：按 m_defaultIdx 统一刷三个按钮；二选：仅默认在左时交换两色。
    auto Accent = [](CControlUI* c, bool on) {
        CSkinButtonUI* sb = static_cast<CSkinButtonUI*>(c);
        if (on) {
            sb->SetBkColor(0xFF8CA0DE);
            sb->SetBorderColor(0xFF8CA0DE);
            sb->SetTextColor(0xFFFFFFFF);
        } else {
            sb->SetBkColor(0xFFFFFFFF);
            sb->SetBorderColor(0xFFE4E8EF);
            sb->SetTextColor(0xFF1B2432);
        }
    };
    if (three && bm) {
        Accent(b1, m_defaultIdx == 0);
        Accent(bm, m_defaultIdx == 1);
        Accent(b2, m_defaultIdx == 2);
    } else if (!m_defaultRight && b1 && b2) {
        Accent(b1, true);
        Accent(b2, false);
    }
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
        // 行内高亮（丢失！/保留！红+粗）：只对包含 hl 子串的那一行生效。
        if (!m_hl.empty() && line.find(m_hl) != std::wstring::npos) {
            CSkinLabelUI* sl = static_cast<CSkinLabelUI*>(lbl);
            sl->SetHighlight(m_hl);
        }
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
    const bool three = !m_midText.empty();
    if (name == _T("BtnExit"))        Finish(kExit);
    else if (name == _T("BtnMid"))    Finish(three ? 1 : kExit);  // 仅三选可见
    else if (name == _T("BtnReboot")) Finish(three ? 2 : kExitReboot);
    else if (name == _T("DlgClose"))  Finish(kExit);  // 关闭 = 安全项（0）
}

LRESULT CConfirmDlg::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 键盘兜底：ESC = 左按钮（安全的那个）；回车 = 默认项（m_defaultIdx）。
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) { Finish(kExit); return 0; }
        if (wParam == VK_RETURN) {
            Finish(m_defaultIdx);
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
                      const std::wstring& rightText, bool defaultIsRight,
                      const std::wstring& hl) {
    CConfirmDlg* dlg = new CConfirmDlg();
    dlg->m_title = title;
    dlg->m_msg = msg;
    dlg->m_leftText = leftText;
    dlg->m_rightText = rightText;
    dlg->m_defaultRight = defaultIsRight;
    dlg->m_defaultIdx = defaultIsRight ? 1 : 0;  // 回车落点与高亮同一来源
    dlg->m_hl = hl;
    return RunModal(owner, dlg);
}

int CConfirmDlg::Ask3(HWND owner, const std::wstring& title,
                      const std::wstring& msg, const std::wstring& leftText,
                      const std::wstring& midText, const std::wstring& rightText,
                      int defaultIdx) {
    CConfirmDlg* dlg = new CConfirmDlg();
    dlg->m_title = title;
    dlg->m_msg = msg;
    dlg->m_leftText = leftText;
    dlg->m_midText = midText;
    dlg->m_rightText = rightText;
    if (defaultIdx < 0) defaultIdx = 0;
    if (defaultIdx > 2) defaultIdx = 2;
    dlg->m_defaultIdx = defaultIdx;
    dlg->m_defaultRight = (defaultIdx == 2);  // 三选高亮走 m_defaultIdx 分支
    return RunModal(owner, dlg);
}

int CConfirmDlg::RunModal(HWND owner, CConfirmDlg* dlg) {
    // 无系统外框（WS_POPUP）+ 不进任务栏；owner = 主窗口（模态禁用）。
    HWND h = dlg->Create(owner, Tr(L"九转还原"),
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
