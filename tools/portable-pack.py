r"""把 dist/ 打包成单文件（PIT-201 / PIT-213）。

**从 0.9.92 起，真正的打包由 exe 自己做**（`SysRecover.exe pack-self`）：
容器载荷用 ntdll 的 XPRESS-Huffman 压缩（约 50MB → 38MB），Python 侧不再重复
实现一份压缩器 —— 两套实现必然漂移。

用法（Makefile 不变）：
  python tools/portable-pack.py <stub.exe> <out.exe> <distDir> [releaseDir]

  stub.exe  : 仅为兼容旧命令行保留（实际用 <distDir>\bootfiles\portable-stub.exe）
  out.exe   : 输出路径
  distDir   : 要打包的目录（= 程序的 AppDir）
  releaseDir: 可选的发布目录（按版本号复制一份，如 D:\九转大还原0.9.92.exe）
"""
import json
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    _stub, out, dist = sys.argv[1], sys.argv[2], sys.argv[3]
    release_dir = sys.argv[4] if len(sys.argv) == 5 else None

    exe = os.path.join(dist, 'x64', 'SysRecover.exe')
    if not os.path.exists(exe):
        exe = os.path.join(dist, 'SysRecover.exe')  # 32 位入口（无 x64 子目录时）
    if not os.path.exists(exe):
        print('portable-pack: SysRecover.exe not found under %s' % dist)
        return 1

    out_abs = os.path.abspath(out)
    r = subprocess.run([exe, 'pack-self', '--out', out_abs])
    if r.returncode != 0 or not os.path.exists(out_abs):
        print('portable-pack: pack-self failed (rc=%d)' % r.returncode)
        return 1
    size = os.path.getsize(out_abs)
    print('portable-pack: %s (%.1f MB)' % (out, size / 1048576.0))

    if release_dir:
        ver = '0.0.0'
        vj = os.path.join(dist, 'version.json')
        if os.path.exists(vj):
            with open(vj, 'r', encoding='utf-8') as f:
                ver = json.load(f).get('version', ver)
        zh = '\u4e5d\u8f6c\u5927\u8fd8\u539f%s.exe' % ver
        dst_zh = os.path.join(release_dir, zh)
        try:
            shutil.copyfile(out_abs, dst_zh)
            print('portable-pack: released -> %s' % dst_zh)
        except OSError as e:
            print('portable-pack: release copy FAILED: %s' % e)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
