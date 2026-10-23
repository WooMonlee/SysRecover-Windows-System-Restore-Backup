#!/usr/bin/env python3
"""给发布目录生成 SHA256SUMS.txt（零依赖，仅标准库）。

用法：
    python tools/write-sha256.py [目录] [输出文件]

默认目录 = dist，默认输出 = <目录>/SHA256SUMS.txt。
跳过日志目录与输出文件自身；不含目录项，按相对路径排序。
"""
import hashlib
import os
import sys

SKIP_DIRS = {"logs", "__pycache__"}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "dist"
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "SHA256SUMS.txt")
    root = os.path.abspath(root)
    out = os.path.abspath(out)
    if not os.path.isdir(root):
        print("directory not found: %s" % root, file=sys.stderr)
        return 2

    rows = []
    for base, dirs, files in os.walk(root):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
        for name in sorted(files):
            path = os.path.join(base, name)
            if os.path.normcase(path) == os.path.normcase(out):
                continue
            rel = os.path.relpath(path, root).replace("\\", "/")
            rows.append((rel, sha256(path)))

    with open(out, "w", encoding="utf-8", newline="\n") as f:
        for rel, digest in rows:
            f.write("%s  %s\n" % (digest, rel))
    print("SHA256SUMS: %d file(s) -> %s" % (len(rows), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
