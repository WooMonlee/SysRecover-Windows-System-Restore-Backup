#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-secrets.py -- 高置信度密钥/凭据扫描（零依赖，接进 make check）。

只扫 **git 跟踪的文本文件**，避免把本地 dist/、日志、测试盘误报。
只报高置信度模式（私钥头、云厂商 token、GitHub token 等）；不做“看到
password 就报”的模糊规则。二进制/不可解码文件跳过。
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BINARY_EXT = {".exe", ".dll", ".lib", ".a", ".o", ".gz", ".xz", ".zst",
              ".7z", ".zip", ".png", ".jpg", ".ico", ".pdf", ".cpio",
              ".vhd", ".vmdk", ".raw", ".img", ".dmp", ".bin", ".efi",
              ".cer", ".crt", ".key", ".pfx", ".p12"}
PATTERNS = [
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")),
    ("aws-access-key", re.compile(r"\bAKIA[0-9A-Z]{16}\b")),
    ("github-token", re.compile(r"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9_]{36,}\b")),
    ("github-pat", re.compile(r"\bgithub_pat_[A-Za-z0-9_]{20,}\b")),
    ("google-api-key", re.compile(r"\bAIza[0-9A-Za-z_-]{35}\b")),
    ("slack-token", re.compile(r"\bxox[baprs]-[0-9A-Za-z-]{10,}\b")),
    ("openai-key", re.compile(r"\bsk-[A-Za-z0-9]{32,}\b")),
]


def tracked() -> list[Path]:
    try:
        raw = subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT)
        names = [x for x in raw.decode("utf-8", "replace").split("\0") if x]
        return [ROOT / x for x in names]
    except Exception:
        return [p for p in ROOT.rglob("*") if p.is_file()]


def main() -> int:
    bad = []
    for p in tracked():
        if p.suffix.lower() in BINARY_EXT or not p.is_file():
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            for name, rx in PATTERNS:
                if rx.search(line):
                    bad.append("%s:%d: %s" % (
                        str(p.relative_to(ROOT)).replace("\\", "/"), lineno, name))
    if bad:
        print("check-secrets: FAIL (%d hit(s))" % len(bad))
        for x in bad:
            print("  - " + x)
        return 1
    print("check-secrets: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
