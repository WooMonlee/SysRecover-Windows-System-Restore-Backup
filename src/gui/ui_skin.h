#pragma once
// 自绘控件层（经典 Duilib 无图片资源方案）。
//
// 背景：经典 Duilib 的 Button / Option / CheckBox / Combo / Progress 全部走
// 「状态图链」，没有图片就什么都不画（UIButton.cpp PaintStatusImage、
// UIProgress.cpp 无 foreimage 直接返回）；且控件级圆角只有 borderround 一个
// 属性。老 C#（WPF）界面的圆角/图标/自绘行无法用纯 XML 表达，故这里用
// GDI/GDI+ 自绘，经 WindowImplBase::CreateControl 注入（标签名必须新，
// 内建名在 UIDlgBuilder.cpp:292-352 已被拦截，不会回调到 CreateControl）。
//
// 注意：所有 C++ 标准头必须在 StdAfx.h 之前（PIT-012）。
#include <map>

#include "StdAfx.h"

#include "../common/version.h"  // SYSRECOVER_VERSION_W（标题栏副标题的唯一来源）

using namespace DuiLib;

namespace ui_skin {

// ── GDI+ 生命周期（进程内一次） ─────────────────────────────
void EnsureGdiplus();

// ── 颜色 ────────────────────────────────────────────────────
COLORREF ToRef(DWORD argb);

// ── 文本：GDI ExtTextOut + TA_BASELINE ──────────────────────
// 字号 px 即「逻辑像素高」；老界面的 CJK 字形视觉中心 ≈ 基线 - 0.43*px，
// 所以垂直居中一律用 BaselineOf() 反推基线，逐字对位才准。
int  BaselineOf(int centerY, int px);
int  TextW(HDC hdc, const wchar_t* s, int px, bool bold = false);
void TextAt(HDC hdc, const wchar_t* s, int x, int baselineY, int px,
            DWORD argb, bool bold = false);
// 取（并缓存）HFONT 句柄，px = 像素高。给原生 EDIT 子窗口 WM_SETFONT 用。
HFONT GetFontHandle(int px, bool bold = false);
// 在 rc 内水平对齐（0=左 1=中 2=右）+ 垂直居中
void TextIn(HDC hdc, const wchar_t* s, const RECT& rc, int px, DWORD argb,
            int halign = 0, bool bold = false);

// ── 图形：GDI+（抗锯齿）────────────────────────────────────
void FillRound(HDC hdc, const RECT& rc, int radius, DWORD argb);
void FillRound2(HDC hdc, int l, int t, int r, int b, int radius, DWORD argb);
void StrokeRound(HDC hdc, const RECT& rc, int radius, float width, DWORD argb);
void FillEll(HDC hdc, const RECT& rc, DWORD argb);
void Line(HDC hdc, int x1, int y1, int x2, int y2, float width, DWORD argb);
void Poly(HDC hdc, const POINT* pts, int n, float width, DWORD argb, bool close = false);
// GDI 实心矩形（画 1px 直线/方块用，不抗锯齿 → 硬边与原图一致）
void SolidRect(HDC hdc, const RECT& rc, DWORD argb);

// ── Color tokens（docs/ui-design.md §5，勿自创）─────────────
const DWORD C_TEXT      = 0xFF1B2432;
const DWORD C_SECOND    = 0xFF5F6C80;
const DWORD C_ACCENT    = 0xFF2B5CE0;
const DWORD C_PRIMARY   = 0xFF8CA0DE;
const DWORD C_DISABLED  = 0xFFE2E6EE;
const DWORD C_DIS_TEXT  = 0xFF9AA3B2;
const DWORD C_STEP      = 0xFFE08A2E;
const DWORD C_PROGRESS  = 0xFF6A5CE0;
const DWORD C_TRACK     = 0xFFE8EDF5;
const DWORD C_GREEN     = 0xFF1F9D57;

// ── 实测补充 token（来源：界面1.png 逐像素，见 docs/ui-design.md §6）──
const DWORD C_BORDER    = 0xFFDFE5EE;  // 面板 / 卡片 1px 边框
const DWORD C_EDIT_BD   = 0xFFC9D6F2;  // 输入框 / 组合框边框
const DWORD C_BTN_BD    = 0xFFE4E8EF;  // 白底次要按钮边框（实测比 C_BORDER 略深）
const DWORD C_BAR       = 0xFF8FA6E8;  // 分区行迷你用量条填充（≠ 主按钮 C_PRIMARY）
const DWORD C_TOPBAR    = 0xFFF7F9FC;
const DWORD C_BLUE_CARD = 0xFFF0F5FF;
const DWORD C_BAND      = 0xFFE8EFFB;  // 第二步表头条带
const DWORD C_STATUS    = 0xFFFAFBFE;
const DWORD C_GLYPH     = 0xFF697588;  // 窗口按钮字形
const DWORD C_CHECK_BD  = 0xFF787878;  // 静默模式勾选框边框
const DWORD C_CHECK_MK  = 0xFF242C3A;  // 对勾
const DWORD C_CHECK_TX  = 0xFF080808;  // 静默模式文字（比 C_TEXT 更黑）
const DWORD C_CHEV      = 0xFF98A2B3;  // 组合框右侧箭头

// ── 蓝色顶栏（2026-09-16 改版：渐变 + 选中 Tab 深蓝底）─────────
const DWORD C_TOP_L     = 0xFF8AA0E6;  // 顶栏渐变左端（Tab 区，较浅）
const DWORD C_TOP_R     = 0xFF5C74D0;  // 顶栏渐变右端（标题区，较深）
const DWORD C_TAB_SEL   = 0xFF3D53BE;  // 选中 Tab 圆角底（比顶栏明显更深）
const DWORD C_TAB_ON    = 0xFFFFFFFF;  // 选中 Tab 文字/图标（白）
const DWORD C_TAB_OFF   = 0xFFD7E0F7;  // 未选中 Tab 文字/图标（浅蓝白）
const DWORD C_TITLE_FG  = 0xFFFFFFFF;  // 标题文字
const DWORD C_TITLE_SUB = 0xFFDCE4F8;  // 副标题 v0.1
const DWORD C_GLYPH_LT  = 0xFFE8EDFB;  // 窗口按钮字形（浅色）

// 解析 "#RRGGBB" / "#AARRGGBB" / "0xAARRGGBB"（供控件 color 属性用）
DWORD ParseColor(LPCTSTR s);

}  // namespace ui_skin

