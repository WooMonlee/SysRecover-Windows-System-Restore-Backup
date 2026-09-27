#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-i18n.py — 国际化门禁（PLAN §14 M3），由 `make check` 调用。

五项检查（全过 = 0，任一失败 = 1）：
  C1 源码里没有「该翻却没翻」的中文字面量
     src/**/*.{cpp,h} 的每个含 CJK 字符串 run，callee 必须是 Tr() 或
     SKIP_CALLS（日志/历史/进度，机器可读，按纪律不翻）。
  C2 词条键全覆盖、无死键
     源码 Tr() 键 + skin/*.xml 含 CJK 属性值  ⊆  lang/en.lang，
     且 lang/en.lang 不含源码里已不存在的键（改文案后忘 --gen-lang 会在这里报）。
  C3 英文值里不许残留汉字（漏译/半译）。
  C4 中英 printf 占位符序列一致（顺序与个数，`%%` 不计）——格式化参数对不上会崩溃。
  C5 lang/en.lang 与 tools/i18n-en.py 同步（直接改 .lang 不改 .py 会被抓）。

复用 i18n-wrap.py 的词法/CJK 判据，保证与回填、生成同一把尺子。
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HAN = re.compile(r"[\u4e00-\u9fff\u3400-\u4dbf]")


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


wrap = _load("i18n_wrap", Path(__file__).resolve().parent / "i18n-wrap.py")
has_cjk = wrap.has_cjk

SPEC_RE = re.compile(
    r"%[-+ #0]*\*?(?:\.\*?)?(?:hh|h|l|ll|z|t|L)?"
    r"[diouxXeEfFgGaAcspn%]")


def specs(s):
    return [x for x in SPEC_RE.findall(s) if x != "%%"]


def unescape(s):
    """.lang 行的转义还原（与 i18n.cpp 的 Unescape 同口径）。"""
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({"n": "\n", "r": "\r", "t": "\t",
                        "\\": "\\", "=": "="}.get(c, "\\" + c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def src_files():
    return sorted(ROOT.glob("src/**/*.cpp")) + sorted(ROOT.glob("src/**/*.h"))


def skin_files():
    return sorted(ROOT.glob("skin/*.xml"))


def source_keys_and_violations():
    """→ (键集合, C1 违规列表 [(rel, line, callee, sample)])。"""
    keys, bad = set(), []
    for p in src_files():
        rel = str(p.relative_to(ROOT)).replace("\\", "/")
        _, text, _ = wrap.read_text(p)
        toks = wrap.lex(text)
        starts = wrap.line_starts(text)
        for run in wrap.find_runs(text, toks):
            callee, _c = wrap.callee_before(text, toks, run[0])
            key = "".join(wrap.decode_cpp(text[toks[i][1]:toks[i][2]])
                          for i in run)
            if callee == "Tr":
                keys.add(key)
            elif callee in wrap.SKIP_CALLS:
                continue  # 日志/契约出口：纪律上不翻
            else:
                bad.append((rel, wrap.line_of(starts, toks[run[0]][1]),
                            callee, key))
    for p in skin_files():
        for m in re.finditer(r'"([^"<>]*)"', p.read_text(encoding="utf-8")):
            if has_cjk(m.group(1)):
                keys.add(m.group(1))
    return keys, bad


def split_entry(line):
    """首个**未转义**的 '=' 处分割（`\\` 会转义紧随其后的字符）。"""
    i = 0
    n = len(line)
    while i < n:
        c = line[i]
        if c == "\\":
            i += 2
            continue
        if c == "=":
            return line[:i], line[i + 1:]
        i += 1
    return None, None


