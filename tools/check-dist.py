#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-dist.py -- 发布目录制品门禁（零依赖）。

检查 `make package` 后的 dist/：
  * 必需文件齐全（双架构 exe、救援资产、语言/皮肤/版本）；
  * dist/version.json == src/common/version.h；
  * 不应出现运行期 logs/、*.tmp、*.dmp；
  * 若存在 SHA256SUMS.txt，逐条重算校验。

用法：python tools/check-dist.py [dist]
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REQUIRED = [
    "SysRecover.exe", "SysRecoverUI.exe", "SysRecoverCheck.exe",
    "x64/SysRecover.exe", "x64/SysRecoverUI.exe", "x64/SysRecoverCheck.exe",
    "SysRecoverCheck_x86.exe",
    "bootfiles/vmlinuz-zjrestore",
    "bootfiles/initramfs-zjrestore.cpio.gz",
    "bootfiles/zjrestore-lite.sh",
    "bootfiles/zjlog.sh",
    "bootfiles/zj-ea-apply.exe",
    "bootfiles/zj-restore-boot.exe",
    "lang/en.lang", "skin/main.xml",
    "resources/themes/default/global.xml",
    "version.json",
]


def sha256(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main() -> int:
    dist = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist"
    bad = []
    if not dist.is_dir():
        print("check-dist: FAIL missing %s" % dist)
        return 1
    for rel in REQUIRED:
        if not (dist / rel).is_file():
            bad.append("missing " + rel)
    vh = (ROOT / "src" / "common" / "version.h").read_text(encoding="utf-8")
    m = re.search(r'SYSRECOVER_VERSION\s+"([^"]+)"', vh)
    want = m.group(1) if m else ""
    vj = dist / "version.json"
    if vj.is_file():
        try:
            got = json.loads(vj.read_text(encoding="utf-8")).get("version")
        except Exception as e:
            got = None
            bad.append("version.json parse: %s" % e)
        if want and got != want:
            bad.append("version.json=%r != version.h=%r" % (got, want))
    if (dist / "logs").exists():
        bad.append("runtime directory present: logs/")
    for p in dist.rglob("*"):
        if p.is_file() and (p.name.endswith(".tmp") or p.suffix.lower() == ".dmp"):
            bad.append("runtime artifact: " + str(p.relative_to(dist)))
    sums = dist / "SHA256SUMS.txt"
    if sums.is_file():
        for line in sums.read_text(encoding="utf-8").splitlines():
            m = re.match(r"^([0-9a-fA-F]{64})  (.+)$", line)
            if not m:
                continue
            p = dist / m.group(2)
            if not p.is_file():
                bad.append("SHA256SUMS missing " + m.group(2))
            elif sha256(p) != m.group(1).lower():
                bad.append("SHA256 mismatch " + m.group(2))
    if bad:
        print("check-dist: FAIL")
        for x in bad:
            print("  - " + x)
        return 1
    print("check-dist: PASS (%s)" % dist)
    return 0


if __name__ == "__main__":
    sys.exit(main())
