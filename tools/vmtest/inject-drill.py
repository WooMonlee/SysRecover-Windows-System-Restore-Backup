# -*- coding: utf-8 -*-
"""把测试场地注入文件打进 initramfs。

与旧实现的区别：旧实现直接「截断 base 归档 → 追加」，于是 base 的 /init 与注入的
/init 同路径，内核可能保留先出现的 base init（实测：注入脚本从未执行）。
这里改为「解析 base → 过滤 → 重发 → 追加 stage」，可显式删除要覆盖的路径，
最终产出**单个** newc 归档。

用法: python inject-drill.py <原cpio> <输出cpio> <注入目录> [要删除的路径,逗号分隔]
"""
import sys, os

NEWC = b'070701'


def parse(data):
    pos = data.find(NEWC)
    entries = []
    while pos + 110 <= len(data):
        if data[pos:pos + 6] != NEWC:
            pos += 1
            continue
        hdr = data[pos:pos + 110]
        try:
            v = [int(hdr[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        except ValueError:
            pos += 1
            continue
        mode, filesize, namesize = v[1], v[6], v[11]
        name = data[pos + 110:pos + 110 + namesize - 1].decode('utf-8', 'replace')
        hp = (110 + namesize + 3) & ~3
        doff = pos + hp
        if name == 'TRAILER!!!':
            break
        entries.append((name, mode, data[doff:doff + filesize]))
        pos = doff + ((filesize + 3) & ~3)
    return entries


def newc(name, mode, blob):
    # newc 头字段：ino, mode, uid, gid, nlink, mtime, filesize, devmajor,
    # devminor, rdevmajor, rdevminor, namesize, check
    nm = name.encode() + b'\x00'
    ns, fs = len(nm), len(blob)
    fields = [0, mode, 0, 0, 1, 0, fs, 0, 0, 0, 0, ns, 0]
    hdr = NEWC + b''.join(f'{f:08x}'.encode() for f in fields)
    assert len(hdr) == 110
    hn = 110 + ns
    pn = (4 - hn % 4) % 4
    pd = (4 - fs % 4) % 4
    return hdr + nm + b'\x00' * pn + blob + b'\x00' * pd


def main():
    src, dst, stage = sys.argv[1], sys.argv[2], sys.argv[3]
    drop = set()
    if len(sys.argv) > 4 and sys.argv[4]:
        drop = {x for x in sys.argv[4].split(',') if x}

    data = open(src, 'rb').read()
    out = b''
    kept = 0
    for name, mode, blob in parse(data):
        if name in drop:
            continue
        out += newc(name, mode, blob)
        kept += 1

    files = []
    for root, dirs, fs in os.walk(stage):
        for d in dirs:
            rel = os.path.relpath(os.path.join(root, d), stage).replace(os.sep, '/')
            files.append((rel, 0o040755, b''))
        for f in fs:
            p = os.path.join(root, f)
            rel = os.path.relpath(p, stage).replace(os.sep, '/')
            if f.endswith('.symlink'):
                files.append((rel[:-len('.symlink')], 0o120777,
                              open(p, 'r').read().strip().encode()))
            else:
                files.append((rel, 0o100755, open(p, 'rb').read()))
    files.sort(key=lambda x: (x[0].count('/'), x[0]))
    for n, m, b in files:
        out += newc(n, m, b)
    out += newc('TRAILER!!!', 0, b'')

    open(dst, 'wb').write(out)
    print(f'kept {kept} base entries (dropped={sorted(drop)}), '
          f'injected {len(files)}: {[f[0] for f in files]}')


if __name__ == '__main__':
    main()
