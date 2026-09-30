// 自绘控件层实现。所有几何/配色来自对老程序界面截图（界面1.png）的逐像素实测，
// 见 docs/ui-design.md §6 与 AGENTS.md §15。改动前请先看 docs 里的实测表。
//
// 注意：<gdiplus.h> 必须在 StdAfx.h 之前（duilib 的 StdAfx 把 min/max 定义成
// 函数宏，会污染 GDI+ 头）。
#include "common/i18n.h"
#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tchar.h>
#include <wchar.h>
#include <string>  // std::wstring（SyncTextFromNative 用；标准头须在 StdAfx.h 之前，PIT-012）

using sysrecover::Tr;
#include "ui_skin.h"

namespace {

// Combo / TextItem 内文字的左缩进（px）：越小，左侧标签与下拉框越贴合。
const int kItemPadX = 6;

// ───────────────────────── 字体缓存 ─────────────────────────

struct FontKey {
    int  px;
    bool bold;
    bool operator<(const FontKey& o) const {
        if (px != o.px) return px < o.px;
        return bold < o.bold;
    }
};

std::map<FontKey, HFONT>& FontCache() {
    static std::map<FontKey, HFONT> s;
    return s;
}

// 老程序用「微软雅黑」；Win8+ 有 UI 变体，退回普通雅黑（PE 里可能都没有，
// 那就交给 GDI 字体链接）。字体名只探一次。
const wchar_t* FaceName() {
    static const wchar_t* s_face = nullptr;
    if (s_face) return s_face;
    HDC hdc = ::GetDC(nullptr);
    HFONT f = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    HGDIOBJ old = ::SelectObject(hdc, f);
    wchar_t got[LF_FACESIZE] = {0};
    ::GetTextFaceW(hdc, LF_FACESIZE, got);
    s_face = (_wcsicmp(got, L"Microsoft YaHei UI") == 0) ? L"Microsoft YaHei UI"
                                                         : L"Microsoft YaHei";
    ::SelectObject(hdc, old);
    ::DeleteObject(f);
    ::ReleaseDC(nullptr, hdc);
    return s_face;
}

HFONT GetFont(int px, bool bold) {
    FontKey k{px, bold};
    auto it = FontCache().find(k);
    if (it != FontCache().end()) return it->second;
    // ClearType：实测灰度抗锯齿（ANTIALIASED_QUALITY）的墨迹覆盖率明显偏低
    // （表头 460px vs 老界面 558px、分区行 215px vs 452px），观感「发虚、不清晰」。
    // 老界面虽是无边框透明窗口（WPF 灰度渲染），但笔画更实，用 ClearType 更接近。
    HFONT f = ::CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, FaceName());
    FontCache()[k] = f;
    return f;
}

Gdiplus::RectF RcF(const RECT& rc) {
    return Gdiplus::RectF((Gdiplus::REAL)rc.left, (Gdiplus::REAL)rc.top,
                          (Gdiplus::REAL)(rc.right - rc.left),
                          (Gdiplus::REAL)(rc.bottom - rc.top));
}

