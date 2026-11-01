"""把 dist/ 打包成"单文件"载荷，追加到 portable stub 后面（PIT-201）。

格式（小端）：
  条目: 'ZJF1' | u32 nameLen | u64 dataLen | u64 mtime | name(UTF-8) | data
  结束: 'ZJEND'
  尾部: 'ZJFT' | u64 payloadStart

用法:
  python tools/portable-pack.py <stub.exe> <out.exe> <distDir>
"""
import os
import struct
import sys

SKIP_DIRS = {'logs'}
SKIP_FILES = {'SHA256SUMS.txt'}


def collect(dist):
    items = []
    for root, dirs, files in os.walk(dist):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for f in sorted(files):
            if f in SKIP_FILES or f.startswith('SysRecover-Portable'):
                continue
            full = os.path.join(root, f)
            rel = os.path.relpath(full, dist).replace('\\', '/')
            items.append((rel, full))
    items.sort()
    return items


def main():
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    stub, out, dist = sys.argv[1], sys.argv[2], sys.argv[3]
    release_dir = sys.argv[4] if len(sys.argv) == 5 else None
    items = collect(dist)
    if not items:
        print('portable-pack: dist is empty')
        return 1

    with open(stub, 'rb') as f:
        stub_bytes = f.read()

    payload = bytearray()
    for rel, full in items:
        with open(full, 'rb') as f:
            data = f.read()
        name = rel.encode('utf-8')
        payload += b'ZJF1'
        payload += struct.pack('<I', len(name))
        payload += struct.pack('<Q', len(data))
        payload += struct.pack('<Q', int(os.path.getmtime(full)))
        payload += name
        payload += data
    payload += b'ZJEND'

    with open(out, 'wb') as f:
        f.write(stub_bytes)
        start = len(stub_bytes)
        f.write(payload)
        f.write(b'ZJFT')
        f.write(struct.pack('<Q', start))

    total = len(stub_bytes) + len(payload) + 12
    print('portable-pack: %d file(s) -> %s (%.1f MB)' % (len(items), out, total / 1048576.0))

    # 复制一份到发布目录，按版本号命名（如 D:\九转大还原0.9.76.exe）
    if release_dir:
        import shutil
        ver = '0.0.0'
        vj = os.path.join(dist, 'version.json')
        if os.path.exists(vj):
            import json
            with open(vj, 'r', encoding='utf-8') as f:
                ver = json.load(f).get('version', ver)
        name = 'NineTurn-SysRecover-%s.exe' % ver
        zh = '\u4e5d\u8f6c\u5927\u8fd8\u539f%s.exe' % ver
        dst_zh = os.path.join(release_dir, zh)
        try:
            shutil.copyfile(out, dst_zh)
            print('portable-pack: released -> %s' % dst_zh)
        except OSError as e:
            print('portable-pack: release copy FAILED: %s' % e)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
