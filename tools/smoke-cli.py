#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""smoke-cli.py -- 已构建 CLI 的启动/退出码冒烟测试（零依赖）。

不碰磁盘、不写契约；只跑 version/help/参数错误等无副作用命令：
  * version                 退出 0，输出含语义化版本
  * --lang en version       退出 0
  * help                    退出 0
  * version --help          退出 0
  * 无参数                   退出 2（Usage）
  * 未知命令                 退出 2（Usage）

第二个参数可给「镜像检查」程序（dist/x64/SysRecoverCheck.exe），
再跑一组同样的启动/退出码用例（--help / 未知选项 / 不存在文件 / 无参）。

用法：python tools/smoke-cli.py [dist/x64/SysRecover.exe] [dist/x64/SysRecoverCheck.exe]
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(exe: Path, args: list[str]) -> tuple[int, str]:
    p = subprocess.run([str(exe), *args], cwd=str(ROOT),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=60)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def run_check(exe: Path) -> int:
    """「镜像检查」程序的冒烟（SysRecoverCheck.exe，需求 2 的独立程序）。"""
    if not exe.is_file():
        print("smoke-cli: FAIL missing %s" % exe)
        return 1
    # 不存在的镜像路径：只验证参数/退出码口径，不读任何真实镜像。
    missing = str(ROOT / "build" / "definitely-not-an-image.esd")
    cases = [
        ("check --help", ["--help"], 0),
        ("check unknown option", ["--badopt"], 2),
        ("check no args", [], 2),          # 非独占控制台（管道）→ 用法 + 2
        ("check missing file", ["--quick", missing], 1),
        ("check --lang en help", ["--lang", "en", "--help"], 0),
    ]
    bad = 0
    for name, args, want in cases:
        try:
            rc, out = run(exe, args)
        except Exception as e:
            print("FAIL %-22s exception: %s" % (name, e))
            bad += 1
            continue
        ok = rc == want
        if name == "check --help" and ok:
            ok = "SysRecover" in out
        if name == "check unknown option" and ok:
            ok = "badopt" in out
        if name == "check --lang en help" and ok:
            ok = "Check image" in out and "Usage:" in out
        print("%s %-22s rc=%d want=%d" % ("ok  " if ok else "FAIL", name, rc, want))
        if not ok:
            bad += 1
            print(out[-600:])
    return bad


def main() -> int:
    exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "dist" / "x64" / "SysRecover.exe"
    if not exe.is_file():
        print("smoke-cli: FAIL missing %s" % exe)
        return 1
    cases = [
        ("version", ["version"], 0),
        ("--lang en version", ["--lang", "en", "version"], 0),
        ("help", ["help"], 0),
        ("version --help", ["version", "--help"], 0),
        ("no args", [], 2),
        ("unknown command", ["definitely-not-a-command"], 2),
        ("unknown option", ["version", "--bogus"], 2),
        ("extra arg", ["version", "extra"], 2),
        ("missing value", ["repair-boot", "--disk"], 2),
        ("bad disk", ["restore", "--image", "x", "--disk", "abc", "--part", "1", "--yes"], 2),
        ("bad part", ["restore", "--image", "x", "--disk", "1", "--part", "abc", "--yes"], 2),
        ("bad compress", ["backup", "--dest", "x", "--compress", "bogus"], 2),
        ("bad repair disk", ["repair-boot", "--disk", "abc"], 2),
        ("removed apply", ["restore", "--apply", "--image", "x", "--disk", "1", "--part", "1", "--yes"], 2),
        ("option as value", ["backup", "--name", "--yes"], 2),
        ("history --last 5", ["history", "--last", "5"], 0),
        ("history bad last", ["history", "--last", "abc"], 2),
        ("shortcut startmenu", ["shortcut", "--startmenu"], 2),
        ("list --json", ["list", "--json"], 0),
    ]
    bad = 0
    for name, args, want in cases:
        try:
            rc, out = run(exe, args)
        except Exception as e:
            print("FAIL %-18s exception: %s" % (name, e))
            bad += 1
            continue
        ok = rc == want
        if name == "version" and ok:
            ok = bool(re.search(r"SysRecover\s+\d+\.\d+\.\d+", out))
        if name == "--lang en version" and ok:
            ok = "SysRecover" in out
        if name == "list --json" and ok:
            ok = '"disks"' in out
        print("%s %-18s rc=%d want=%d" % ("ok  " if ok else "FAIL", name, rc, want))
        if not ok:
            bad += 1
            print(out[-600:])
    if bad:
        print("smoke-cli: FAIL (%d)" % bad)
        return 1
    # 第二个参数 = 「镜像检查」程序（可选；给了就一起冒烟）
    if len(sys.argv) > 2:
        bad += run_check(Path(sys.argv[2]))
        if bad:
            print("smoke-cli: FAIL (%d)" % bad)
            return 1
    print("smoke-cli: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