void AddRound(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float rad) {
    float d = rad * 2.0f;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    if (d <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    path.StartFigure();
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

}  // namespace

namespace ui_skin {

void EnsureGdiplus() {
    static ULONG_PTR s_token = 0;
    if (s_token == 0) {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&s_token, &in, nullptr);
    }
}

COLORREF ToRef(DWORD argb) {
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

int BaselineOf(int centerY, int px) {
    // 实测：雅黑 CJK 字形墨迹 ≈ [基线-0.86px, 基线+0.13px]，视觉中心 = 基线-0.43px
    return centerY + (int)(px * 0.43f + 0.5f);
}

int TextW(HDC hdc, const wchar_t* s, int px, bool bold) {
    if (!s || !*s) return 0;
    HGDIOBJ old = ::SelectObject(hdc, GetFont(px, bold));
    SIZE sz = {0, 0};
    ::GetTextExtentPoint32W(hdc, s, (int)wcslen(s), &sz);
    ::SelectObject(hdc, old);
    return sz.cx;
}

void TextAt(HDC hdc, const wchar_t* s, int x, int baselineY, int px, DWORD argb, bool bold) {
    if (!s || !*s) return;
    HGDIOBJ old = ::SelectObject(hdc, GetFont(px, bold));
    ::SetBkMode(hdc, TRANSPARENT);
    ::SetTextColor(hdc, ToRef(argb));
    ::SetTextAlign(hdc, TA_LEFT | TA_BASELINE);
    ::ExtTextOutW(hdc, x, baselineY, 0, nullptr, s, (UINT)wcslen(s), nullptr);
    ::SelectObject(hdc, old);
}

void TextIn(HDC hdc, const wchar_t* s, const RECT& rc, int px, DWORD argb, int halign, bool bold) {
    if (!s || !*s) return;
    int w = TextW(hdc, s, px, bold);
    int x = rc.left;
    if (halign == 1) x = rc.left + (rc.right - rc.left - w) / 2;
    else if (halign == 2) x = rc.right - w;
    int cy = (rc.top + rc.bottom) / 2;
    TextAt(hdc, s, x, BaselineOf(cy, px), px, argb, bold);
}

HFONT GetFontHandle(int px, bool bold) {
    return GetFont(px, bold);
}

DWORD ParseColor(LPCTSTR s) {
    if (!s) return 0;
    while (*s == L'#' || *s == L' ' || *s == L'\t') ++s;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    DWORD v = (DWORD)wcstoul(s, nullptr, 16);
    if (v <= 0x00FFFFFF) v |= 0xFF000000;  // 无 alpha 段视为不透明
    return v;
}

void FillRound(HDC hdc, const RECT& rc, int radius, DWORD argb) {
    FillRound2(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, argb);
}

void FillRound2(HDC hdc, int l, int t, int r, int b, int radius, DWORD argb) {
    if (r <= l || b <= t) return;
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    RECT rc = {l, t, r, b};
    AddRound(path, RcF(rc), (float)radius);
    Gdiplus::SolidBrush brush{Gdiplus::Color(argb)};
    g.FillPath(&brush, &path);
}

void StrokeRound(HDC hdc, const RECT& rc, int radius, float width, DWORD argb) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    if (width <= 1.5f) {
        // 1px 描边必须走 GDI：GDI+ 的抗锯齿会把 1px 线摊成 2px 半透明
        // （实测：#C9D6F2 的边框被渲染成 #E4EAF8，正是 50% 混白的结果），
        // 而老界面（WPF）直线段是纯色。做法与 Duilib CRenderEngine::DrawRoundRect
        // 一致（UIRender.cpp:1270）：PS_INSIDEFRAME + HOLLOW_BRUSH，线全部压在 rc 内。
        HPEN pen = ::CreatePen(PS_SOLID | PS_INSIDEFRAME, 1, ToRef(argb));
        HGDIOBJ oldPen = ::SelectObject(hdc, pen);
        HGDIOBJ oldBrush = ::SelectObject(hdc, ::GetStockObject(HOLLOW_BRUSH));
        const int d = radius > 0 ? radius * 2 : 0;  // RoundRect 收「椭圆宽高」= 直径
        ::RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, d, d);
        ::SelectObject(hdc, oldBrush);
        ::SelectObject(hdc, oldPen);
        ::DeleteObject(pen);
        return;
    }
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // 0.5px 内缩，让 1px 描边压在矩形边线上（与 WPF 1px Border 观感一致）
    Gdiplus::RectF r = RcF(rc);
    r.X += width / 2.0f;
    r.Y += width / 2.0f;
    r.Width -= width;
    r.Height -= width;
    Gdiplus::GraphicsPath path;
    AddRound(path, r, (float)radius);
    Gdiplus::Pen pen{Gdiplus::Color(argb), width};
    g.DrawPath(&pen, &path);
}

void FillEll(HDC hdc, const RECT& rc, DWORD argb) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush brush{Gdiplus::Color(argb)};
    g.FillEllipse(&brush, RcF(rc));
}

void Line(HDC hdc, int x1, int y1, int x2, int y2, float width, DWORD argb) {
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen{Gdiplus::Color(argb), width};
    g.DrawLine(&pen, x1, y1, x2, y2);
}

void Poly(HDC hdc, const POINT* pts, int n, float width, DWORD argb, bool close) {
    if (!pts || n < 2) return;
    EnsureGdiplus();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen{Gdiplus::Color(argb), width};
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (close) {
        g.DrawPolygon(&pen, reinterpret_cast<const Gdiplus::Point*>(pts), n);
    } else {
        g.DrawLines(&pen, reinterpret_cast<const Gdiplus::Point*>(pts), n);
    }
}

void SolidRect(HDC hdc, const RECT& rc, DWORD argb) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    HBRUSH br = ::CreateSolidBrush(ToRef(argb));
    ::FillRect(hdc, &rc, br);
    ::DeleteObject(br);
}

}  // namespace ui_skin

using namespace ui_skin;

// ═════════════════════════ CTabOptionUI ═════════════════════════

// ═════════════════════════ CTopBarUI ═════════════════════════

void CTopBarUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("left")) == 0) {
        m_left = ParseColor(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("right")) == 0) {
        m_right = ParseColor(pstrValue);
        return;
    }
    CControlUI::SetAttribute(pstrName, pstrValue);
}

bool CTopBarUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return true;
    EnsureGdiplus();
    Gdiplus::Graphics g(hDC);
    Gdiplus::Rect r(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    Gdiplus::LinearGradientBrush br(
        r, Gdiplus::Color(m_left), Gdiplus::Color(m_right),
        Gdiplus::LinearGradientModeHorizontal);
    g.FillRectangle(&br, r);
    return true;
}

// ═════════════════════════ CTabOptionUI ═════════════════════════

void CTabOptionUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {    if (_tcscmp(pstrName, _T("icon")) == 0) {
        m_icon = (_tcscmp(pstrValue, _T("backup")) == 0) ? 2 : 1;
        return;
    }
    if (_tcscmp(pstrName, _T("bold")) == 0) {
        m_bold = (_tcscmp(pstrValue, _T("true")) == 0 || _tcscmp(pstrValue, _T("1")) == 0);
        return;
    }
    COptionUI::SetAttribute(pstrName, pstrValue);
}

// 16x16 矢量图标：1=还原（顺时针圆箭头）2=备份（盒子+盖线）
void CTabOptionUI::DrawIcon(HDC hDC, int x, int y, int size, DWORD argb) {
    const float w = 1.3f;
    if (m_icon == 2) {
        RECT box = {x, y + 3, x + size, y + size};
        StrokeRound(hDC, box, 2, w, argb);
        Line(hDC, x + 1, y + 7, x + size - 1, y + 7, w, argb);
    } else {
        int cx = x + size / 2;
        int cy = y + size / 2 + 1;
        int r = size / 2 - 1;
        RECT arc = {cx - r, cy - r, cx + r, cy + r};
        EnsureGdiplus();
        {
            Gdiplus::Graphics g(hDC);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen{Gdiplus::Color(argb), w};
            g.DrawArc(&pen, RcF(arc), 45.0f, 275.0f);
            // 箭头（圆弧终点 320° 处的切向三角）
            double a = 320.0 * 3.14159265358979 / 180.0;
            int px = cx + (int)(r * cos(a));
            int py = cy + (int)(r * sin(a));
            Gdiplus::Point tri[3] = {Gdiplus::Point(px + 3, py + 2),
                                     Gdiplus::Point(px - 1, py + 3),
                                     Gdiplus::Point(px + 1, py - 2)};
            Gdiplus::SolidBrush br{Gdiplus::Color(argb)};
            g.FillPolygon(&br, tri, 3);
        }
    }
}

bool CTabOptionUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    const bool sel = IsSelected();
    const DWORD col = sel ? C_TAB_ON : C_TAB_OFF;
    const int cy = (rc.top + rc.bottom) / 2;

    // 顶栏改为蓝色渐变（见 CTopBarUI）后，Tab 不再有白色贴片：
    // 选中项用「更深的蓝色圆角底」拉开对比（用户 2026-09-16 规格），白字白图标；
    // 未选中项透明底 + 浅蓝白字。
    if (sel) {
        RECT pill = {rc.left - 6, rc.top + 3, rc.right + 6, rc.bottom - 3};
        FillRound(hDC, pill, 12, C_TAB_SEL);
    }
    DrawIcon(hDC, rc.left + 6, cy - 7, 16, col);
    // 实测：老界面 Tab 文字为粗体（墨迹密度约为常规体的 1.6 倍），墨迹中心 ≈ cy+1
    TextAt(hDC, GetText(), rc.left + 30, BaselineOf(cy, 15) + 1, 15, col, m_bold);
    return true;
}

// ═════════════════════════ CGlyphCheckBoxUI ═════════════════════════

bool CGlyphCheckBoxUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    const int cy = (rc.top + rc.bottom) / 2;

    // 实测：14x14 方框（y307..320），1px #787878 边框，勾为深色
    // （WPF 默认 CheckBox 观感：白底灰框黑勾，不是蓝底白勾）
    RECT box = {rc.left, cy - 6, rc.left + 14, cy + 8};
    SolidRect(hDC, box, 0xFFFFFFFF);
    StrokeRound(hDC, box, 2, 1.0f, C_CHECK_BD);
    if (IsSelected()) {
        POINT chk[3] = {{box.left + 3, box.top + 7},
                        {box.left + 6, box.top + 10},
                        {box.left + 11, box.top + 3}};
        Poly(hDC, chk, 3, 1.7f, C_CHECK_MK);
    }
    // 实测文字起点 = 方框左 + 18，13px，颜色比正文更黑
    TextAt(hDC, GetText(), rc.left + 18, BaselineOf(cy, 13), 13, C_CHECK_TX);
    return true;
}

