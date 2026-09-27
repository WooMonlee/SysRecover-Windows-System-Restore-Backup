#!/usr/bin/env python3
"""check-widths.py — 定宽文本控件的多语言宽度门禁（PLAN §14 M3②c），由 `make check` 调用。

背景：`skin/main.xml` 的控件都是 745×410 像素级定死的盒子，而 `ui_skin.cpp::TextIn`
**不做裁剪** —— 英文比中文长 1.5~3 倍，一旦超宽就直接画出控件、压到邻居身上。
本门禁用与渲染一致的 GDI 测宽（`CreateFontW(-px)` = 像素字号，与 app 的
`ui_skin::GetFont` 同源；`GetTextExtentPoint32W` 无 padding，与 `TextIn` 同口径），
逐条比较 **英文** 是否装得下，装不下就 FAIL —— 一条门禁管所有语言的新增词条。

判定规则：
  * budget  = 控件实际可用宽度（默认 = 控件盒子宽；少数控件有覆盖，见 BUDGET）；
  * limit   = max(budget, 中文宽) —— 中文自己就超宽的老控件（如「安装启动还原」93>88）
              不会让门禁永远红（那是历史几何问题，另案处理），但英文不得更差；
  * EN  > limit  → FAIL（必须改译文；译文只能改 tools/i18n-en.py，再 --gen-lang）；
  * ZH  > budget → WARN（中文物理上也放不下，仅提示，不拦提交）。

三类受检对象：
  A. 皮肤控件 `skin/main.xml` 的 `text=`（zh）+ 代码 `Text(_T(name), Tr(L"zh"))` 覆盖（zh/en 均取键原文/译文）；
  B. 状态栏 `SetStatus(...)` 的所有中文键 —— 预算按**静止态**（进度条隐藏）算，
     跑进度时的 179px 窄预算由 `main_form.cpp::FitStatusLine` 运行时掐头省略号兜底，
     不在本门禁里拦（那类本就允许截断）；
  C. `CODE_LABELS` —— 不经皮肤、由自绘代码画死的短文字（如分区行的「当前系统」角色标签）。

用法: python tools/check-widths.py          # 有 FAIL 则退出码 1
      python tools/check-widths.py --verbose # 打印每条的 zh/en/limit 实测值
"""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIN = ROOT / "skin" / "main.xml"
CODE = ROOT / "src" / "gui" / "main_form.cpp"

# ── 预算覆盖（不等于盒子宽的控件）────────────────────────────────────────
# 值的算法写在注释里；未列出的控件一律 budget = 盒子宽 (r-l)。
def _gap_to(pairs: dict, a: str, b: str) -> int:
    return pairs[b][0] - pairs[a][0]  # 右控件左边界 − 左控件左边界


BUDGET_OVERRIDES = {
    # 标题与后面的说明文字贴在一起，标题只能用「到说明文字为止」的间隙
    "PartTitle": lambda pos: _gap_to(pos, "PartTitle", "PartHint"),
    # Tab 左边有图标 + 内边距（icon 24 + pad ≈ 12），文字净空 = 盒宽 − 36
    "ModeRestore": lambda pos: pos["ModeRestore"][2] - pos["ModeRestore"][0] - 36,
    "ModeBackup": lambda pos: pos["ModeBackup"][2] - pos["ModeBackup"][0] - 36,
    # 勾选框（13px 方框 + 间距）占掉 ~24px
    "Silent": lambda pos: pos["Silent"][2] - pos["Silent"][0] - 24,
}

# 状态栏：只按**静止态**（进度条/讨论链接之前）把关
STATUS_BUDGET = None  # 运行时按 SiteLink.left − StatusText.left − 8 算出

# 自绘代码直接画的文字（不在 skin 里，或没有 text=）
CODE_LABELS = {
    "当前系统": 57,  # ui_skin.cpp PartItem 角色标签区（kTagX=120 起，到分区名 x177）
}

FONT = "Microsoft YaHei UI"  # 与 ui_skin.cpp::FaceName 一致（同源同宽）


# ── GDI 测宽 ─────────────────────────────────────────────────────────────
class Measure:
    def __init__(self) -> None:
        import ctypes
        from ctypes import wintypes

        self.g32 = ctypes.windll.gdi32
        self.u32 = ctypes.windll.user32
        self.hdc = self.u32.GetDC(None)
        if not self.hdc:
            raise SystemExit("check-widths: cannot GetDC")
        self._cache: dict = {}

    def __call__(self, s: str, px: int, bold: bool = False) -> int:
        if not s:
            return 0
        key = (s, px, bold)
        if key in self._cache:
            return self._cache[key]
        import ctypes
        from ctypes import wintypes

        f = self.g32.CreateFontW(
            -px, 0, 0, 0, 700 if bold else 400, 0, 0, 0, 1,  # DEFAULT_CHARSET
            0, 0, 0, 0, FONT,
        )
        old = self.g32.SelectObject(self.hdc, f)
        sz = wintypes.SIZE()
        self.g32.GetTextExtentPoint32W(self.hdc, s, len(s), ctypes.byref(sz))
        self.g32.SelectObject(self.hdc, old)
        self.g32.DeleteObject(f)
        self._cache[key] = sz.cx
        return sz.cx

    def close(self) -> None:
        self.u32.ReleaseDC(None, self.hdc)


