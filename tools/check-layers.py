#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-layers.py -- 分层依赖门禁（AGENTS §4 依赖 DAG）。

只检查 src/ 下 C++ 源文件的 #include "../../<layer>/..."（以及带 src/
前缀的包含），不执行程序、无第三方依赖。

规则是“允许下层依赖上层吗”的反向表述：
    common 不依赖任何业务层
    disk   只能 common
    wim    只能 common
    boot   只能 common + disk
    app    可以 common/disk/wim/boot
    cli    可以 app + 下层（CLI 需要直接列盘/列镜像）
    gui    可以 app + 下层
    tools  可以 common/disk/boot/wim
禁止下层反向依赖 cli/gui/tools。

用法：python tools/check-layers.py [--verbose]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
LAYERS = ("common", "disk", "wim", "boot", "app", "cli", "gui", "tools")
ALLOWED = {
    "common": set(),
    "disk": {"common"},
    "wim": {"common"},
    "boot": {"common", "disk"},
    "app": {"common", "disk", "wim", "boot"},
    "cli": {"common", "disk", "wim", "boot", "app", "tools"},
    "gui": {"common", "disk", "wim", "boot", "app", "tools"},
    "tools": {"common", "disk", "wim", "boot"},
}
INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def layer_of(path: Path) -> str:
    try:
        return path.relative_to(SRC).parts[0]
    except (ValueError, IndexError):
        return ""


def normalize_include(inc: str) -> str:
    inc = inc.replace("\\", "/")
    # ../common/x.h, ../../src/common/x.h, common/x.h
    if inc.startswith("../"):
        rest = inc[3:]
        if rest.startswith("src/"):
            rest = rest[4:]
        return rest
    if inc.startswith("src/"):
        return inc[4:]
    return inc


def main() -> int:
    verbose = "--verbose" in sys.argv
    bad = []
    for p in sorted(SRC.rglob("*")):
        if p.suffix not in (".cpp", ".h"):
            continue
        layer = layer_of(p)
        if layer not in ALLOWED:
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in INC_RE.finditer(text):
            dep_path = normalize_include(m.group(1))
            dep = dep_path.split("/", 1)[0]
            if dep not in LAYERS or dep == layer:
                continue
            if dep not in ALLOWED[layer]:
                bad.append((str(p.relative_to(ROOT)).replace("\\", "/"),
                            dep, m.group(1)))
    if verbose:
        print("layers checked:", ", ".join(LAYERS))
    if bad:
        print("check-layers: FAIL (%d violation(s))" % len(bad))
        for rel, dep, inc in bad:
            print("  %s -> %s  (%s)" % (rel, dep, inc))
        return 1
    print("check-layers: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