// ═════════════════════════ CRoundProgressUI ═════════════════════════

void CRoundProgressUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("value")) == 0) {
        SetValue(_ttoi(pstrValue));
        return;
    }
    CControlUI::SetAttribute(pstrName, pstrValue);
}

void CRoundProgressUI::SetValue(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (v == m_value) return;
    m_value = v;
    Invalidate();
}

bool CRoundProgressUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    const int h = rc.bottom - rc.top;
    const int radius = h / 2;
    FillRound(hDC, rc, radius, C_TRACK);
    if (m_value > 0) {
        int w = (rc.right - rc.left) * m_value / 100;
        if (w < h) w = h;  // 极小进度也画成圆头，避免出现方角
        FillRound2(hDC, rc.left, rc.top, rc.left + w, rc.bottom, radius, C_PROGRESS);
    }
    return true;
}

// ═════════════════════════ CPartItemUI ═════════════════════════

void CPartItemUI::SetPart(const wchar_t* letter, const wchar_t* ptype, const wchar_t* fs,
                          bool isSystem, const wchar_t* disk, const wchar_t* sub,
                          const wchar_t* partNo, const wchar_t* capText,
                          const wchar_t* freeText, int usedPct) {
    m_letter = letter ? letter : L"";
    m_ptype  = ptype ? ptype : L"";
    m_fs     = fs ? fs : L"";
    m_sys    = isSystem;
    m_disk   = disk ? disk : L"";
    m_sub    = sub ? sub : L"";
    m_partNo = partNo ? partNo : L"";
    m_cap    = capText ? capText : L"";
    m_free   = freeText ? freeText : L"";
    m_pct    = usedPct < 0 ? 0 : (usedPct > 100 ? 100 : usedPct);
    // 角色标签：系统分区绿字，其余用普通蓝（老项目 TypeBrush；当前只区分系统/非系统）
    m_tag = isSystem ? Tr(L"当前系统") : L"";
}

// 四列布局（相对 rc.left 的实测偏移，来自老界面逐像素测量）
// 组合框左边框绝对 x35，故这里 = 绝对坐标 - 35。
static const int kCol1 = 9;    // 盘符 + MBR/GPT + 文件系统 + 角色标签（"C" 墨迹 x44）
static const int kCol2 = 177;  // 磁盘型号 / 卷标
static const int kCol3 = 364;  // 磁盘号/分区号（"0/1" 墨迹 x399）
static const int kCol4 = 438;  // 用量条左沿 x473 + 第二行「可用」x474
static const int kBarW = 60;   // 用量条 x473..532
static const int kBarH = 7;    // y241..247
static const int kCapX = 504;  // 容量文字 x539
static const int kChev = 781;  // 右侧箭头 x816..822
static const int kTagX = 120;  // 角色标签（"当前系统" 墨迹 x155）

bool CPartItemUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    // 下拉列表里的行：选中/悬停底色
    if (IsSelected()) {
        FillRound(hDC, m_rcItem, 6, 0xFFEDF2FF);
    } else if ((m_uButtonState & UISTATE_HOT) != 0) {
        FillRound(hDC, m_rcItem, 6, 0xFFF7F9FC);
    }
    PaintRow(hDC, m_rcItem, /*collapsed=*/false);
    return true;
}

void CPartItemUI::DrawItemText(HDC hDC, const RECT& rcItem) {
    // Combo 收起框：底色由 Combo 自己画，这里只画行内容 + 右侧 chevron
    PaintRow(hDC, rcItem, /*collapsed=*/true);
}

