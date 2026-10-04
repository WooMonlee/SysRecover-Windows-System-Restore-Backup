#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""i18n-wrap.py — 把面向用户的中文字面量机械化包上 Tr(...)（PLAN §14 M2 回填）。

用法：
  python tools/i18n-wrap.py                # 演练（默认）：列出将要做的改写
  python tools/i18n-wrap.py --apply        # 落盘（LF/BOM 原样保留；幂等，可重复跑）
  python tools/i18n-wrap.py --dump-keys    # 汇总词条（源码 Tr() + 皮肤 XML）→ TSV
  python tools/i18n-wrap.py --gen-lang     # tools/i18n-en.py 译文表 → lang/en.lang

设计（AGENTS 国际化纪律 / PLAN §14）：
  * 词法状态机（跳注释，字符串里的 // 不误判）；CJK 判据 **import 自
    extract-strings.py** —— 与账本同一把尺子，防漂移；
  * 相邻字符串字面量归并成 run（间隙只许空白与 `+`；`+` 改写时去掉，改用
    C++ 字面量隐式拼接）；run 含 CJK 才包；
  * 从 run 首 token 括号回溯找调用名：日志/机器可读出口（Log*/AppendHistory/
    Progress*）一律跳过 —— 契约与日志不翻；
  * `_T("中文")`/`TEXT("中文")` → `Tr(L"中文")`（宏体里不能再套函数调用）；
  * 幂等：callee=Tr 跳过 —— 重复 --apply 不会二次改写；
  * 只改 WRAP_FILES 白名单（契约文件 boot/task.cpp 等天然不在其内）。
