# -*- coding: utf-8 -*-
"""mk-dual.py - A/B swap 端到端测试盘（双分区，21GiB，纯真实镜像）

布局（512B 扇区，全盘 44040192 扇区 = 21GiB）：
  p1  start=2048       count=29360128   14GiB   type 0x07 + active
      先 mformat 成 FAT32 只为放手写任务日志 _zjresy-dual.log（救援层
      get_task 从**目标分区根**读它；mkntfs 之前日志已进内存，抹掉无妨）。
      type 写 0x07：mkntfs 不改分区表 type，写 0x0C 会让 Windows 把
      NTFS 内容认成 RAW（drill 不进 Windows 所以无所谓，我们 S1.5 要）。
  p2  start=29362176   count=14678016   ~7.0GiB type 0x0C FAT32
      images/b.esd          系统 A 镜像（22H2，工作台，有 PowerShell）
      images/c.esd          系统 B 镜像（精简 3.03GB，换位树最小）
      restore-task.conf     手写契约（S1）
      ZJRESTORE/            软件目录（救援日志 persist 到这里，mtools 可读）

引导：S1 用 QEMU -kernel/-initrd **直启**救援内核（零引导文件、不写 MBR
boot code）。S1.5 宿主挂盘跑 bcdboot + bootsect /mbr 补齐 Windows 引导链，
此后 S3/S3b/S5 走完全真实链（MBR→bootmgr→BCD→…）。

用法：python tools/vmtest/mk-dual.py
"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MB = r'D:\Prog\ProgIDE\msys64\mingw64\bin'
QIMG = os.path.join(MB, 'qemu-img.exe')
MCOPY = os.path.join(MB, 'mcopy.exe')
MFORMAT = os.path.join(MB, 'mformat.exe')
MMD = os.path.join(MB, 'mmd.exe')
ROOT = os.path.dirname(os.path.dirname(HERE))
BASE = os.path.join(HERE, 'base')
IMG = os.path.join(BASE, 'dual.raw')
IMGS = r'D:\zjlab\imgs'                        # 镜像源（b.esd / c.esd，只读）
SEC = 512

DISK_SECTORS = 21 * 1024 * 1024 * 1024 // SEC          # 44040192
P1_START, P1_LEN = 2048, 14 * 1024 * 1024 * 1024 // SEC   # 14GiB
P2_START = P1_START + P1_LEN                             # 29362176
P2_LEN = DISK_SECTORS - P2_START                         # 14678016

DISK_SERIAL = 'QEMU-DUAL-0001'
P1_OFFSET = P1_START * SEC                               # 1048576
P1_SIZE = P1_LEN * SEC                                   # 15032385536
DISK_SIZE = DISK_SECTORS * SEC                           # 22548578304

LOG_TXT = """action=restore
log_time=2026-10-17 15:00:00
software_version=0.7.56
software_path=D:\\ZJRESTORE\\SysRecover.exe
target_disk_name=QEMU HARDDISK DUAL
target_disk_serial=%s
target_disk_size=%d
target_part_offset=%d
target_part_size=%d
target_fs=FAT32
target_vol_label=ZJSYS
image_path=D:/images/b.esd
image_index=1
esp_index=0
ea_index=0
repair_boot=1
pt_type=mbr
""" % (DISK_SERIAL, DISK_SIZE, P1_OFFSET, P1_SIZE)

CONF_TXT = """action=restore
pt_type=mbr
target_offset=%d
target_size=%d
target_disk_serial=%s
image_path=D:/images/b.esd
image_index=1
esp_index=0
ea_index=0
repair_boot=1
""" % (P1_OFFSET, P1_SIZE, DISK_SERIAL)


def run(*args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write('FAILED: %s\n%s\n' % (
            ' '.join(args), r.stderr.decode('utf-8', 'replace')))
        raise SystemExit(1)


def chs(lba):
    c, h, s = lba // (255 * 63), (lba // 63) % 255, lba % 63 + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h, (s | ((c >> 8) << 6)) & 0xFF, c & 0xFF])


def write_partition_table():
    """MBR = 零 boot code + 分区表 + 55AA（boot code 由 S1.5 bootsect 补）。"""
    with open(IMG, 'r+b') as f:
        f.seek(0)
        f.write(b'\x00' * 446)
        off = 446
        parts = [
            (P1_START, P1_LEN, 0x07, 0x80),   # p1 NTFS active
            (P2_START, P2_LEN, 0x0C, 0x00),   # p2 FAT32 LBA
        ]
        for start, length, ptype, active in parts:
            entry = struct.pack('<B3sB3sII', active, chs(start), ptype,
                                chs(start + length - 1), start, length)
            f.seek(off)
            f.write(entry)
            off += 16
        f.seek(446 + 32)
        f.write(b'\x00' * 16)                 # 空槽清零
        f.seek(510)
        f.write(b'\x55\xaa')
    print('MBR: zero boot code + %d-partition table' % len(parts))


def sparse_copy(dst_img, src_path, dst_offset):
    """把 src 内容按非零块写进 dst_img@offset（避免把稀疏文件写实）。"""
    with open(dst_img, 'r+b') as disk, open(src_path, 'rb') as p:
        disk.seek(dst_offset)
        written = 0
        while True:
            chunk = p.read(1 << 20)
            if not chunk:
                break
            if any(chunk):
                disk.seek(dst_offset + written)
                disk.write(chunk)
            written += len(chunk)
    return written


def build_fat_part(part_idx, start, length, label, files, subdirs=()):
    """每分区独立精确大小镜像 -> mformat/mcopy -> 稀疏拼回磁盘（PIT-080）。"""
    pimg = os.path.join(BASE, 'part%d.img' % part_idx)
    if os.path.exists(pimg):
        os.remove(pimg)
    run(QIMG, 'create', '-f', 'raw', pimg, str(length * SEC))
    run(MFORMAT, '-i', pimg, '-F', '-v', label, '::')
    for d in subdirs:
        run(MMD, '-i', pimg, '::' + d)
    for src, dst in files:
        run(MCOPY, '-o', '-i', pimg, src, '::' + dst)
    n = sparse_copy(IMG, pimg, start * SEC)
    os.remove(pimg)
    print('part%d (%s, %d sectors) <- %d file(s), %d bytes copied'
          % (part_idx, label, length, len(files), n))


def main():
    for name in ('b.esd', 'c.esd'):
        p = os.path.join(IMGS, name)
        if not os.path.exists(p):
            print('missing %s (copy from D:\\zjlab\\imgs first)' % p)
            return 1
    os.makedirs(BASE, exist_ok=True)
    if os.path.exists(IMG):
        os.remove(IMG)
    run(QIMG, 'create', '-f', 'raw', IMG, str(DISK_SECTORS * SEC))
    write_partition_table()

    logf = os.path.join(BASE, '_zjresy-dual.log')
    with open(logf, 'w', newline='\n') as f:
        f.write(LOG_TXT)
    build_fat_part(1, P1_START, P1_LEN, 'ZJSYS', [(logf, '_zjresy-dual.log')])

    conf = os.path.join(BASE, 'restore-task.conf')
    with open(conf, 'w', newline='\n') as f:
        f.write(CONF_TXT)
    build_fat_part(2, P2_START, P2_LEN, 'ZJIMG', [
        (os.path.join(IMGS, 'b.esd'), 'images/b.esd'),
        (os.path.join(IMGS, 'c.esd'), 'images/c.esd'),
        (conf, 'restore-task.conf'),
    ], subdirs=('images', 'ZJRESTORE'))

    print('== dual.raw ready: %d bytes == ' % os.path.getsize(IMG))
    return 0


if __name__ == '__main__':
    sys.exit(main())