def load_lang(path):
    """.lang → dict。口径与 i18n.cpp::LoadTextLocked 一致：只去尾部 \\r、
    空行与首字符 '#' 跳过、首个未转义 '=' 分割 —— **不许 strip**，
    键值本身可以带前导/尾随空格（如 ' 备份' / '    已用 '）。"""
    out, dup = {}, []
    for n, raw in enumerate(
            path.read_text(encoding="utf-8").split("\n"), 1):
        line = raw[:-1] if raw.endswith("\r") else raw
        if not line or line[0] == "#":
            continue
        ks, vs = split_entry(line)
        if ks is None:
            raise SystemExit(f"{path}:{n}: no unescaped '=' in entry")
        k, v = unescape(ks), unescape(vs)
        if k in out:
            dup.append((n, k))
        out[k] = v
    if dup:
        raise SystemExit(f"{path}: duplicate key(s): {dup[:3]}")
    return out


def main():
    errs = []
    lang_path = ROOT / "lang" / "en.lang"
    if not lang_path.exists():
        print("FAIL C0: lang/en.lang missing "
              "(run: python tools/i18n-wrap.py --gen-lang)")
        return 1

    # C1 + 源码键全集
    keys, bad = source_keys_and_violations()
    for rel, ln, callee, sample in bad[:20]:
        errs.append(f"C1 {rel}:{ln}: CJK string not wrapped "
                    f"(callee={callee or '<none>'!r}): {sample[:60]!r}")
    if len(bad) > 20:
        errs.append(f"C1 ... and {len(bad) - 20} more")

    lang = load_lang(lang_path)

    # C2 覆盖 / 死键
    missing = sorted(keys - set(lang), key=lambda s: s.encode("utf-8"))
    for k in missing[:10]:
        errs.append(f"C2 key missing in lang/en.lang: {wrap.lang_escape(k)}")
    if len(missing) > 10:
        errs.append(f"C2 ... and {len(missing) - 10} more missing")
    dead = sorted(set(lang) - keys, key=lambda s: s.encode("utf-8"))
    for k in dead[:10]:
        errs.append(f"C2 dead key in lang/en.lang (no source): "
                    f"{wrap.lang_escape(k)}")
    if len(dead) > 10:
        errs.append(f"C2 ... and {len(dead) - 10} more dead")

    # C3 英文值不得含汉字
    for k, v in lang.items():
        if HAN.search(v):
            errs.append(f"C3 Han left in English value: "
                        f"{wrap.lang_escape(k)[:60]} -> "
                        f"{wrap.lang_escape(v)[:60]}")

    # C4 printf 占位符序列一致
    for k, v in lang.items():
        if specs(k) != specs(v):
            errs.append(f"C4 printf mismatch: key={specs(k)} "
                        f"value={specs(v)} for {wrap.lang_escape(k)[:50]}")

    # C5 .lang 必须由 i18n-en.py 生成（防止直接改 .lang）
    en_mod = _load("i18n_en",
                   Path(__file__).resolve().parent / "i18n-en.py")
    en = getattr(en_mod, "EN", None)
    if not isinstance(en, dict) or not en:
        errs.append("C5 tools/i18n-en.py: EN dict missing/empty")
        en = {}
    else:
        empty = [k for k, v in en.items() if not str(v).strip()]
        for k in empty[:10]:
            errs.append(f"C5 untranslated in tools/i18n-en.py: "
                        f"{wrap.lang_escape(k)}")
        want = {wrap.lang_escape(k): wrap.lang_escape(str(v))
                for k, v in en.items()}
        got = {wrap.lang_escape(k): wrap.lang_escape(v)
               for k, v in lang.items()}
        if want != got:
            diffs = [k for k in set(want) | set(got)
                     if want.get(k) != got.get(k)]
            errs.append(f"C5 lang/en.lang out of sync with tools/i18n-en.py "
                        f"({len(diffs)} entry(ies); run: "
                        f"python tools/i18n-wrap.py --gen-lang)")

    if errs:
        print(f"check-i18n: FAIL ({len(errs)})")
        for e in errs:
            print("  " + e)
        return 1
    print(f"check-i18n: OK ({len(keys)} keys, {len(lang)} lang entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