# ── 读皮肤：控件名 → (盒子, fontsize, bold, 默认 zh) ─────────────────────
def load_skin() -> tuple[dict, dict]:
    import xml.etree.ElementTree as ET

    boxes: dict = {}  # name -> (l,t,r,b)
    meta: dict = {}  # name -> (fontsize, bold, zh_text)
    for el in ET.parse(SKIN).iter():
        name = el.get("name")
        pos = el.get("pos")
        if not name or not pos:
            continue
        try:
            l, t, r, b = (int(x) for x in pos.split(","))
        except ValueError:
            continue
        boxes[name] = (l, t, r, b)
        fs = int(el.get("fontsize") or 14)
        bold = (el.get("bold") or "").lower() == "true"
        meta[name] = (fs, bold, el.get("text") or "")
    return boxes, meta


def load_code_labels() -> dict:
    """main_form.cpp 里 `Text(_T("name"), Tr(L"zh"))` 的覆盖文字。"""
    src = CODE.read_text(encoding="utf-8")
    out = {}
    for m in re.finditer(r'Text\(_T\("([^"]+)"\)\s*,\s*Tr\(L"([^"]+)"\)\)', src):
        out[m.group(1)] = m.group(2)
    return out


def load_status_keys() -> list:
    """所有 `SetStatus(...)` 语句里的 L"..." 中文键（含三元的两条）。"""
    src = CODE.read_text(encoding="utf-8")
    keys = []
    for m in re.finditer(r"SetStatus\((.*?)\);", src, re.S):
        for k in re.findall(r'L"([^"]+)"', m.group(1)):
            if k not in keys:
                keys.append(k)
    return keys


def load_en() -> dict:
    path = ROOT / "tools" / "i18n-en.py"
    spec = importlib.util.spec_from_file_location("i18n_en_width", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.EN


def main() -> int:
    verbose = "--verbose" in sys.argv
    en = load_en()
    boxes, meta = load_skin()
    code_zh = load_code_labels()
    m = Measure()

    fails, warns = [], []

    def judge(label: str, zh: str, en_s: str, budget: int, fs: int, bold: bool) -> None:
        zh_w = m(zh, fs, bold)
        limit = max(budget, zh_w)
        if en_s:
            en_w = m(en_s, fs, bold)
            if en_w > limit:
                fails.append(
                    f"{label}: en {en_w}px > limit {limit}px "
                    f"(budget {budget}, zh {zh_w}px, fs {fs}{'b' if bold else ''})  "
                    f"zh={zh!r}  en={en_s!r}"
                )
        else:
            en_w = 0
        if zh_w > budget:
            warns.append(f"{label}: zh {zh_w}px > budget {budget}px (fs {fs})  {zh!r}")
        if verbose:
            print(
                f"  {label:24s} fs={fs:>2}{'b' if bold else ' '} budget={budget:>3} "
                f"limit={limit:>3} zh={zh_w:>3} en={en_w:>3}  {zh!r} -> {en_s!r}"
            )

    # A. 皮肤控件
    for name, (l, t, r, b) in sorted(boxes.items()):
        fs, bold, zh_xml = meta[name]
        zh = code_zh.get(name) or zh_xml
        if not zh:
            continue
        if name in BUDGET_OVERRIDES:
            budget = BUDGET_OVERRIDES[name](boxes)
        elif name == "StatusText":
            budget = STATUS_BUDGET or (boxes["SiteLink"][0] - l - 8)
        else:
            budget = r - l
        judge(f"skin:{name}", zh, en.get(zh, ""), budget, fs, bold)

    # B. 状态栏 SetStatus 键（静止态预算）
    st = boxes["StatusText"]
    sb = STATUS_BUDGET or (boxes["SiteLink"][0] - st[0] - 8)
    for zh in load_status_keys():
        judge("status", zh, en.get(zh, ""), sb, 13, False)

    # C. 自绘代码文字
    for zh, budget in CODE_LABELS.items():
        judge("code", zh, en.get(zh, ""), budget, 13, False)

    m.close()
    if verbose:
        print()
    for w in warns:
        print(f"check-widths: WARN {w}")
    if fails:
        for f in fails:
            print(f"check-widths: FAIL {f}")
        print(
            f"check-widths: FAIL ({len(fails)}; 改译文：tools/i18n-en.py → "
            f"python tools/i18n-wrap.py --gen-lang)"
        )
        return 1
    print(f"check-widths: OK ({len(warns)} WARN)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
