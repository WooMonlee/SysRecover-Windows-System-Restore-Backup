# -*- coding: utf-8 -*-
"""mk-drill.py - 重建还原演练盘 base/drill.raw（2GB，3 个 FAT32 分区）。

为什么用 Python 而不是 mk-drill.sh：
  mtools 的 `mformat` 用**文件大小**决定卷大小，对多分区磁盘镜像会把整盘当成一个卷
  （分区 FAT 相互覆盖 → sda2/sda3 变 "non DOS media"，grldr 也会丢）。
  正确做法：每个分区**单独**建一个精确大小的镜像文件 → mformat/mcopy → 再拼回磁盘。
  （2026-09-24；原 mk-drill.sh 已因此失效，另它还引用了已删除的 bootfiles/restore.sh。）

布局（512B 扇区）：
  sda1 2048..206847     100MB  引导（grldr.mbr 由 MBR 加载；grldr/menu.lst/vmlinuz/initramfs）
                              + 兼作假 ESP（根下 EFI/ 目录命中 find_esp_dev 的 FAT 回退）
  sda2 206848..1606847  683MB  还原目标（根目录 _zjresy-drill.log = 任务靶子）
  sda3 1606848..4194303 1.2GB  镜像分区（images/test.wim + bootfix/ + restore-task.conf）

用法：python tools/vmtest/mk-drill.py   （先确保 drill-images/test.wim 存在）

EA 修复子镜像（PIT-122）要求 test.wim 里带 ZJEA 子镜像（index=3）。git 不保存
NTFS 扩展属性，换机器/重新生成 test.wim 前先给样例文件打 EA（否则 test.wim 会
少一个子镜像，run-drill 的 EA 断言会失败）：
  build\\ea-scan.exe --set tools\\vmtest\\drill-src\\ea-test.txt
  build\\ea-scan.exe --set tools\\vmtest\\drill-src\\DirA\\ea2.bin
  （ea-scan.exe 构建：g++ -O2 -static -municode -o build\\ea-scan.exe tools\\ea-scan.cpp -lntdll）
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
BOOT = os.path.join(ROOT, 'bootfiles')
BASE = os.path.join(HERE, 'base')
IMG = os.path.join(BASE, 'drill.raw')
WIM = os.path.join(HERE, 'drill-images', 'test.wim')
# ZJ_DRILL_INITRD 可指定实验用 initramfs（如强制降级版）；默认产线版
INITRD = os.environ.get('ZJ_DRILL_INITRD') or os.path.join(
    BOOT, 'initramfs-zjrestore.cpio.gz')
SEC = 512
DISK_SECTORS = 2 * 1024 * 1024 * 1024 // SEC

PARTS = [
    (2048, 204800, 'ZJBOOT'),
    (206848, 1400000, 'ZJSYS'),
    (1606848, DISK_SECTORS - 1606848, 'ZJIMG'),
]

LOG_TXT = """action=restore
log_time=2026-09-24 17:00:00
software_version=0.3.0
software_path=D:\\ZJRESTORE\\SysRecover.exe
target_disk_name=QEMU HARDDISK DRILL
target_disk_serial=QEMU-DRILL-0001
target_disk_size=2147483648
target_part_offset=105906176
target_part_size=716800000
target_fs=FAT32
target_vol_label=ZJSYS
image_path=D:/images/test.wim
image_index=1
esp_index=2
ea_index=3
repair_boot=1
pt_type=mbr
"""

CONF_TXT = """action=restore
pt_type=mbr
target_offset=105906176
target_size=716800000
target_disk_serial=QEMU-DRILL-0001
image_path=D:/images/test.wim
image_index=1
esp_index=2
ea_index=3
repair_boot=1
"""

MENU_TXT = ("timeout 3\ndefault 0\ntitle zjrestore-test\n"
            "kernel /vmlinuz-zjrestore console=tty0 console=ttyS0,115200 loglevel=7 "
            "nvme_core.io_timeout=1\ninitrd /initramfs-zjrestore.cpio.gz\n")


def run(*args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write('FAILED: %s\n%s\n' % (' '.join(args), r.stderr.decode('utf-8', 'replace')))
        raise SystemExit(1)


def chs(lba):
    c, h, s = lba // (255 * 63), (lba // 63) % 255, lba % 63 + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h, (s | ((c >> 8) << 6)) & 0xFF, c & 0xFF])


def write_mbr():
    with open(IMG, 'r+b') as f:
        gr = open(os.path.join(BOOT, 'grldr.mbr'), 'rb').read()
        f.seek(0); f.write(gr)                      # 引导代码先写（会覆盖 0..8191）
        off = 446
        for i, (start, length, _lbl) in enumerate(PARTS):
            active = 0x80 if i == 0 else 0x00
            entry = struct.pack('<B3sB3sII', active, chs(start), 0x0C,
                                chs(start + length - 1), start, length)
            f.seek(off); f.write(entry); off += 16
        f.seek(510); f.write(b'\x55\xaa')
    print('MBR: grldr.mbr + %d partitions written' % len(PARTS))


def make_part(part_idx, files, subdirs=()):
    """files: list of (src_abs_path, dst_name)。建精确大小的分区镜像并拼回磁盘。"""
    start, length, label = PARTS[part_idx]
    pimg = os.path.join(BASE, 'part%d.img' % part_idx)
    if os.path.exists(pimg):
        os.remove(pimg)
    run(QIMG, 'create', '-f', 'raw', pimg, str(length * SEC))
    run(MFORMAT, '-i', pimg, '-F', '-v', label, '::')
    for d in subdirs:
        run(MMD, '-i', pimg, '::' + d)
    for src, dst in files:
        run(MCOPY, '-o', '-i', pimg, src, '::' + dst)
    # 拼回磁盘
    with open(IMG, 'r+b') as disk, open(pimg, 'rb') as p:
        disk.seek(start * SEC)
        while True:
            chunk = p.read(8 << 20)
            if not chunk:
                break
            disk.write(chunk)
    os.remove(pimg)
    print('part%d (%s, %d sectors) -> %d files' % (part_idx, label, length, len(files)))


def main():
    if not os.path.exists(WIM):
        print('missing %s - create it first:' % WIM)
        print('  dist/SysRecover.exe backup --source tools/vmtest/drill-src/ '
              '--dest tools/vmtest/drill-images/test.wim --compress fast --esp --yes')
        print('  (EA samples need EAs first - see the header comment)')
        return 1
    for f in ('grldr', 'grldr.mbr', 'vmlinuz-zjrestore'):
        if not os.path.exists(os.path.join(BOOT, f)):
            print('missing bootfiles/%s' % f); return 1
    os.makedirs(BASE, exist_ok=True)

    if os.path.exists(IMG):
        os.remove(IMG)
    run(QIMG, 'create', '-f', 'raw', IMG, '2G')
    write_mbr()

    menu = os.path.join(BASE, 'menu-drill.lst')
    with open(menu, 'w', newline='\n') as f:
        f.write(MENU_TXT)
    # sda1 兼作"假 ESP"：根下有 EFI/ 目录 → 救援层 find_esp_dev 的 FAT 回退会命中它；
    # 预置 marker 文件用来验证 ESP 子镜像 apply 是**只加不删**（目录模式）。
    marker = os.path.join(BASE, 'esp-marker.txt')
    with open(marker, 'wb') as f:
        f.write(b'DRILL-ESP-MARKER (must survive ESP subimage apply)\n')
    make_part(0, [
        (os.path.join(BOOT, 'grldr'), 'grldr'),
        (menu, 'menu.lst'),
        (os.path.join(BOOT, 'vmlinuz-zjrestore'), 'vmlinuz-zjrestore'),
        (INITRD, 'initramfs-zjrestore.cpio.gz'),
        (marker, 'EFI/DRILL-MARKER.txt'),
    ], subdirs=('EFI',))

    logf = os.path.join(BASE, '_zjresy-drill.log')
    with open(logf, 'w', newline='\n') as f:
        f.write(LOG_TXT)
    make_part(1, [(logf, '_zjresy-drill.log')])

    # bootfix（生产由 Windows 侧生成；演练造假文件，Linux 侧会 cp 进目标）
    bf = os.path.join(BASE, 'bootfix')
    os.makedirs(os.path.join(bf, 'Boot', 'fonts'), exist_ok=True)
    for rel, content in (('bootmgr', b'FAKE-BOOTMGR'), ('Boot/BCD', b'FAKE-BCD'),
                         ('Boot/fonts/chs_boot.ttf', b'FAKE-FONT')):
        with open(os.path.join(bf, rel.replace('/', os.sep)), 'wb') as f:
            f.write(content)
    conf = os.path.join(BASE, 'restore-task.conf')
    with open(conf, 'w', newline='\n') as f:
        f.write(CONF_TXT)
    # ZJRESTORE/ = 软件目录（log 里 software_path=D:\ZJRESTORE\SysRecover.exe）：
    # 救援层 find_soft_dir 会命中它 → zjrestore-debug.log / zjrestore-apply.out
    # 落盘（回归：日志实时镜像与最终持久化）。
    make_part(2, [
        (WIM, 'images/test.wim'),
        (conf, 'restore-task.conf'),
        (os.path.join(bf, 'bootmgr'), 'bootfix/bootmgr'),
        (os.path.join(bf, 'Boot', 'BCD'), 'bootfix/Boot/BCD'),
        (os.path.join(bf, 'Boot', 'fonts', 'chs_boot.ttf'), 'bootfix/Boot/fonts/chs_boot.ttf'),
    ], subdirs=('images', 'bootfix', 'bootfix/Boot', 'bootfix/Boot/fonts',
                'ZJRESTORE'))

    print('== drill.raw ready: %s (%d bytes) ==' % (IMG, os.path.getsize(IMG)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
