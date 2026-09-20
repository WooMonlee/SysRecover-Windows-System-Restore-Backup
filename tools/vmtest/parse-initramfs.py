# -*- coding: utf-8 -*-
"""正确解析 newc cpio 归档：filesize=v[6], namesize=v[11]。
用法: python parse-initramfs.py <cpio(未压缩)> <模式> <参数>
模式:
  list            打印全部条目名列表（收敛输出）
  head <path>     打印某路径文件前 200 字节
  cat <path>      完整打印某路径文件
  find <substr>   列出路径含 substr 的条目
"""
import sys

def parse(path):
    data = open(path, 'rb').read()
    # 找第一个 newc 魔数，前面的前缀（如压缩描述头）跳过
    off = data.find(b'070701')
    if off < 0:
        print('no newc magic found')
        return
    entries = []
    pos = off
    n = len(data)
    while pos + 110 <= n:
        if data[pos:pos+6] != b'070701':
            pos += 1
            continue
        hdr = data[pos:pos+110]
        v = [int(hdr[6+i*8:14+i*8], 16) for i in range(13)]
        namesize = v[11]
        filesize = v[6]
        mode = v[1]
        name_off = pos + 110
        name = data[name_off:name_off+namesize-1].decode('utf-8', 'replace')
        hdr_padded = 110 + namesize
        hdr_padded = (hdr_padded + 3) & ~3
        data_off = pos + hdr_padded
        entries.append((name, mode, filesize, data_off))
        if name == 'TRAILER!!!':
            break
        pos = data_off + ((filesize + 3) & ~3)
    return entries, data

def main():
    path, mode = sys.argv[1], sys.argv[2]
    res = parse(path)
    if res is None:
        return
    entries, data = res
    print(f'total entries: {len(entries)}')
    if mode == 'list':
        for name, m, sz, off in entries:
            t = 'dir ' if (m & 0o170000) == 0o040000 else ('lnk ' if (m & 0o170000) == 0o120000 else 'file')
            print(f'{t} {sz:>10} {name}')
    elif mode in ('head', 'cat'):
        target = sys.argv[3]
        for name, m, sz, off in entries:
            if name == target or name == target.lstrip('/'):
                blob = data[off:off+sz]
                if mode == 'head':
                    blob = blob[:200]
                sys.stdout.buffer.write(blob)
                print(f'\n--- [{name}] size={sz} shown={len(blob)} ---')
                return
        print(f'NOT FOUND: {target}')
    elif mode == 'find':
        sub = sys.argv[3]
        for name, m, sz, off in entries:
            if sub in name:
                print(f'{sz:>10} {name}')

if __name__ == '__main__':
    main()