void CPartItemUI::PaintRow(HDC hDC, const RECT& rc, bool collapsed) {
    const int mid = (rc.top + rc.bottom) / 2;
    // 实测老界面分区行是「三带」布局（逐行墨迹扫描 old1.png y232..272）：
    //   带1 中心 = mid-9（y241.5）  磁盘型号、「已用 / 总」
    //   带2 中心 = mid  （y251  ）  盘符列整列垂直居中、分区号
    //   带3 中心 = mid+9（y261  ）  卷标（无则「—」）、「可用 xx」
    const int l1 = mid - 9;
    const int l2 = mid + 9;
    const int b1 = BaselineOf(l1, 12);
    const int b2 = BaselineOf(l2, 12);
    const int bm = BaselineOf(mid, 13);

    // ── 列1：盘符(粗) + 分区表 + 文件系统 + 角色标签 ──
    int x = rc.left + kCol1;
    if (!m_letter.IsEmpty()) {
        TextAt(hDC, m_letter, x, bm, 13, C_TEXT, true);
        x += TextW(hDC, m_letter, 13, true) + 6;
    }
    if (!m_ptype.IsEmpty()) {
        TextAt(hDC, m_ptype, x, bm, 12, C_TEXT, true);
        x += TextW(hDC, m_ptype, 12, true) + 14;
    }
    if (!m_fs.IsEmpty()) {
        // 实测：系统分区的文件系统也走绿色 TypeBrush（NTFS 与「当前系统」同色）
        TextAt(hDC, m_fs, x, bm, 13, m_sys ? C_GREEN : C_TEXT, true);
    }
    if (!m_tag.IsEmpty()) {
        TextAt(hDC, m_tag, rc.left + kTagX, bm, 13, C_GREEN, true);
    }

    // ── 列2：磁盘型号（灰） / 卷标（深，无则「—」）──
    if (!m_disk.IsEmpty()) TextAt(hDC, m_disk, rc.left + kCol2, b1, 12, C_SECOND);
    TextAt(hDC, m_sub.IsEmpty() ? Tr(L"—") : m_sub.GetData(), rc.left + kCol2, b2, 13, C_TEXT);

    // ── 列3：磁盘号/分区号（单行，垂直居中）──
    TextAt(hDC, m_partNo, rc.left + kCol3, BaselineOf(mid, 13), 13, C_TEXT);

    // ── 列4：用量条 + 已用/总 + 可用 ──
    RECT bar = {rc.left + kCol4, l1 - kBarH / 2, rc.left + kCol4 + kBarW, l1 - kBarH / 2 + kBarH};
    FillRound(hDC, bar, kBarH / 2, C_TRACK);
    if (m_pct > 0) {
        int w = kBarW * m_pct / 100;
        if (w < kBarH) w = kBarH;
        FillRound2(hDC, bar.left, bar.top, bar.left + w, bar.bottom, kBarH / 2, C_BAR);
    }
    if (!m_cap.IsEmpty()) TextAt(hDC, m_cap, rc.left + kCapX, b1, 12, C_TEXT);
    if (!m_free.IsEmpty()) TextAt(hDC, m_free, rc.left + kCol4, b2, 12, C_SECOND);

    // ── 收起框右侧 chevron（下拉列表里不画）──
    if (collapsed) {
        POINT ch[3] = {{rc.left + kChev, mid - 2},
                       {rc.left + kChev + 4, mid + 2},
                       {rc.left + kChev + 9, mid - 2}};
        Poly(hDC, ch, 3, 1.4f, C_CHEV);
    }
}

// ═════════════════════════ CTextItemUI ═════════════════════════

void CTextItemUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("text")) == 0) {
        SetText(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("align")) == 0) {
        // 本类自己拦下 align（不传基类）：只管收起框文字对齐，别让
        // CLabelUI 的文本样式位影响 DoPaint 里我们自己的画法。
        m_center = (_tcscmp(pstrValue, _T("center")) == 0);
        return;
    }
    CListContainerElementUI::SetAttribute(pstrName, pstrValue);
}

bool CTextItemUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    if (IsSelected()) {
        FillRound(hDC, m_rcItem, 6, 0xFFEDF2FF);
    } else if ((m_uButtonState & UISTATE_HOT) != 0) {
        FillRound(hDC, m_rcItem, 6, 0xFFF7F9FC);
    }
    const RECT rc = m_rcItem;
    TextAt(hDC, GetText(), rc.left + kItemPadX, BaselineOf((rc.top + rc.bottom) / 2, 13), 13, C_TEXT);
    return true;
}

void CTextItemUI::DrawItemText(HDC hDC, const RECT& rcItem) {
    if (m_center) {   // 按钮态（CComboUI::PaintText）：在框内水平居中
        TextIn(hDC, GetText(), rcItem, 13, C_TEXT, 1);
        return;
    }
    TextAt(hDC, GetText(), rcItem.left + kItemPadX,
           BaselineOf((rcItem.top + rcItem.bottom) / 2, 13), 13, C_TEXT);
}

// ═════════════════════════ CTitleLabelUI ═════════════════════════

void CTitleLabelUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("sub")) == 0) {
        m_sub = pstrValue;
        return;
    }
    if (_tcscmp(pstrName, _T("fontsize")) == 0) {
        m_px = _ttoi(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("subfontsize")) == 0) {
        m_subPx = _ttoi(pstrValue);
        return;
    }
    CLabelUI::SetAttribute(pstrName, pstrValue);
}

