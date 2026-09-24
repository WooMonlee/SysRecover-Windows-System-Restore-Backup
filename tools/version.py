#!/usr/bin/env python3
"""SysRecover 版本号工具 —— `src/common/version.h` 是唯一来源，本脚本负责读写它。

用法：
  python tools/version.py                # 打印当前版本（Makefile 用它生成 version.json）
  python tools/version.py --bump         # 修订号 +1（**每解决一个问题**用一次）
  python tools/version.py --set 0.2.0    # 显式设置 主.次.修订（主/次版本由人指定）
  python tools/version.py --check        # 只校验格式

规则见 PLAN.md「版本号规则」与 src/common/version.h 顶部注释。
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "src" / "common" / "version.h"
PAT = re.compile(r'#define\s+SYSRECOVER_VERSION\s+"([^"]+)"')
SEMVER = re.compile(r"\d+\.\d+\.\d+")


def is_semver(v: str) -> bool:
    return bool(SEMVER.fullmatch(v))


def bump(v: str) -> str:
    """修订号 +1（主/次不动）。"""
    major, minor, patch = (int(x) for x in v.split("."))
    return "%d.%d.%d" % (major, minor, patch + 1)


def read_version(path: Path = VERSION_H) -> str:
    m = PAT.search(Path(path).read_text(encoding="utf-8"))
    if not m:
        sys.exit("ERROR: 在 %s 里找不到 SYSRECOVER_VERSION" % path)
    return m.group(1)


def write_version(new: str, path: Path = VERSION_H) -> None:
    path = Path(path)
    text = path.read_text(encoding="utf-8")
    text, n = PAT.subn('#define SYSRECOVER_VERSION "%s"' % new, text, count=1)
    if n != 1:
        sys.exit("ERROR: 写入 %s 失败" % path)
    # 必须用 \n（这是 C++ 源文件，CRLF 无意义；Windows 上文本模式会写成 \r\n）
    path.write_text(text, encoding="utf-8", newline="\n")
    print(new)


def main() -> None:
    ap = argparse.ArgumentParser(description="SysRecover 版本号工具")
    ap.add_argument("--bump", action="store_true", help="修订号 +1")
    ap.add_argument("--set", metavar="X.Y.Z", help="显式设置版本（主/次版本由人指定）")
    ap.add_argument("--check", action="store_true", help="只校验格式")
    a = ap.parse_args()

    cur = read_version()
    if a.check:
        if not is_semver(cur):
            sys.exit("ERROR: 版本号格式不对: " + cur)
        print("OK " + cur)
        return
    if a.set:
        if not is_semver(a.set):
            sys.exit("ERROR: 需要 X.Y.Z 格式，收到: " + a.set)
        write_version(a.set)
        return
    if a.bump:
        write_version(bump(cur))
        return
    print(cur)


if __name__ == "__main__":
    main()
