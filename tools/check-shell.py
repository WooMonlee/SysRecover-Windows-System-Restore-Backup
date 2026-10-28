#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-shell.py — 救援层 shell 门禁（P2，AGENTS PIT-147）

三条规则（全部零依赖、纯 Python）：

  C1 语法   —— bash -n 过一遍全部产品脚本（+ 负例 harness）。
               语法错误必须死在开发机，不能死在客户机的 rescue shell 里。

  C2 棘轮   —— 每个产品文件里 `2>/dev/null` 的出现次数（纯注释行不计）
               不得超过 tools/shell-stderr-baseline.txt 里记录的基线。
               新增消音 = 必须显式改基线（diff 可见 = 人工分类审查过）；
               删减自然通过。防止"随手补一个 2>/dev/null"让静默失败回潮。

  C3 禁消音 —— 关键命令的 stderr 绝不允许 2>/dev/null（wimlib/mkntfs/...），
               这些命令失败必须原样吼出来（按"段首命令"判定：
               `command -v wimlib-imagex 2>/dev/null` 是探测，不算执行）。

分类哲学见 docs/25-PIT坑位册.md PIT-147：
  A（保留）= 探针/尽力而为/有读回校验或响亮兜底的消音；
  B（修掉）= 消音会藏真实故障的 —— 一律改成 WARN/ERROR 或路由到证据文件。

用法：
  python tools/check-shell.py                # 跑门禁
  python tools/check-shell.py --update-baseline   # 分类审查后重设基线（慎用）
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 产品脚本（计入棘轮 + 禁消音 + 语法）
PRODUCT = [
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjswap.sh",
    "bootfiles/zjswap-undo.sh",
    "bootfiles/setup-swap.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/alpine/init",
]
# 语法检查额外覆盖（工具脚本，不计棘轮）
SYNTAX_ONLY = [
    "tools/vmtest/test-caps-failpath.sh",
    "tools/vmtest/test-reparse-awk.sh",
    "tools/vmtest/test-menu-loop.sh",
]
BASELINE_PATH = os.path.join(ROOT, "tools", "shell-stderr-baseline.txt")
TOKEN = "2>/dev/null"

# C3：这些命令失败必须看得见（未修复前不许进名单 —— 名单里的命令
# 当前必须零违例，否则门禁一上线就红）
DENY_CMDS = [
    "wimlib-imagex",
    "mkntfs",
    "ntfsfix",
    "ntfsresize",
    "wipefs",
    "sfdisk",
    "fdisk",
    "ntfs-3g",
]


def find_bash():
    p = shutil.which("bash")
    if p:
        return p
    for cand in (
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
    ):
        if os.path.isfile(cand):
            return cand
    return None


def read_lines(rel):
    path = os.path.join(ROOT, rel)
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8", errors="replace").splitlines()


def is_comment(line):
    return line.lstrip().startswith("#")


def count_token(rel):
    n = 0
    for line in read_lines(rel):
        if is_comment(line):
            continue
        n += line.count(TOKEN)
    return n


# 段首才是被消音的命令：行首/`|`/`&&`/`||`/`;`/`(` 之后（可带 timeout N / ! 前缀）
def _segment_re(cmd):
    return re.compile(
        r"(?:^|[|;&{(])\s*(?:!\s*)?(?:timeout\s+\d+\s+)?" + re.escape(cmd) + r"\b"
    )


def deny_violations(rel):
    """返回 (行号, 命令) 列表：该行消了 stderr 且段首是禁消音命令。"""
    out = []
    for i, line in enumerate(read_lines(rel), 1):
        if is_comment(line) or TOKEN not in line:
            continue
        for cmd in DENY_CMDS:
            if _segment_re(cmd).search(line):
                out.append((i, cmd))
    return out


def load_baseline():
    base = {}
    if not os.path.isfile(BASELINE_PATH):
        return base
    for line in open(BASELINE_PATH, encoding="utf-8"):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit():
            base[" ".join(parts[1:])] = int(parts[0])
    return base


def write_baseline(counts):
    lines = [
        "# check-shell.py C2 棘轮基线（各文件 2>/dev/null 消音次数上限，纯注释行不计）",
        "# 只许降不许升；确需升级 = 先在 AGENTS/PIT 里完成 A/B 分类审查再更新本文件。",
        "# 格式：次数 路径",
        "",
    ]
    for rel in PRODUCT:
        lines.append("%d\t%s" % (counts[rel], rel))
    with open(BASELINE_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    update = "--update-baseline" in sys.argv
    fail = []

    bash = find_bash()
    if not bash:
        print("check-shell: FAIL: bash not found (install Git for Windows)")
        return 1

    # C1 语法
    for rel in PRODUCT + SYNTAX_ONLY:
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            fail.append("missing file: %s" % rel)
            continue
        r = subprocess.run([bash, "-n", path], capture_output=True, text=True)
        if r.returncode != 0:
            fail.append("bash -n %s:\n%s%s" % (rel, r.stdout, r.stderr))

    # C2 棘轮
    counts = {rel: count_token(rel) for rel in PRODUCT}
    if update:
        write_baseline(counts)
        print("check-shell: baseline updated -> tools/shell-stderr-baseline.txt")
    else:
        base = load_baseline()
        if not base:
            fail.append(
                "baseline missing/empty: run --update-baseline after classification review"
            )
        else:
            for rel in PRODUCT:
                cur = counts[rel]
                lim = base.get(rel)
                if lim is None:
                    fail.append("baseline has no entry for %s" % rel)
                elif cur > lim:
                    fail.append(
                        "%s: %d occurrences of %s > baseline %d "
                        "(new suppression must be classified: "
                        "document A/B rationale in a PIT, then --update-baseline)"
                        % (rel, cur, TOKEN, lim)
                    )

    # C3 禁消音
    for rel in PRODUCT:
        for lineno, cmd in deny_violations(rel):
            fail.append(
                "%s:%d: stderr of critical command '%s' must not be silenced (%s)"
                % (rel, lineno, cmd, TOKEN)
            )

    if fail:
        print("check-shell: FAIL (%d)" % len(fail))
        for f in fail:
            print("  - " + f)
        return 1
    print(
        "check-shell: PASS (syntax %d ok; %s; deny-list %s clean)"
        % (
            len(PRODUCT) + len(SYNTAX_ONLY),
            ", ".join("%s=%d" % (os.path.basename(k), v) for k, v in counts.items()),
            len(DENY_CMDS),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