void CTitleLabelUI::PaintText(HDC hDC) {
    if (!IsVisible()) return;
    const RECT rc = m_rcItem;
    const int cy = (rc.top + rc.bottom) / 2;
    const int base = BaselineOf(cy, m_px);  // 标题与副标题同基线（实测对齐）
    const wchar_t* title = GetText();
    const wchar_t* sub = m_sub.GetData();

    int w1 = TextW(hDC, title, m_px, true);
    int w2 = (sub && *sub) ? TextW(hDC, sub, m_subPx, false) : 0;
    const int gap = (w2 > 0) ? 9 : 0;  // 实测 标题墨迹止于 x568 → v0.1 起 x583
    int x = rc.left + (rc.right - rc.left - (w1 + gap + w2)) / 2 - 50;  // 整组左移 50px（= 居中基础上右侧让空当；不用尾随空格：字体回退会变宽）

    TextAt(hDC, title, x, base, m_px, C_TITLE_FG, true);
    if (w2 > 0) TextAt(hDC, sub, x + w1 + gap, base, m_subPx, C_TITLE_SUB);
}

// ═════════════════════════ CSkinLabelUI ═════════════════════════

void CSkinLabelUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("fontsize")) == 0) {
        m_px = _ttoi(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("color")) == 0) {
        m_color = ParseColor(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("bold")) == 0) {
        m_bold = (_tcscmp(pstrValue, _T("true")) == 0 || _tcscmp(pstrValue, _T("1")) == 0);
        return;
    }
    if (_tcscmp(pstrName, _T("align")) == 0) {
        if (_tcscmp(pstrValue, _T("center")) == 0) m_align = 1;
        else if (_tcscmp(pstrValue, _T("right")) == 0) m_align = 2;
        else m_align = 0;
        return;
    }
    CControlUI::SetAttribute(pstrName, pstrValue);
}

bool CSkinLabelUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    // 带底色/圆角的标签（如「第一步」徽标）自己画底和边；纯文字标签不画任何
    // 底边（老界面的「镜像说明：」「备份备注：」就是无边框标签）
    if (m_dwBackColor != 0 || m_cxyBorderRound.cx > 0) {
        const int radius = m_cxyBorderRound.cx > 0 ? m_cxyBorderRound.cx / 2 : 0;
        if (m_dwBackColor != 0) FillRound(hDC, rc, radius, m_dwBackColor);
        if (m_dwBorderColor != 0) StrokeRound(hDC, rc, radius, 1.0f, m_dwBorderColor);
    }
    TextIn(hDC, GetText(), rc, m_px, m_color, m_align, m_bold);
    return true;
}

// ═════════════════════════ CWindowBtnUI ═════════════════════════

void CWindowBtnUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("glyph")) == 0) {
        if (_tcscmp(pstrValue, _T("max")) == 0) m_glyph = 1;
        else if (_tcscmp(pstrValue, _T("close")) == 0) m_glyph = 2;
        else m_glyph = 0;
        return;
    }
    CButtonUI::SetAttribute(pstrName, pstrValue);
}

bool CWindowBtnUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;  // 实测字形不整体偏移，逐个微调
    const bool hot = (m_uButtonState & UISTATE_HOT) != 0;

    DWORD col = C_GLYPH_LT;
    if (m_glyph == 2 && hot) {
        FillRound2(hDC, rc.left, rc.top, rc.right, rc.bottom, 0, 0xFFE81123);
        col = 0xFFFFFFFF;
    } else if (hot) {
        // 蓝色顶栏上的 hover：半透明白微微提亮
        FillRound2(hDC, rc.left, rc.top, rc.right, rc.bottom, 0, 0x33FFFFFF);
    }

    if (m_glyph == 2) {
        // ✕：实测 x842..852（11 宽）、y23..32（10 高），中心 = 按钮中心 845
        int y0 = cy - 5, y1 = cy + 5;
        Line(hDC, cx - 5, y0, cx + 6, y1, 1.4f, col);
        Line(hDC, cx + 6, y0, cx - 5, y1, 1.4f, col);
    } else if (m_glyph == 1) {
        // □：老图实测 7x7（x803..809 / y26..32），但实测观感偏小，
        // 用户要求「比最小化/关闭大一号」→ 画 9x9，中心仍对齐按钮中心 804
        RECT box = {cx - 4, cy - 2, cx + 5, cy + 7};
        StrokeRound(hDC, box, 0, 1.0f, col);
    } else {
        // —：实测 x758..772（15 宽）、y28 单像素，中心 = 按钮中心 763
        RECT bar = {cx - 7, cy + 1, cx + 8, cy + 2};
        SolidRect(hDC, bar, col);
    }
    return true;
}