// ────────────────────────────────────────────────────────────
// 顶栏渐变（左浅右深，2026-09-16 改版）。经典 CControlUI 只支持纯色
// bkcolor，渐变必须自绘。属性：left / right（#RRGGBB / #AARRGGBB）。
// ────────────────────────────────────────────────────────────
class CTopBarUI : public CControlUI {
public:
    CTopBarUI() : m_left(ui_skin::C_TOP_L), m_right(ui_skin::C_TOP_R) {}

    LPCTSTR GetClass() const override { return _T("TopBar"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

private:
    DWORD m_left, m_right;
};

// ────────────────────────────────────────────────────────────
// 模式 Tab（还原 / 备份）：图标 + 文字，选中时深蓝圆角底 + 白字
// 继承 COptionUI → group/selected/SELECTCHANGED 通知全部沿用，Notify 不改。
// ────────────────────────────────────────────────────────────
class CTabOptionUI : public COptionUI {
public:
    CTabOptionUI() : m_icon(0), m_bold(false) {}

    LPCTSTR GetClass() const override { return _T("TabOption"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

private:
    void DrawIcon(HDC hDC, int x, int y, int size, DWORD argb);
    int  m_icon;  // 1=还原 2=备份
    bool m_bold;  // 老界面 Tab 文字是粗体（实测墨迹密度明显更高）
};

// ────────────────────────────────────────────────────────────
// 静默模式复选框：13px 方框 + 深色对勾 + 文字（老界面是 WPF 默认
// CheckBox 观感：白底灰框 + 黑勾，不是蓝底白勾）
// ────────────────────────────────────────────────────────────
class CGlyphCheckBoxUI : public COptionUI {
public:
    LPCTSTR GetClass() const override { return _T("GlyphCheck"); }
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;
};

// ────────────────────────────────────────────────────────────
// 圆角进度条（老界面：轨道 #E8EDF5，前景 #6A5CE0，全圆角）
// ────────────────────────────────────────────────────────────
class CRoundProgressUI : public CControlUI {
public:
    CRoundProgressUI() : m_value(0) {}

    LPCTSTR GetClass() const override { return _T("RoundProgress"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

    void SetValue(int v);
    int  GetValue() const { return m_value; }

private:
    int m_value;  // 0..100
};

// ────────────────────────────────────────────────────────────
// 分区行（四列自绘，对齐老项目 PartitionRow.cs）
// 必须同时重写 DoPaint（下拉列表内）与 DrawItemText（Combo 收起框，
// 见 UICombo.cpp:1243 → IListItemUI::DrawItemText，基类是空实现）。
// ────────────────────────────────────────────────────────────
class CPartItemUI : public CListContainerElementUI {
public:
    CPartItemUI() : m_sys(false), m_pct(0) {}

    LPCTSTR GetClass() const override { return _T("PartItem"); }
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;
    void DrawItemText(HDC hDC, const RECT& rcItem) override;

    // 由 PopulatePartitions() 填充（字符串已格式化好）
    void SetPart(const wchar_t* letter, const wchar_t* ptype, const wchar_t* fs,
                 bool isSystem, const wchar_t* disk, const wchar_t* sub,
                 const wchar_t* partNo, const wchar_t* capText, const wchar_t* freeText,
                 int usedPct);

private:
    void PaintRow(HDC hDC, const RECT& rc, bool collapsed);

    CDuiString m_letter, m_ptype, m_fs, m_tag;   // 盘符 / MBR·GPT / 文件系统 / 角色标签
    CDuiString m_disk, m_sub, m_partNo, m_cap, m_free;
    bool m_sys;
    int  m_pct;
};

// ────────────────────────────────────────────────────────────
// 纯文字条目（子镜像下拉 / 保存格式下拉）
// 同样必须重写 DrawItemText，否则 Combo 收起框空白。
// ────────────────────────────────────────────────────────────
class CTextItemUI : public CListContainerElementUI {
public:
    LPCTSTR GetClass() const override { return _T("TextItem"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;
    void DrawItemText(HDC hDC, const RECT& rcItem) override;
};

// ────────────────────────────────────────────────────────────
// 标题「九转还原 v0.1」：老界面里两者同基线，标题 19px 粗体。
// 用自带 HFONT 绕开 Duilib 的 <Font> 粗体 bug（UIManager.cpp:2363 的
// lfWeight += FW_BOLD 叠在 400 上 = 1100，非法权重）。
// ────────────────────────────────────────────────────────────
class CTitleLabelUI : public CLabelUI {
public:
    CTitleLabelUI() : m_sub(SYSRECOVER_VERSION_W), m_subPx(13), m_px(19) {}

    LPCTSTR GetClass() const override { return _T("TitleLabel"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    void PaintText(HDC hDC) override;

private:
    CDuiString m_sub;  // 副标题（v0.1）
    int m_px, m_subPx;
};

// ────────────────────────────────────────────────────────────
// 自绘按钮：文字完全由 ui_skin 画（不走 Duilib 的 <Font>/font=N 体系）。
// 为什么必须自绘：经典 CButtonUI::PaintText 走 CRenderEngine::DrawText +
// CPaintManagerUI 字体表，且 PaintText 是在 borderround 的圆角裁剪区**内**
// 被调用的 —— 实测结果是字体回退成默认宋体、字号偏小、文字贴顶（详见
// AGENTS.md §13）。本类改为「底色 + 边框 + GDI 文字」全部自己画，字号/
// 粗细/颜色/垂直居中才可控。
// 属性：text / fontsize / bold / bkcolor / textcolor / bordercolor / borderround
// ────────────────────────────────────────────────────────────
class CSkinButtonUI : public CButtonUI {
public:
    CSkinButtonUI() : m_px(14), m_bold(false) {}

    LPCTSTR GetClass() const override { return _T("SkinButton"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

private:
    int  m_px;
    bool m_bold;
};

// ────────────────────────────────────────────────────────────
// 自绘输入框：底/边/文字自绘，同时把字号同步给原生 EDIT 子窗口（WM_SETFONT），
// 这样「有焦点（原生窗口显示）」和「无焦点（自绘文本）」两种状态字号一致。
// 属性：同 CSkinButtonUI，另沿用基类的 textpadding / nativebkcolor 等。
// ────────────────────────────────────────────────────────────
class CSkinEditUI : public CEditUI {
public:
    CSkinEditUI()
        : m_px(14), m_bold(false), m_fontApplied(false), m_bUnderline(false),
          m_dropHooked(false), m_dwLineColor(0xFFC9D6F2) {}

    LPCTSTR GetClass() const override { return _T("SkinEdit"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    void SetPos(RECT rc, bool bNeedInvalidate = true) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;
    void DoEvent(TEventUI& event) override;

private:
    void ApplyNativeFont();
    // 把原生 EDIT 子窗口里的文本同步回控件（失焦后是控件自绘，见 DoPaint）
    void SyncTextFromNative();
    // 原生 EDIT 子窗口只接受文件拖放还不够（默认 WndProc 会吞掉 WM_DROPFILES），
    // 这里给它挂一个子类过程，把 WM_DROPFILES 原样转投给顶层窗口统一处理。
    void EnsureDropTarget();
    static LRESULT CALLBACK DropSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR uIdSubclass,
                                             DWORD_PTR dwRefData);

    int   m_px;
    bool  m_bold;
    bool  m_fontApplied;
    bool  m_bUnderline;      // 只画最下方一根横线（无外框）
    bool  m_dropHooked;      // 原生 EDIT 已登记为拖放目标并挂好子类
    DWORD m_dwLineColor;     // 下划线颜色
};

// ────────────────────────────────────────────────────────────
// 窗口按钮（— □ ✕）：老界面无系统外框，全部自绘；关闭 hover 红底白字。
// 继承 CButtonUI → 点击通知仍走 Notify 里的 CLICK("MinBtn"/"CloseBtn")。
// ────────────────────────────────────────────────────────────
class CWindowBtnUI : public CButtonUI {
public:
    CWindowBtnUI() : m_glyph(0) {}

    LPCTSTR GetClass() const override { return _T("WinBtn"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

private:
    int m_glyph;  // 0=最小化 1=最大化 2=关闭
};

// ────────────────────────────────────────────────────────────
// 静态文字：一律走这个类，不走 Duilib 的 <Font>/font=N 体系。
// 原因：Duilib 的粗体是 lfWeight += FW_BOLD 叠在 400 上（UIManager.cpp:2363
// 附近），得到非法权重；而本类直接用自带 HFONT 缓存，字号/颜色/对齐全可控。
// 同时负责画 bkcolor/border（经典 Duilib 的 CLabelUI::DoPaint 不画底色，
// 所以表头条带 #E8EFFB 这类底色必须自己兜）。
// 属性：text / fontsize / color / align(left|center|right) / bold
// ────────────────────────────────────────────────────────────
class CSkinLabelUI : public CControlUI {
public:
    CSkinLabelUI() : m_px(13), m_align(0), m_bold(false), m_color(ui_skin::C_TEXT) {}

    LPCTSTR GetClass() const override { return _T("SkinLabel"); }
    void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) override;
    bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) override;

private:
    int   m_px;
    int   m_align;  // 0=左 1=中 2=右
    bool  m_bold;
    DWORD m_color;
};

// 在 CMainForm::CreateControl 里注册以上标签
CControlUI* CreateSkinControl(LPCTSTR pstrClass);
