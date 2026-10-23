# -*- coding: utf-8 -*-
"""mk-swap-drill.py - Build A-engine swap drill disk (base/swap-drill.raw).

Layout (512B sectors, same as mk-drill.py):
  sda1  2048..206847     100MB  boot (FAT32: grldr/menu.lst/vmlinuz/initramfs)
  sda2  206848..1606847  683MB  target (NTFS: old system at root + ~new/ + .zj-done)
  sda3  1606848..END     rest   conf partition (FAT32: restore-task.conf with engine=swap)

NTFS for sda2 is built by mk-swap-ntfs.ps1 (PowerShell Storage module) because
mkntfs isn't available on the host and diskpart output encoding is fragile.

Usage (admin PowerShell required for NTFS VHD step):
  D:/Prog/ProgIDE/Python/Python313/python.exe graveyard/tools/vmtest/mk-swap-drill.py

Then run:
  powershell -ExecutionPolicy Bypass -File graveyard/tools/vmtest/run-swap-drill.ps1
"""
import os
import struct
import subprocess
import sys
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MB = r'D:\Prog\ProgIDE\msys64\mingw64\bin'
QIMG = os.path.join(MB, 'qemu-img.exe')
MCOPY = os.path.join(MB, 'mcopy.exe')
MFORMAT = os.path.join(MB, 'mformat.exe')
MMD = os.path.join(MB, 'mmd.exe')
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
BOOT = os.path.join(ROOT, 'bootfiles')
BASE = os.path.join(HERE, 'base')
IMG = os.path.join(BASE, 'swap-drill.raw')
NTFS_IMG = os.path.join(BASE, 'swap-ntfs.img')
SEC = 512
DISK_SECTORS = 2 * 1024 * 1024 * 1024 // SEC

P1_START, P1_SIZE = 2048, 204800       # 100MB boot FAT32
P2_START, P2_SIZE = 206848, 1400000    # 683MB target NTFS
P3_START, P3_SIZE = 1606848, DISK_SECTORS - 1606848  # rest conf FAT32

CONF_TXT = """action=restore
pt_type=mbr
target_offset=105906176
target_size=716800000
target_disk_serial=QEMU-SWAP-0001
image_path=D:/images/test.wim
image_index=1
esp_index=0
ea_index=0
repair_boot=1
engine=swap
"""

LOG_TXT = """action=restore
log_time=2026-10-09 20:00:00
software_version=0.7.2
software_path=D:\\ZJRESTORE\\SysRecover.exe
target_disk_name=QEMU HARDDISK SWAP
target_disk_serial=QEMU-SWAP-0001
target_disk_size=2147483648
target_part_offset=105906176
target_part_size=716800000
target_fs=NTFS
target_vol_label=ZJSYS
image_path=D:/images/test.wim
image_index=1
esp_index=0
ea_index=0
repair_boot=1
pt_type=mbr
engine=swap
contract_version=1
"""

MENU_TXT = ("timeout 3\ndefault 0\ntitle zjrestore-swap-test\n"
            "kernel /vmlinuz-zjrestore console=tty0 console=ttyS0,115200 loglevel=7 "
            "nvme_core.io_timeout=1 zjsetup=1\ninitrd /initramfs-zjrestore.cpio.gz\n")


