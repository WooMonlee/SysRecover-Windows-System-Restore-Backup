# -*- coding: utf-8 -*-
"""e2e-peek.py - 从 dual.raw 里查看/导出 p2（FAT32）内容

背景：mtools 的 `img@@大偏移`（p2 起点 ≈15GB）在 Windows 上会因 32 位
偏移解析读错位置（"non DOS media"）；而双分区拼接法（PIT-080）把 p2 做成
独立镜像再拼回磁盘，**写入端从不用大偏移**，只有验证/取证需要读。这里把
p2 区域按非零块提取成临时文件（偏移归零），再交给 mtools 正常用。

用法：
  python tools/vmtest/e2e-peek.py ls [提取后目录]
  python tools/vmtest/e2e-peek.py type <盘内路径> [提取后目录]
  python tools/vmtest/e2e-peek.py take <盘内路径> <宿主目标> [提取后目录]
  python tools/vmtest/e2e-peek.py fresh   # 强制重新提取（盘被改过之后）
提取缓存：tools/vmtest/base/_p2.img（与 dual.raw 同步问题：盘写过后须 fresh）
"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MB = r'D:\Prog\ProgIDE\msys64\mingw64\bin'
BASE = os.path.join(HERE, 'base')
RAW = os.path.join(BASE, 'dual.raw')
P2 = os.path.join(BASE, '_p2.img')
SEC = 512
P2_START, P2_LEN = 29362176, 14678016
P2_OFF = P2_START * SEC
P2_SIZE = P2_LEN * SEC


def extract(force=False):
    """把 dual.raw 的 p2 区域按非零块写进 _p2.img（稀疏，快）。"""
    if os.path.exists(P2) and not force:
        r = os.path.getmtime(P2)
        if r >= os.path.getmtime(RAW):
            return False
    with open(RAW, 'rb') as src, open(P2, 'wb') as dst:
        src.seek(P2_OFF)
        off = 0
        while off < P2_SIZE:
            chunk = src.read(min(1 << 20, P2_SIZE - off))
            if any(chunk):
                dst.seek(off)
                dst.write(chunk)
            off += len(chunk)
        dst.truncate(P2_SIZE)
    print('extracted p2 -> %s' % P2)
    return True


def mtool(cmd, args):
    exe = os.path.join(MB, cmd + '.exe')
    r = subprocess.run([exe, '-i', P2] + args, capture_output=True)
    out = (r.stdout + r.stderr).decode('utf-8', 'replace')
    sys.stdout.write(out)
    return r.returncode


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    force = cmd == 'fresh'
    if force:
        cmd = 'ls'
        extract(force=True)
    else:
        extract()

    if cmd == 'ls':
        rest = sys.argv[2:] or ['::']
        # mdir 目录参数：依次列出（mtools 目录风格 ::images/b.esd 等）
        if len(rest) == 1 and not rest[0].startswith('::'):
            rest[0] = '::' + rest[0].lstrip('/\\')
        return mtool('mdir', rest)
    if cmd == 'type':
        p = sys.argv[2]
        return mtool('mtype', ['::' + p.replace('\\', '/').lstrip('/')])
    if cmd == 'take':
        p, dst = sys.argv[2], sys.argv[3]
        return mtool('mcopy', ['-n', '::' + p.replace('\\', '/').lstrip('/'), dst])
    print('unknown cmd: %s' % cmd)
    return 1


if __name__ == '__main__':
    sys.exit(main())
