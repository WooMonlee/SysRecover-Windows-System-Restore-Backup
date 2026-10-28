#include "common/i18n.h"
#include "common/logger.h"
#include "confirm_dlg.h"

using sysrecover::Tr;
#include "ui_skin.h"

namespace {
// 与 skin/confirm.xml 的 <Window size> 一致；圆角直径同主窗口（24 = 半径 12）
// 2026-10-19（1019 案 W3）：消息最多 4 行 —— 第 4 行（EFS/重解析点提示）只在
// 需要时把窗口加高 kLineH、按钮整体下移；≤3 行的对话框尺寸与位置完全不变。
// 2026-10-20：最多 5 行（空间不足提示）；四选（Ask4）窗口加宽到 520。
const int kWndW   = 420;
const int kQuadW  = 520;
const int kWndH   = 196;
const int kLineH  = 22;
const int kCorner = 24;
const int kButtonH = 38;
const int kButtonGap = 18;
const int kBottomGap = 18;

// 消息行数（按 '\n' 计），上限 6（skin 里 DlgMsg1..6）。
int MsgLineCount(const std::wstring& msg) {
    int lines = 1;
    for (wchar_t c : msg)
        if (c == L'\n' && ++lines >= 6)
            return 6;
    return lines;
}

// 自适应几何：按钮顶部 = 最后一行文字底部 + 固定间距；窗口底部 = 按钮底部
// + 固定留白。这样不会再出现“按钮下移了、窗口没长高 → 底部被裁掉”。
int MessageBottom(int lines) { return 56 + lines * kLineH; }
int ButtonTop(int lines) { return MessageBottom(lines) + kButtonGap; }
int WindowHeight(int lines) {
    const int h = ButtonTop(lines) + kButtonH + kBottomGap;
    return h > kWndH ? h : kWndH;
}

// 标题只占**单行**（skin/confirm.xml 的 DlgTitle 是 22,12,300,38 单行标签，
// CSkinLabelUI 用 TextOut 绘制、不认 '\n'，超出宽度的部分直接画到框外）。
// 2026-10-27 用户实测（PIT-183）：镜像检查把整段 3 行提示误传给 title →
// 屏上只剩「将对整个镜像做完整性校验（逐字扫描）。几十GB的镜」且右侧被切。
// 这里兜底降级：首行做标题、其余行并回正文，并留日志便于修调用方。
void NormalizeTitleAndMsg(std::wstring& title, std::wstring& msg) {
    const size_t nl = title.find(L'\n');
    if (nl == std::wstring::npos) return;
    std::wstring rest = title.substr(nl + 1);
    title = title.substr(0, nl);
    msg = rest + (msg.empty() ? std::wstring() : L"\n" + msg);
    sysrecover::LogWarn("confirm dlg: multi-line title downgraded into message "
                        "(fix the caller)");
}

// 消息最多 6 行（skin 只有 DlgMsg1..6）：超出的行原本会被静默丢掉 ——
// 截断并在末行加省略号，保证「还有内容」这件事在界面上看得见。
void ClampMsgLines(std::wstring& msg, int maxLines) {
    size_t pos = 0;
    for (int line = 1; line <= maxLines; ++line) {
        const size_t nl = msg.find(L'\n', pos);
        if (nl == std::wstring::npos) return;          // 总行数未超限
        if (line == maxLines) {                        // 末行之后还有内容
            msg = msg.substr(0, nl) + L"...";
            sysrecover::LogWarn("confirm dlg: message truncated to 6 lines");
            return;
        }
        pos = nl + 1;
    }
}
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
    CControlUI* b1 = m_PaintManager.FindControl(_T("BtnExit"));
    if (b1 && !m_leftText.empty())
        b1->SetText(m_leftText.c_str());
    CControlUI* b2 = m_PaintManager.FindControl(_T("BtnReboot"));
    if (b2 && !m_rightText.empty())
        b2->SetText(m_rightText.c_str());
    CControlUI* bm = m_PaintManager.FindControl(_T("BtnMid"));
    CControlUI* b4 = m_PaintManager.FindControl(_T("BtnMore"));
    // 三选（Ask3）/ 四选（Ask4）模式；4/5 行消息时整体下移 kLineH 的整数倍
    // —— RunModal 已把窗口加高同样的量。
    const bool three = !m_midText.empty();
    const bool quad = !m_moreText.empty();
    const int lines = MsgLineCount(m_msg);
    const int buttonTop = ButtonTop(lines);
    const wchar_t* names[6] = {_T("DlgMsg1"), _T("DlgMsg2"), _T("DlgMsg3"),
                               _T("DlgMsg4"), _T("DlgMsg5"), _T("DlgMsg6")};
    if (quad && b1 && bm && b2 && b4) {
        // 加宽：顶栏 / 关闭按钮 / 消息标签都按 520 宽重排（XML 是 420 版）。
        if (CControlUI* bg = m_PaintManager.FindControl(_T("TopBarBg"))) {
            RECT r = { 0, 0, kQuadW - 4, 44 };
            bg->SetPos(r);
        }
        if (CControlUI* cl = m_PaintManager.FindControl(_T("DlgClose"))) {
            RECT r = { kQuadW - 48, 2, kQuadW - 6, 42 };
            cl->SetPos(r);
        }
        for (int i = 0; i < 6; ++i) {
            CControlUI* lbl = m_PaintManager.FindControl(names[i]);
            if (lbl) {
                RECT r = { 22, 56 + i * kLineH, kQuadW - 22,
                           78 + i * kLineH };
                lbl->SetPos(r);
            }
        }
        bm->SetText(m_midText.c_str());
        bm->SetVisible(true);
        b4->SetText(m_moreText.c_str());
        b4->SetVisible(true);
        RECT r1 = { 12, buttonTop, 126, buttonTop + kButtonH };
        RECT rm = { 134, buttonTop, 248, buttonTop + kButtonH };
        RECT r2 = { 256, buttonTop, 370, buttonTop + kButtonH };
        RECT r4 = { 378, buttonTop, 492, buttonTop + kButtonH };
        b1->SetPos(r1);
        bm->SetPos(rm);
        b2->SetPos(r2);
        b4->SetPos(r4);
    } else if (three && bm && b1 && b2) {
        bm->SetText(m_midText.c_str());
        bm->SetVisible(true);
        RECT r1 = { 12, buttonTop, 137, buttonTop + kButtonH };
        RECT rm = { 145, buttonTop, 270, buttonTop + kButtonH };
        RECT r2 = { 278, buttonTop, 403, buttonTop + kButtonH };
        b1->SetPos(r1);
        bm->SetPos(rm);
        b2->SetPos(r2);
    } else if (b1 && b2) {
        RECT r1 = { 106, buttonTop, 232, buttonTop + kButtonH };
        RECT r2 = { 242, buttonTop, 398, buttonTop + kButtonH };
        b1->SetPos(r1);
        b2->SetPos(r2);
    }
    // 默认项视觉高亮（用户 2026-10-09）：skin 里 BtnReboot 固定蓝底（accent），
    // 默认项落在别处时要把蓝底/白字换到默认按钮上，否则看起来像默认项（误导）。
    // 注意：FindControl 返回 CControlUI*，SetTextColor 在 CLabelUI 基类上，
    // 需向下转型到 CSkinButtonUI。
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
    if (quad && b1 && bm && b2 && b4) {
        Accent(b1, m_defaultIdx == 0);
        Accent(bm, m_defaultIdx == 1);
        Accent(b2, m_defaultIdx == 2);
        Accent(b4, m_defaultIdx == 3);
    } else if (three && bm) {
        Accent(b1, m_defaultIdx == 0);
        Accent(bm, m_defaultIdx == 1);
        Accent(b2, m_defaultIdx == 2);
        // 禁用态（2026-10-20）：中间按钮显示为红色、不可点（空间不足）。
        if (m_midDisabled) {
            CSkinButtonUI* sb = static_cast<CSkinButtonUI*>(bm);
            sb->SetBkColor(0xFFFDECEC);
            sb->SetBorderColor(0xFFE8A0A0);
            sb->SetTextColor(0xFFC42B1C);
        }
    } else if (!m_defaultRight && b1 && b2) {
        Accent(b1, true);
        Accent(b2, false);
    }
    // 消息按 '\n' 拆成最多 5 行（CSkinLabelUI 的 TextIn 是单行绘制，不认 '\n'）。
    size_t start = 0;
    for (int i = 0; i < 6; ++i) {
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
    // ⚠️ 2026-10-28 实测（PIT-186）：`WindowImplBase::OnCreate` 会按**皮肤 XML 的
    // `<Window size="420,196">` 把窗口重设回 420x196** —— 于是 RunModal 传进去的
    // 加高尺寸（5/6 行消息）被 XML 覆盖，按钮下半截被裁掉（4/5/6 行都中招，
    // 3 行时恰好等于 196 所以以前没暴露）。这里在控件排完后再按行数**重设尺寸**。
    {
        const int w = quad ? kQuadW : kWndW;
        const int h = WindowHeight(lines);
        ::SetWindowPos(m_hWnd, nullptr, 0, 0, w, h,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
    const bool quad = !m_moreText.empty();
    if (name == _T("BtnExit"))        Finish(kExit);
    else if (name == _T("BtnMid")) {
        if (!m_midDisabled)           // 禁用态：忽略点击（红「空间不足」）
            Finish(three ? 1 : kExit);
    }
    else if (name == _T("BtnReboot")) Finish(three || quad ? 2 : kExitReboot);
    else if (name == _T("BtnMore"))   Finish(quad ? 3 : kExit);
    else if (name == _T("DlgClose"))  Finish(kExit);  // 关闭 = 安全项（0）
}

LRESULT CConfirmDlg::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    // 键盘兜底：ESC = m_escIdx（默认左按钮 = 安全项）；回车 = 默认项（m_defaultIdx）。
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_ESCAPE) { Finish(m_escIdx); return 0; }
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
                      int defaultIdx, bool midDisabled, int escIdx) {
    CConfirmDlg* dlg = new CConfirmDlg();
    dlg->m_title = title;
    dlg->m_msg = msg;
    dlg->m_leftText = leftText;
    dlg->m_midText = midText;
    dlg->m_rightText = rightText;
    dlg->m_midDisabled = midDisabled;
    if (midDisabled && defaultIdx == 1) defaultIdx = 0;  // 回车不能落在禁用项
    if (defaultIdx < 0) defaultIdx = 0;
    if (defaultIdx > 2) defaultIdx = 2;
    dlg->m_defaultIdx = defaultIdx;
    dlg->m_defaultRight = (defaultIdx == 2);  // 三选高亮走 m_defaultIdx 分支
    // ESC 落点（2026-10-28）：默认 0（左=历史安全项）；「安装到启动」传 2（取消）。
    if (escIdx < 0 || escIdx > 2 || (midDisabled && escIdx == 1)) escIdx = 0;
    dlg->m_escIdx = escIdx;
    return RunModal(owner, dlg);
}

int CConfirmDlg::Ask4(HWND owner, const std::wstring& title,
                      const std::wstring& msg, const std::wstring& leftText,
                      const std::wstring& midText, const std::wstring& rightText,
                      const std::wstring& moreText, int defaultIdx) {
    CConfirmDlg* dlg = new CConfirmDlg();
    dlg->m_title = title;
    dlg->m_msg = msg;
    dlg->m_leftText = leftText;
    dlg->m_midText = midText;
    dlg->m_rightText = rightText;
    dlg->m_moreText = moreText;
    if (defaultIdx < 0) defaultIdx = 0;
    if (defaultIdx > 3) defaultIdx = 3;
    dlg->m_defaultIdx = defaultIdx;
    dlg->m_defaultRight = (defaultIdx == 2);
    return RunModal(owner, dlg);
}

int CConfirmDlg::RunModal(HWND owner, CConfirmDlg* dlg) {
    // 无系统外框（WS_POPUP）+ 不进任务栏；owner = 主窗口（模态禁用）。
    // 消息行数自适应：窗口高度永远包含按钮底部 + 底留白；四选 → 加宽到 520。
    // 文案防御（PIT-183）：多行标题降级并回正文、超 6 行截断加省略号。
    NormalizeTitleAndMsg(dlg->m_title, dlg->m_msg);
    ClampMsgLines(dlg->m_msg, 6);
    const bool quad = !dlg->m_moreText.empty();
    const int w = quad ? kQuadW : kWndW;
    const int lines = MsgLineCount(dlg->m_msg);
    const int hgt = WindowHeight(lines);
    sysrecover::LogInfo("confirm dlg: lines=" + std::to_string(lines) +
                        " h=" + std::to_string(hgt) + " msgLen=" +
                        std::to_string(dlg->m_msg.size()));
    HWND h = dlg->Create(owner, Tr(L"九转还原"),
                         WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                         WS_EX_TOOLWINDOW, 0, 0, w, hgt);
    if (!h) {
        delete dlg;
        return kExit;  // 建窗失败则视为退出（fail-safe，不误重启）
    }
    // 圆角：roundcorner 只在 WM_SIZE 生效，而 XML 是 WM_CREATE 期解析的，
    // 建窗后手动补一次，幂等（同 instance_dlg.cpp）。
    HRGN hRgn = ::CreateRoundRectRgn(0, 0, w + 1, hgt + 1, kCorner, kCorner);
    if (::SetWindowRgn(h, hRgn, TRUE) == 0)
        ::DeleteObject(hRgn);

    if (owner)
        ::EnableWindow(owner, FALSE);
    dlg->CenterWindow();
    dlg->ShowWindow(true);
    ::SetForegroundWindow(h);
    // PIT-140（用户 2026-10-13）：启动期的 owner 一度传 nullptr，导致对话框与
    // 主窗口是两个互不相关的顶层窗口 → 藏在主界面背后、且主界面仍可点。
    // owner 正确时 Win32 保证被拥有窗口在 owner 之上；这里再显式提到 Z 序最前，
    // 防 SetForegroundWindow 在前台锁下失效（只改 Z 序/激活，不移动不改尺寸）。
    ::SetWindowPos(h, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

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