def run(*args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write('FAILED: %s\n%s\n%s\n' % (
            ' '.join(args), r.stdout.decode('utf-8', 'replace'),
            r.stderr.decode('utf-8', 'replace')))
        raise SystemExit(1)
    return r


def chs(lba):
    c, h, s = lba // (255 * 63), (lba // 63) % 255, lba % 63 + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h, (s | ((c >> 8) << 6)) & 0xFF, c & 0xFF])


def write_mbr(f, parts):
    """Write partition table entries at offset 446 (boot code already written)."""
    for i, (start, size, ptype, boot) in enumerate(parts):
        off = 446 + i * 16
        f.seek(off)
        mbr = bytearray(16)
        mbr[0] = 0x80 if boot else 0x00
        mbr[1:4] = chs(start)
        mbr[4] = ptype
        mbr[5:8] = chs(start + size - 1)
        mbr[8:12] = struct.pack('<I', start)
        mbr[12:16] = struct.pack('<I', size)
        f.write(mbr)
    # Boot signature
    f.seek(510)
    f.write(b'\x55\xAA')


def build_fat32_partition(path, size_lba, label, files):
    if os.path.exists(path):
        os.remove(path)
    run(QIMG, 'create', '-f', 'raw', path, str(size_lba * SEC))
    run(MFORMAT, '-i', path, '-F', '-v', label, '::')
    dirs = set()
    for arc, _ in files:
        parts = arc.split('/')
        for j in range(1, len(parts)):
            dirs.add('/'.join(parts[:j]))
    for d in sorted(dirs):
        run(MMD, '-i', path, '::' + d)
    for arc, src in files:
        if isinstance(src, bytes):
            tmp = tempfile.NamedTemporaryFile(delete=False, suffix='.bin')
            tmp.write(src)
            tmp.close()
            run(MCOPY, '-o', '-i', path, tmp.name, '::' + arc)
            os.unlink(tmp.name)
        else:
            run(MCOPY, '-o', '-i', path, src, '::' + arc)


def main():
    os.makedirs(BASE, exist_ok=True)
    print('Building swap drill disk: %s' % IMG)

    # Step 2: Create empty disk image
    with open(IMG, 'wb') as f:
        f.truncate(DISK_SECTORS * SEC)

    # Step 3: Write MBR (grldr.mbr first, then partition table)
    with open(IMG, 'r+b') as f:
        # Write grldr.mbr to sector 0 (boot code)
        grldr_mbr = os.path.join(BOOT, 'grldr.mbr')
        if os.path.exists(grldr_mbr):
            with open(grldr_mbr, 'rb') as gr:
                f.seek(0)
                f.write(gr.read())
            print('  MBR: grldr.mbr written')
        # Write partition table at offset 446
        write_mbr(f, [
            (P1_START, P1_SIZE, 0x0C, False),  # FAT32 LBA
            (P2_START, P2_SIZE, 0x07, True),   # NTFS (bootable)
            (P3_START, P3_SIZE, 0x0C, False),  # FAT32 LBA
        ])

    # Step 4: Build sda1 (boot FAT32)
    print('  sda1: boot FAT32')
    boot_files = []
    for name in ('grldr', 'grldr.mbr'):
        src = os.path.join(BOOT, name)
        if os.path.exists(src):
            boot_files.append((name, src))
        else:
            print('  WARN: %s not found' % name)
    boot_files.append(('menu.lst', MENU_TXT.encode()))
    vmlinuz = os.path.join(BOOT, 'vmlinuz-zjrestore')
    if os.path.exists(vmlinuz):
        boot_files.append(('vmlinuz-zjrestore', vmlinuz))
    initrd = os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz')
    if os.path.exists(initrd):
        boot_files.append(('initramfs-zjrestore.cpio.gz', initrd))
    boot_files.append(('restore-task.conf', CONF_TXT.encode()))
    boot_files.append(('_zjresy-drill.log', LOG_TXT.encode()))
    tmp_fat = os.path.join(BASE, '_tmp_sda1.img')
    build_fat32_partition(tmp_fat, P1_SIZE, 'ZJBOOT', boot_files)
    with open(IMG, 'r+b') as f:
        f.seek(P1_START * SEC)
        with open(tmp_fat, 'rb') as tf:
            shutil.copyfileobj(tf, f)
    os.remove(tmp_fat)

    # Step 5: sda2 is left empty - setup boot (zjsetup=1) will mkntfs + populate it
    print('  sda2: left empty (will be formatted by setup boot)')

    # Step 6: Build sda3 (conf FAT32)
    print('  sda3: conf FAT32')
    conf_files = [
        ('restore-task.conf', CONF_TXT.encode()),
        ('_zjresy-drill.log', LOG_TXT.encode()),
    ]
    tmp_fat3 = os.path.join(BASE, '_tmp_sda3.img')
    build_fat32_partition(tmp_fat3, P3_SIZE, 'ZJIMG', conf_files)
    with open(IMG, 'r+b') as f:
        f.seek(P3_START * SEC)
        with open(tmp_fat3, 'rb') as tf:
            shutil.copyfileobj(tf, f)
    os.remove(tmp_fat3)

    print('Done: %s (%d MB)' % (IMG, os.path.getsize(IMG) // (1024*1024)))


if __name__ == '__main__':
    main()