// ═════════════════════════ CSkinButtonUI ═════════════════════════

// 每个通道加 delta（用于 hover/pushed 的轻微加深）
static DWORD Tint(DWORD argb, int delta) {
    int a = (int)((argb >> 24) & 0xFF);
    int r = (int)((argb >> 16) & 0xFF) + delta;
    int g = (int)((argb >> 8) & 0xFF) + delta;
    int b = (int)(argb & 0xFF) + delta;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
}

void CSkinButtonUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("fontsize")) == 0) {
        m_px = _ttoi(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("bold")) == 0) {
        m_bold = (_tcscmp(pstrValue, _T("true")) == 0 || _tcscmp(pstrValue, _T("1")) == 0);
        return;
    }
    CButtonUI::SetAttribute(pstrName, pstrValue);
}

bool CSkinButtonUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    const RECT rc = m_rcItem;
    // borderround 是「椭圆宽高」= 直径（见 AGENTS.md §13），半径 = /2
    const int radius = m_cxyBorderRound.cx > 0 ? m_cxyBorderRound.cx / 2 : 0;

    const bool en = IsEnabled();
    DWORD bg = m_dwBackColor ? m_dwBackColor : 0xFFFFFFFF;
    if (en) {
        if ((m_uButtonState & UISTATE_PUSHED) != 0)     bg = Tint(bg, -14);
        else if ((m_uButtonState & UISTATE_HOT) != 0)   bg = Tint(bg, -8);
    }
    FillRound(hDC, rc, radius, bg);
    if (m_dwBorderColor != 0) StrokeRound(hDC, rc, radius, 1.0f, m_dwBorderColor);

    DWORD fg = en ? (m_dwTextColor ? m_dwTextColor : C_TEXT) : C_DIS_TEXT;
    TextIn(hDC, GetText(), rc, m_px, fg, /*halign=*/1, m_bold);
    return true;
}

// ═════════════════════════ CSkinEditUI ═════════════════════════

void CSkinEditUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue) {
    if (_tcscmp(pstrName, _T("fontsize")) == 0) {
        m_px = _ttoi(pstrValue);
        return;
    }
    if (_tcscmp(pstrName, _T("bold")) == 0) {
        m_bold = (_tcscmp(pstrValue, _T("true")) == 0 || _tcscmp(pstrValue, _T("1")) == 0);
        return;
    }
    if (_tcscmp(pstrName, _T("underline")) == 0) {
        m_bUnderline = (_tcscmp(pstrValue, _T("true")) == 0 || _tcscmp(pstrValue, _T("1")) == 0);
        return;
    }
    CEditUI::SetAttribute(pstrName, pstrValue);
}

void CSkinEditUI::ApplyNativeFont() {
    if (m_fontApplied) return;
    HWND hEdit = GetNativeEditHWND();  // 原生 EDIT 子窗口（UIEdit.h:36）
    if (!hEdit) return;                // 还没创建 → 下次 SetPos/DoPaint 再试
    ::SendMessageW(hEdit, WM_SETFONT, (WPARAM)GetFontHandle(m_px, m_bold), TRUE);
    m_fontApplied = true;
}

void CSkinEditUI::SetPos(RECT rc, bool bNeedInvalidate) {
    CEditUI::SetPos(rc, bNeedInvalidate);
    ApplyNativeFont();
}

void CSkinEditUI::DoEvent(TEventUI& event) {
    CEditUI::DoEvent(event);
    // 原生 EDIT 子窗口是「获得焦点/点击时才创建」的（UIEdit.cpp:301/317），
    // 所以每次事件后兜一次：它一出现就登记为拖放目标并挂子类过程。
    EnsureDropTarget();
    // 关键（用户 2026-09-23 反馈"手动输入后鼠标一移开字符就消失"）：
    // 失焦后文本由**控件自绘**（见 DoPaint 的 `!IsFocused()` 分支），用的是控件的
    // GetText()；而手动输入/粘贴进的是**原生 EDIT 子窗口** —— 不同步回来，自绘就画
    // 出空的（视觉上"字符消失"），按钮那边也拿不到路径。每次事件后同步一次。
    SyncTextFromNative();
}

void CSkinEditUI::SyncTextFromNative() {
    HWND h = GetNativeEditHWND();
    if (!h) return;
    int n = ::GetWindowTextLengthW(h);
    if (n <= 0) return;
    std::wstring buf(static_cast<size_t>(n) + 1, L'\0');
    ::GetWindowTextW(h, &buf[0], n + 1);
    buf.resize(static_cast<size_t>(n));
    if (GetText() != CDuiString(buf.c_str()))
        SetText(buf.c_str());
}

