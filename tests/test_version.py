#!/usr/bin/env python3
"""version.py 的单元测试（零依赖）。跑法：python tests/test_version.py

只测**纯逻辑**（is_semver / bump / 读写往返），用临时文件，不碰真的 version.h。
"""
import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("zjversion", ROOT / "tools" / "version.py")
v = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(v)

_checks = 0
_fails = 0


def check(cond, msg):
    global _checks, _fails
    _checks += 1
    if not cond:
        _fails += 1
        print("  [FAIL] " + msg)


def eq(a, b, msg):
    check(a == b, "%s -> %r != %r" % (msg, a, b))


# ---- 纯函数 ----
eq(v.is_semver("1.2.3"), True, "is_semver 1.2.3")
eq(v.is_semver("1.2"), False, "is_semver 1.2")
eq(v.is_semver("v1.2.3"), False, "is_semver v1.2.3")
eq(v.is_semver("1.2.3.4"), False, "is_semver 1.2.3.4")
eq(v.is_semver(""), False, "is_semver 空")
eq(v.bump("0.2.0"), "0.2.1", "bump 0.2.0")
eq(v.bump("1.9.9"), "1.9.10", "bump 1.9.9")
eq(v.bump("0.0.0"), "0.0.1", "bump 0.0.0")

# ---- 读写往返（临时文件）----
with tempfile.TemporaryDirectory() as td:
    p = Path(td) / "version.h"
    p.write_text('#pragma once\n#define SYSRECOVER_VERSION "0.2.0"\n',
                 encoding="utf-8", newline="\n")
    eq(v.read_version(p), "0.2.0", "read_version")
    v.write_version("0.3.0", p)
    eq(v.read_version(p), "0.3.0", "write_version")
    raw = p.read_bytes()
    check(b"\r\n" not in raw, "version.h 不应写成 CRLF")
    check(b"#pragma once" in raw, "其余内容应保留")
    v.write_version(v.bump(v.read_version(p)), p)
    eq(v.read_version(p), "0.3.1", "bump 往返")

print("%d checks, %d failures" % (_checks, _fails))
sys.exit(1 if _fails else 0)
