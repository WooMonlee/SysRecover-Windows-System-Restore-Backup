#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract-strings.py — i18n 字符串账本（PLAN §14 M1.3）。

把**将来要翻译的字符串**从源码里捞出来，产出：
  * 位置清单（file:line）与体量统计（条数 / UTF-8 字节数）
  * 分类：ui（要翻）/ log（纯日志，按约定不翻）/ xml（皮肤文本与属性）/
          comment（注释里的中文，不翻）/ raw（中文出现在字符串与注释之外 —— 异常，要人工看）

用法：
  python tools/extract-strings.py               # 摘要 + 全量清单（stdout）
  python tools/extract-strings.py --summary     # 只打摘要
  python tools/extract-strings.py --tsv out.tsv # 追加导出 TSV（file\tline\tcategory\tbytes\tsnippet）

设计要点：
  * C++ 用**状态机**扫（不是正则）：要正确跳过注释里的引号、字符串里的 // 与转义。
  * 只认 CJK 汉字与中文标点（英文串不列出 —— 它们本来就该保持原文）。
  * M3 的门禁 tools/check-i18n.py 会在本工具之上加"必须已翻译 / 无漏网"的硬校验。
"""
import argparse
import re
import sys
from pathlib import Path

# 汉字 + 中文常用标点（含全角与破折号）；不含 ASCII 与全角数字
CJK = re.compile(
    r"[\u4e00-\u9fff\u3400-\u4dbf"          # 汉字（含扩展A）
    r"\u3000-\u303f"                          # 。，、《》【】〔〕等
    r"\uff01-\uff5e"                          # 全角 !..~
    r"\u2014\u2018\u2019\u201c\u201d\u2026\u00b7]"  # —— ‘’ “” … ·
)

# 这些调用后面跟的中文 = 纯日志（约定不翻，AGENTS 国际化纪律）
LOG_CALLS = ("LogInfo(", "LogWarn(", "LogError(", "LogDebug(", "LogVerbose(", "Log(")


def has_cjk(s):
    return bool(CJK.search(s))


def scan_cpp(path):
    """状态机扫描 .cpp/.h：产出 (line, category, text)。

    category: str | comment | raw
      str     —— 字符串字面量里的中文（再按行前缀细分成 ui/log）
      comment —— 注释里的中文（不翻）
      raw     —— 既不在字符串也不在注释里出现中文（不该发生，报告出来）
    """
    src = path.read_text(encoding="utf-8", errors="replace")
    n = len(src)
    i = 0
    line = 1
    out = []
    # 状态：code / line_comment / block_comment / str / ch / raw_str
    st = "code"
    lit_start_line = 0
    lit = []

    while i < n:
        c = src[i]
        if st == "code":
            if c == "/" and i + 1 < n and src[i + 1] == "/":
                st = "line_comment"
                lit_start_line = line
                lit = []
                i += 2
                continue
            if c == "/" and i + 1 < n and src[i + 1] == "*":
                st = "block_comment"
                lit_start_line = line
                lit = []
                i += 2
                continue
            # 原始字符串 R"delim( ... )delim"
            if c == "R" and i + 1 < n and src[i + 1] == '"':
                j = src.find("(", i + 2)
                if j != -1:
                    delim = src[i + 2 : j]
                    endmark = ")" + delim + '"'
                    k = src.find(endmark, j)
                    if k != -1:
                        body = src[j + 1 : k]
                        if has_cjk(body):
                            out.append((line, "str", body))
                        # 行号补偿
                        line += src[i : k + len(endmark)].count("\n")
                        i = k + len(endmark)
                        continue
            if c in ('"',) or (c == "L" and i + 1 < n and src[i + 1] == '"'):
                if c == "L":
                    i += 1
                st = "str"
                lit_start_line = line
                lit = []
                i += 1
                continue
            if c == "'":
                # 字符字面量：跳过（一般不含中文；遇到也只按 raw 规则处理）
                st = "ch"
                i += 1
                continue
            if has_cjk(c):
                out.append((line, "raw", c))
            if c == "\n":
                line += 1
            i += 1
            continue

        if st == "line_comment":
            if c == "\n":
                if has_cjk("".join(lit)):
                    out.append((lit_start_line or line, "comment", "".join(lit)))
                lit = []
                st = "code"
                line += 1
            else:
                if len(lit) < 4000:
                    lit.append(c)
            i += 1
            continue

        if st == "block_comment":
            if c == "*" and i + 1 < n and src[i + 1] == "/":
                if has_cjk("".join(lit)):
                    out.append((lit_start_line or line, "comment", "".join(lit)))
                lit = []
                st = "code"
                i += 2
                continue
            if c == "\n":
                line += 1
                # 注释块跨行：把整块攒在一起（丢掉换行，保持可读）
                lit.append(" ")
            elif len(lit) < 4000:
                lit.append(c)
            i += 1
            continue

        if st == "str":
            if c == "\\" and i + 1 < n:
                lit.append(c)
                lit.append(src[i + 1])
                if src[i + 1] == "\n":
                    line += 1
                i += 2
                continue
            if c == '"':
                text = "".join(lit)
                if has_cjk(text):
                    out.append((lit_start_line, "str", text))
                lit = []
                st = "code"
                i += 1
                continue
            if c == "\n":
                line += 1
            lit.append(c)
            i += 1
            continue

        if st == "ch":
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == "'":
                st = "code"
            elif c == "\n":
                line += 1
                st = "code"
            i += 1
            continue

    return out


def classify_line(src_text, lit_line):
    """同一行里若已有 Log* 调用 → 该字符串归 log（不翻）。"""
    lines = src_text.splitlines()
    if 1 <= lit_line <= len(lines):
        head = lines[lit_line - 1]
        for k in LOG_CALLS:
            if k in head:
                return "log"
    return "ui"


def scan_cpp_with_class(path):
    src = path.read_text(encoding="utf-8", errors="replace")
    rows = scan_cpp(path)
    out = []
    for lineno, kind, text in rows:
        if kind == "str":
            cat = classify_line(src, lineno)
        elif kind == "comment":
            cat = "comment"
        else:
            cat = "raw"
        out.append((lineno, cat, text))
    return out


XML_ATTR = re.compile(r'=\s*"([^"]*)"')
XML_TEXT = re.compile(r">([^<>]*?)<")
XML_COMMENT = re.compile(r"<!--.*?-->", re.S)  # 注释里的中文不翻 —— 显式剥掉，别靠正则碰运气


def scan_xml(path):
    out = []
    text = XML_COMMENT.sub(
        lambda m: "\n" * m.group(0).count("\n"),
        path.read_text(encoding="utf-8", errors="replace"),
    )  # 只挖掉注释内容、保留换行 → 后面行号不漂移
    for ln, raw in enumerate(text.splitlines(), 1):
        # 属性值
        for m in XML_ATTR.finditer(raw):
            if has_cjk(m.group(1)):
                out.append((ln, "xml-attr", m.group(1)))
        # 文本节点
        for m in XML_TEXT.finditer(raw):
            if has_cjk(m.group(1)):
                out.append((ln, "xml-text", m.group(1)))
    return out


def main():
    ap = argparse.ArgumentParser(description="i18n 字符串账本（PLAN §14 M1.3）")
    ap.add_argument("--root", help="仓库根目录（默认：脚本上级目录）")
    ap.add_argument("--summary", action="store_true", help="只打摘要")
    ap.add_argument("--tsv", help="把清单写到 TSV")
    args = ap.parse_args()

    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    rows = []  # (file, line, category, bytes, text)
    for pat in ("src/**/*.cpp", "src/**/*.h"):
        for p in sorted(root.glob(pat)):
            for lineno, cat, text in scan_cpp_with_class(p):
                rows.append(
                    (str(p.relative_to(root)), lineno, cat, len(text.encode("utf-8")), text)
                )
    for p in sorted(root.glob("skin/*.xml")):
        for lineno, cat, text in scan_xml(p):
            rows.append(
                (str(p.relative_to(root)), lineno, cat, len(text.encode("utf-8")), text)
            )

    # 分类汇总
    def bucket(pred):
        sel = [r for r in rows if pred(r)]
        return len(sel), sum(r[3] for r in sel)

    n_ui, b_ui = bucket(lambda r: r[2] == "ui")
    n_log, b_log = bucket(lambda r: r[2] == "log")
    n_xmla, b_xmla = bucket(lambda r: r[2] == "xml-attr")
    n_xmlt, b_xmlt = bucket(lambda r: r[2] == "xml-text")
    n_cm, b_cm = bucket(lambda r: r[2] == "comment")
    n_raw, b_raw = bucket(lambda r: r[2] == "raw")

    print("extract-strings: %s" % root)
    print(
        "  ui (cpp 字面量)   : %4d 条  %6.1f KB" % (n_ui, b_ui / 1024.0)
    )
    print(
        "  xml 皮肤文本/属性 : %4d 条  %6.1f KB  (text %d + attr %d)"
        % (n_xmla + n_xmlt, (b_xmla + b_xmlt) / 1024.0, n_xmlt, n_xmla)
    )
    print("  log (不翻)        : %4d 条  %6.1f KB" % (n_log, b_log / 1024.0))
    print("  comment (不翻)    : %4d 条  %6.1f KB" % (n_cm, b_cm / 1024.0))
    if n_raw:
        print("  !! raw (异常)     : %4d 处  —— 中文出现在字符串/注释之外，需人工检查" % n_raw)
    print(
        "  待翻译合计        : %4d 条  %6.1f KB (UTF-8)"
        % (n_ui + n_xmla + n_xmlt, (b_ui + b_xmla + b_xmlt) / 1024.0)
    )

    if not args.summary:
        print("\n%-34s %-6s %-10s %s" % ("file", "line", "category", "text"))
        for f, ln, cat, _b, t in rows:
            if cat in ("comment",):
                continue
            one = t.replace("\n", "\\n").replace("\t", "\\t")
            if len(one) > 120:
                one = one[:117] + "..."
            print("%-34s %-6d %-10s %s" % (f, ln, cat, one))

    if args.tsv:
        with open(args.tsv, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("file\tline\tcategory\tbytes\ttext\n")
            for f, ln, cat, b, t in rows:
                one = t.replace("\n", "\\n").replace("\t", "\\t")
                fh.write("%s\t%d\t%s\t%d\t%s\n" % (f, ln, cat, b, one))
        print("\nTSV -> %s" % args.tsv)

    # raw 是异常：任何一处都退出码 1（中文不允许出现在字符串/注释之外）
    return 1 if n_raw else 0


if __name__ == "__main__":
    sys.exit(main())