"""
import argparse
import bisect
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# CJK 判据复用账本工具（同一把尺子）
exs = _load("extract_strings", Path(__file__).resolve().parent / "extract-strings.py")
has_cjk = exs.has_cjk

# 回填白名单 = 账本 ui 桶的全部来源文件（契约 boot/task.cpp 等不在其内）
WRAP_FILES = [
    "src/gui/main_form.cpp",
    "src/gui/main_win.cpp",
    "src/gui/confirm_dlg.cpp",
    "src/gui/instance_dlg.cpp",
    "src/gui/ui_skin.cpp",
    "src/cli/main.cpp",
    "src/app/ops.cpp",
    "src/app/safety.cpp",
    "src/app/advice.cpp",
    "src/common/vss.cpp",
    "src/common/relocate.cpp",
    "src/common/cpucap.cpp",
    "src/app/selfdiag.cpp",
    "src/common/sysinfo.cpp",
    "src/boot/uefi.cpp",
    "src/boot/grub.cpp",
    "src/wim/wim.cpp",
]
SKIN_FILES = ["skin/main.xml", "skin/confirm.xml", "skin/instance.xml"]

# 日志 / 历史 / 进度（机器可读，不翻）
SKIP_CALLS = {
    "Log", "LogInfo", "LogWarn", "LogError", "LogDebug", "LogVerbose",
    "AppendHistory", "ProgressUpdate", "ProgressDone", "ProgressInit",
}
MACRO_WRAP = ("_T", "TEXT")  # 宏形态的字面量 → Tr(L"...")
GAP_OK = re.compile(r"[\s+]*(?:u8|L|u|U)?[\s+]*\Z")


def decode_cpp(tok_text):
    """C++ 字面量 → 运行时值（词条 key 即它）。tok_text 含前缀与两端引号。"""
    q = tok_text.find('"')
    inner = tok_text[q + 1:]
    if inner.endswith('"'):
        inner = inner[:-1]
    out = []
    i = 0
    while i < len(inner):
        c = inner[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        if i + 1 >= len(inner):
            out.append("\\")
            break
        e = inner[i + 1]
        if e == "n":
            out.append("\n")
        elif e == "r":
            out.append("\r")
        elif e == "t":
            out.append("\t")
        elif e == "\\":
            out.append("\\")
        elif e == '"':
            out.append('"')
        elif e == "'":
            out.append("'")
        elif e == "0":
            out.append("\0")
        elif e == "\n":
            pass  # 行尾续行
        else:
            out.append("\\")
            out.append(e)
        i += 2
    return "".join(out)


def lang_escape(s):
    r"""运行时值 → .lang 单行形态（与 i18n.cpp 的 Unescape 互逆）。

    `=` 也要转义：格式是"首个**未转义**的 ="分割，而 `备份失败(rc=%d): `
    这类词条**键**本身含 '='（值里含 '=' 的更多），不转义会让解析器
    把键截断。必须放在反斜杠转义**之后**，否则新插入的 `\` 会被二次转义。
    """
    return (s.replace("\\", "\\\\").replace("\n", "\\n")
             .replace("\r", "\\r").replace("\t", "\\t")
             .replace("=", "\\="))


def lex(src):
    """→ [(kind, start, end, is_wide)]；kind: str/comment/raw/char/ident/num/punct"""
    toks = []
    n = len(src)
    i = 0

    def scan_str(q):
        j = q + 1
        while j < n:
            c = src[j]
            if c == "\\":
                j += 2
                continue
            if c == '"':
                return j + 1
            if c == "\n":
                return j  # 未闭合：止损
            j += 1
        return j

    while i < n:
        c = src[i]
        if c in " \t\r\n":
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            toks.append(("comment", i, j, False))
            i = j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            toks.append(("comment", i, j, False))
            i = j
            continue
        if c == "R" and src.startswith('R"', i):
            j = src.find("(", i + 2)
            if j > 0:
                delim = src[i + 2:j]
                endmark = ")" + delim + '"'
                k = src.find(endmark, j)
                end = n if k < 0 else k + len(endmark)
                toks.append(("raw", i, end, False))
                i = end
                continue
            toks.append(("punct", i, i + 1, False))
            i += 1
            continue
        q = -1
        wide = False
        if c == '"':
            q = i
        elif src.startswith('u8"', i):
            q = i + 2
        elif c in ("L", "u", "U") and i + 1 < n and src[i + 1] == '"':
            q = i + 1
            wide = c == "L"
        if q >= 0:
            end = scan_str(q)          # 扫描从引号起
            toks.append(("str", i, end, wide))   # 跨度从前缀起（保留 L/u8）
            i = end
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'":
                    j += 1
                    break
                if src[j] == "\n":
                    break
                j += 1
            toks.append(("char", i, j, False))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(("ident", i, j, False))
            i = j
            continue
        if c.isdigit():
            j = i + 1
            while j < n and (src[j].isalnum() or src[j] in "._"):
                j += 1
            toks.append(("num", i, j, False))
            i = j
            continue
        toks.append(("punct", i, i + 1, False))
        i += 1
    return toks


def find_runs(text, toks):
    """相邻字符串字面量归并（间隙只许空白与 +，宽窄须一致）；含 CJK 才返回。"""
    runs = []
    cur = []
    for k, t in enumerate(toks):
        if t[0] != "str":
            continue
        if not cur:
            cur = [k]
            continue
        prev = toks[cur[-1]]
        gap = text[prev[2]:t[1]]
        if GAP_OK.match(gap) and t[3] == prev[3]:
            cur.append(k)
        else:
            runs.append(cur)
            cur = [k]
    if cur:
        runs.append(cur)
    return [r for r in runs
            if any(has_cjk(text[toks[i][1]:toks[i][2]]) for i in r)]


def callee_before(text, toks, k):
    """run 首 token 括号回溯：返回 (调用名, 调用名 token 下标)；语句边界/文件头 → (None, None)。"""
    depth = 0
    for j in range(k - 1, -1, -1):
        kind, s, e, _w = toks[j]
        if kind != "punct":
            continue
        c = text[s]
        if c in ")]":
            depth += 1
        elif c in "([":
            if depth == 0:
                m = j - 1
                if m >= 0 and toks[m][0] == "ident":
                    return text[toks[m][1]:toks[m][2]], m
                return None, None
            depth -= 1
        elif c in ";{}" and depth == 0:
            return None, None
    return None, None


def line_starts(text):
    starts = [0]
    i = text.find("\n")
    while i >= 0:
        starts.append(i + 1)
        i = text.find("\n", i + 1)
    return starts


def line_of(starts, pos):
    return bisect.bisect_right(starts, pos)


def plan_file(text, rel):
    """→ (new_text, stats)。stats: wrap/tmacro/skip_log/skip_dup/keys"""
    toks = lex(text)
    starts = line_starts(text)
    stats = {"wrap": 0, "tmacro": 0, "skip_log": 0, "skip_dup": 0,
             "keys": [], "lines": []}
    edits = []  # (start, end, replacement)

    for run in find_runs(text, toks):
        first, last = toks[run[0]], toks[run[-1]]
        callee, cidx = callee_before(text, toks, run[0])
        key = "".join(decode_cpp(text[toks[i][1]:toks[i][2]]) for i in run)
        if callee == "Tr":
            stats["skip_dup"] += 1
            stats["keys"].append(key)
            continue
        if callee in SKIP_CALLS:
            stats["skip_log"] += 1
            continue
        if callee in MACRO_WRAP:
            # _T("中文") → Tr(L"中文")：callee 在 cidx，结构为 callee ( run )
            if cidx is None or cidx + 1 >= len(toks):
                raise SystemExit(f"{rel}:{line_of(starts, first[1])}: bad _T callee")
            ps, _pe, _pw = toks[cidx + 1][1], toks[cidx + 1][2], None
            if toks[cidx + 1][0] != "punct" or text[ps] != "(":
                raise SystemExit(
                    f"{rel}:{line_of(starts, first[1])}: unexpected _T structure")
            depth = 1
            end_e = None
            for j in range(run[0] + 1, len(toks)):
                kind, s, e, _w = toks[j]
                if kind == "punct" and text[s] == "(":
                    depth += 1
                elif kind == "punct" and text[s] == ")":
                    depth -= 1
                    if depth == 0:
                        end_e = e
                        break
            if end_e is None:
                raise SystemExit(f"{rel}:{line_of(starts, first[1])}: _T without closing paren")
            m_start = toks[cidx][1]
            parts = " ".join(text[toks[i][1]:toks[i][2]] for i in run)
            if not any(
                text[toks[i][1]:toks[i][2]].startswith(("L\"", "u8\"", "u\"", "U\""))
                for i in run
            ):
                parts = " ".join("L" + text[toks[i][1]:toks[i][2]] for i in run)
            edits.append((m_start, end_e, "Tr(" + parts + ")"))
            stats["tmacro"] += 1
            stats["keys"].append(key)
            stats["lines"].append(line_of(starts, first[1]))
            continue
        # 普通调用：整 run 改写（间隙里的 + 一并去掉 → 隐式拼接）
        if not text[first[1]:last[2]].startswith(('"', 'L"', 'u8"', 'u"', 'U"')):
            raise SystemExit(f"{rel}:{line_of(starts, first[1])}: run not a literal? "
                             f"{text[first[1]:first[1]+20]!r}")
        parts = " ".join(text[toks[i][1]:toks[i][2]] for i in run)
        ins = "Tr(" + parts + ")"
        edits.append((first[1], last[2], ins))
        stats["wrap"] += 1
        stats["keys"].append(key)
        stats["lines"].append(line_of(starts, first[1]))

    if not edits:
        return text, stats
    edits.sort(key=lambda e: e[0])
    out = []
    pos = 0
    for s, e, r in edits:
        if s < pos:
            raise SystemExit(f"{rel}: overlapping edits at {s}")
        out.append(text[pos:s])
        out.append(r)
        pos = e
    out.append(text[pos:])
    return "".join(out), stats


def ensure_headers(text, rel):
    """文件头部注入 i18n include + using（PIT-012：include 必须在最前）。"""
    if "common/i18n.h" in text:
        return text, False
    toks = lex(text)
    inc = None
    for k, (kind, s, e, _w) in enumerate(toks):
        if kind == "punct" and text[s] == "#":
            if k + 1 < len(toks) and toks[k + 1][0] == "ident" \
                    and text[toks[k + 1][1]:toks[k + 1][2]] == "include":
                inc = toks[k][1]
                break
    if inc is None:
        raise SystemExit(f"{rel}: no #include found")
    # include 风格：文件内已有 ../common/ 用 ../，否则用 common/
    style = "../common/" if "#include \"../common/" in text else "common/"
    line = f'#include "{style}i18n.h"\n'
    text = text[:inc] + line + text[inc:]
    # using sysrecover::Tr; —— 无 namespace sysrecover 且无 using namespace sysrecover
    if "namespace sysrecover {" not in text and "using namespace sysrecover;" not in text:
        idx = text.find("\n#include")
        if idx >= 0:
            # 放在最后一个 #include 行之后
            pos = idx
            while True:
                nxt = text.find("\n#include", pos + 1)
                if nxt < 0:
                    break
                pos = nxt
            end = text.find("\n", pos)
            text = text[:end + 1] + "using sysrecover::Tr;\n" + text[end + 1:]
    return text, True


def read_text(path):
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    if bom:
        raw = raw[3:]
    return bom, raw.decode("utf-8"), raw


def write_text(path, bom, text):
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    path.write_bytes(data)


def collect_keys(root=ROOT):
    """词条全集（供 check-i18n.py 复用）：源码 Tr() 个数 + 皮肤 XML 键。"""
    keys = set()
    for rel in WRAP_FILES:
        p = root / rel
        if not p.exists():
            continue
        _, text, _ = read_text(p)
        toks = lex(text)
        for run in find_runs(text, toks):
            callee, _cidx = callee_before(text, toks, run[0])
            if callee == "Tr":
                # 整个 run 拼起来才是一个键（与 plan_file 的 key 口径一致）
                keys.add("".join(
                    decode_cpp(text[toks[i][1]:toks[i][2]]) for i in run))
    for rel in SKIN_FILES:
        p = root / rel
        if not p.exists():
            continue
        for m in re.finditer(r'"([^"<>]*)"', p.read_text(encoding="utf-8")):
            if has_cjk(m.group(1)):
                keys.add(m.group(1))
    return keys


def gen_lang(root=ROOT):
    spec = importlib.util.spec_from_file_location(
        "i18n_en", str(Path(__file__).resolve().parent / "i18n-en.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    en = getattr(mod, "EN", None)
    if not isinstance(en, dict) or not en:
        raise SystemExit("tools/i18n-en.py: EN dict missing/empty")
    missing = sorted(collect_keys(root) - set(en),
                     key=lambda s: s.encode("utf-8"))
    if missing:
        print(f"ERROR: {len(missing)} key(s) missing in tools/i18n-en.py:")
        for k in missing[:10]:
            print("  " + lang_escape(k))
        return 2
    out = ["# SysRecover en.lang — 词条表（key=中文原文 → value=英文）",
           "# 由 tools/i18n-wrap.py --gen-lang 生成；改词条请改 tools/i18n-en.py 再重新生成。",
           ""]
    for k in sorted(en, key=lambda s: s.encode("utf-8")):
        out.append(f"{lang_escape(k)}={lang_escape(str(en[k]))}")
    dst = root / "lang" / "en.lang"
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(("\n".join(out) + "\n").encode("utf-8"))
    print(f"lang/en.lang: {len(en)} entries "
          f"({len(set(en) - collect_keys(root))} unused)")
    return 0


def skeleton(root=ROOT):
    """生成/合并 tools/i18n-en.py 骨架：repr() 键保证转义正确，译者只填 value。"""
    path = Path(__file__).resolve().parent / "i18n-en.py"
    en = {}
    if path.exists():
        spec = importlib.util.spec_from_file_location("i18n_en", str(path))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        en = dict(getattr(mod, "EN", {}) or {})
    keys = sorted(collect_keys(root), key=lambda s: s.encode("utf-8"))
    done = sum(1 for k in keys if en.get(k))
    lines = [
        '# -*- coding: utf-8 -*-',
        '# SysRecover 英文词条表（PLAN §14 M2）。键 = 中文源文本（键即原文）。',
        '# 用法：python tools/i18n-wrap.py --skeleton   # 补齐新键、保留已译值',
        '#       python tools/i18n-wrap.py --gen-lang   # 生成 lang/en.lang',
        f'# 进度：{done}/{len(keys)} 已译（value 为空 = 待译，--gen-lang 会拦下）。',
        '# 红线：只翻面向用户的文案；日志/契约/救援层屏幕/品牌名不进此表。',
        'EN = {',
    ]
    for k in keys:
        lines.append(f"    {k!r}: {en.get(k, '')!r},")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="")
    print(f"skeleton: {path.name} — {len(keys)} keys, {done} translated")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dump-keys", action="store_true")
    ap.add_argument("--skeleton", action="store_true")
    ap.add_argument("--gen-lang", action="store_true")
    a = ap.parse_args()

    if a.gen_lang:
        return gen_lang()
    if a.skeleton:
        return skeleton()
    if a.dump_keys:
        keys = sorted(collect_keys(), key=lambda s: s.encode("utf-8"))
        dst = ROOT / "build" / "i18n-keys.tsv"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text("\n".join(lang_escape(k) for k in keys) + "\n",
                       encoding="utf-8", newline="")
        print(f"{len(keys)} keys -> build/i18n-keys.tsv (escaped)")
        return 0

    total = {"wrap": 0, "tmacro": 0, "skip_log": 0, "skip_dup": 0}
    changed = []
    for rel in WRAP_FILES:
        p = ROOT / rel
        if not p.exists():
            raise SystemExit(f"missing {rel}")
        bom, text, _ = read_text(p)
        new, st = plan_file(text, rel)
        if new != text:
            new, added = ensure_headers(new, rel)
            for k in total:
                total[k] += st[k]
            changed.append(rel)
            if a.apply:
                write_text(p, bom, new)
                print(f"wrap {rel}: +{st['wrap']} Tr +{st['tmacro']} _T "
                      f"(skip log={st['skip_log']} dup={st['skip_dup']}"
                      f"{' +header' if added else ''})")
    if not a.apply:
        if changed:
            print("DRY-RUN (use --apply):")
            for rel in changed:
                print("  " + rel)
        else:
            print("idempotent: nothing to do")
    print("total:", " ".join(f"{k}={v}" for k, v in total.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
