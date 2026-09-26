#!/usr/bin/env python3
"""check-docs.py — 一致性校验（接进 `make check`）。只读不改，不一致即退出码 1。

借自 DreamGrain 的"开工核指针"思路（AGENTS §17）：把**机器可校验**的约定钉死，
避免"改了 A 忘了同步 B"这类静默漂移。检查项：
  1) src/common/version.h 的版本号是合法 SemVer；
  2) SYSRECOVER_CONTRACT_VERSION 与 AGENTS.md / docs 里写的 contract_version 一致；
  3) 救援脚本 `get_task <键>` 读的每个键，Windows 侧（src/boot/task.cpp）都有写
     —— 跨层契约（AGENTS §5）的两端字段对齐；
  4) dist/version.json（若存在）与 version.h 一致。
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_H = ROOT / "src" / "common" / "version.h"
TASK_CPP = ROOT / "src" / "boot" / "task.cpp"
RESCUE = [ROOT / "bootfiles" / "zjrestore-lite.sh",
          ROOT / "bootfiles" / "alpine" / "init"]
DOCS_CONTRACT = [ROOT / "AGENTS.md", ROOT / "docs" / "04-跨层契约.md"]

fails = []


def read(p):
    return p.read_text(encoding="utf-8", errors="replace")


def check_semver(v):
    if not re.fullmatch(r"\d+\.\d+\.\d+", v):
        fails.append("version.h 版本号不是 SemVer: %r" % v)


def main():
    vh = read(VERSION_H)
    m = re.search(r'SYSRECOVER_VERSION\s+"([^"]+)"', vh)
    if not m:
        fails.append("version.h 里找不到 SYSRECOVER_VERSION")
        return report()
    ver = m.group(1)
    check_semver(ver)

    m = re.search(r"SYSRECOVER_CONTRACT_VERSION\s+(\d+)", vh)
    if not m:
        fails.append("version.h 里找不到 SYSRECOVER_CONTRACT_VERSION")
        return report()
    cver = int(m.group(1))

    # 2) contract_version 指针一致
    for p in DOCS_CONTRACT:
        if not p.exists():
            fails.append("缺文档: %s" % p.relative_to(ROOT))
            continue
        for mm in re.finditer(r"contract_version\s*=\s*(\d+)", read(p), re.I):
            if int(mm.group(1)) != cver:
                fails.append("%s: contract_version=%s 与 version.h 的 %d 不一致"
                             % (p.relative_to(ROOT), mm.group(1), cver))

    # 3) 契约键：救援脚本读的 ⊆ Windows 侧写的
    written = set()
    if TASK_CPP.exists():
        # 提取格式串里的 "key=" 键名（BuildTaskConf / BuildRestoreLogText）
        written = set(re.findall(r'"([a-z_]+)=', read(TASK_CPP)))
    if not written:
        fails.append("没能从 task.cpp 提取到契约键（格式串变了吗？）")
    read_keys = set()
    for p in RESCUE:
        if p.exists():
            read_keys |= set(re.findall(r"get_task\s+([a-z_]+)", read(p)))
    missing = sorted(read_keys - written)
    if missing:
        fails.append("救援脚本读了 Windows 侧没写的键: %s" % ", ".join(missing))

    # 4) dist/version.json 与 version.h 一致
    vj = ROOT / "dist" / "version.json"
    if vj.exists():
        mm = re.search(r'"version"\s*:\s*"([^"]+)"', read(vj))
        if mm and mm.group(1) != ver:
            fails.append("dist/version.json=%s 与 version.h=%s 不一致" % (mm.group(1), ver))

    print("OK  version=%s contract_version=%d 契约键 %d 个（救援读 %d 个）"
          % (ver, cver, len(written), len(read_keys)))
    return report()


def report():
    if fails:
        print("check-docs: FAIL")
        for f in fails:
            print("  - " + f)
        return 1
    print("check-docs: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