void CSkinEditUI::EnsureDropTarget() {
    if (m_dropHooked) return;
    HWND hEdit = GetNativeEditHWND();
    if (!hEdit) return;
    ::SetWindowSubclass(hEdit, &CSkinEditUI::DropSubclassProc, (UINT_PTR)this,
                        (DWORD_PTR)this);
    ::DragAcceptFiles(hEdit, TRUE);
    // 本进程是管理员权限（高完整性），源（资源管理器）是普通权限 → 必须给
    // 这个原生子窗口也放行拖放消息，否则 UIPI 直接拦下，拖上去没反应。
    ::ChangeWindowMessageFilterEx(hEdit, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(hEdit, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ::ChangeWindowMessageFilterEx(hEdit, 0x0049, MSGFLT_ALLOW, nullptr);
    m_dropHooked = true;
}

LRESULT CALLBACK CSkinEditUI::DropSubclassProc(HWND hWnd, UINT uMsg,
                                               WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass,
                                               DWORD_PTR dwRefData) {
    if (uMsg == WM_DROPFILES) {
        // 不在原生 EDIT 里处理：原样转投给顶层窗口（CMainForm 统一处理，
        // 由它 DragQueryFile + DragFinish 释放这个 HDROP）。
        HWND hTop = hWnd;
        while (::GetParent(hTop)) hTop = ::GetParent(hTop);
        ::PostMessageW(hTop, WM_DROPFILES, wParam, 0);
        return 0;
    }
    if (uMsg == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hWnd, &CSkinEditUI::DropSubclassProc, uIdSubclass);
        CSkinEditUI* self = reinterpret_cast<CSkinEditUI*>(dwRefData);
        if (self) self->m_dropHooked = false;
    }
    return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

bool CSkinEditUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl) {
    (void)rcPaint;
    (void)pStopControl;
    if (!IsVisible()) return true;
    ApplyNativeFont();

    const RECT rc = m_rcItem;
    const int radius = m_cxyBorderRound.cx > 0 ? m_cxyBorderRound.cx / 2 : 0;
    if (m_bUnderline) {
        // 下划线式输入框：外框「大多数地方不可见，只显示最下方一行」
        // （与上一行的圆角框区分开，避免抢主题）
        const DWORD line = m_dwBorderColor ? m_dwBorderColor : m_dwLineColor;
        RECT ln = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
        SolidRect(hDC, ln, line);
    } else {
        if (m_dwBackColor != 0) FillRound(hDC, rc, radius, m_dwBackColor);
        if (m_dwBorderColor != 0) StrokeRound(hDC, rc, radius, 1.0f, m_dwBorderColor);
    }

    // 有焦点时文本由原生 EDIT 子窗口显示（字体已 WM_SETFONT 同步）；无焦点时自绘
    if (!IsFocused()) {
        RECT tr = rc;
        tr.left  += m_rcTextPadding.left;
        tr.right -= m_rcTextPadding.right;
        tr.top   += m_rcTextPadding.top;
        tr.bottom -= m_rcTextPadding.bottom;
        TextIn(hDC, GetText(), tr, m_px,
               m_dwTextColor ? m_dwTextColor : C_TEXT, /*halign=*/0, m_bold);
    }
    return true;
}

// ═════════════════════════ 工厂 ═════════════════════════

CControlUI* CreateSkinControl(LPCTSTR pstrClass) {
    if (_tcscmp(pstrClass, _T("TopBar")) == 0)      return new CTopBarUI;
    if (_tcscmp(pstrClass, _T("TabOption")) == 0)   return new CTabOptionUI;
    if (_tcscmp(pstrClass, _T("GlyphCheck")) == 0)  return new CGlyphCheckBoxUI;
    if (_tcscmp(pstrClass, _T("RoundProgress")) == 0) return new CRoundProgressUI;
    if (_tcscmp(pstrClass, _T("PartItem")) == 0)    return new CPartItemUI;
    if (_tcscmp(pstrClass, _T("TextItem")) == 0)    return new CTextItemUI;
    if (_tcscmp(pstrClass, _T("TitleLabel")) == 0)  return new CTitleLabelUI;
    if (_tcscmp(pstrClass, _T("SkinLabel")) == 0)   return new CSkinLabelUI;
    if (_tcscmp(pstrClass, _T("SkinButton")) == 0)  return new CSkinButtonUI;
    if (_tcscmp(pstrClass, _T("SkinEdit")) == 0)    return new CSkinEditUI;
    if (_tcscmp(pstrClass, _T("WinBtn")) == 0)      return new CWindowBtnUI;
    return nullptr;
}
