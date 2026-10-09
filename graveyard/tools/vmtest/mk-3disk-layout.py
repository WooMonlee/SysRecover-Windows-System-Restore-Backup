# -*- coding: utf-8 -*-
"""mk-3disk-layout.py - 复刻 2026-10-04 客户机三盘结构（支持包实测数据）。

来源：故障用户的资料/SysRecover-logs-20261004-141123.zip 的 list.txt /
restore-task.conf / _zjresy041404.log（精确到 LBA）：

  disk0 MA 0902 SSD      1908GiB MBR : C 1154G / H 503G / I 251G（NTFS）
  disk1 ST1000DM010-2EP102 932GiB MBR: D 300G + 扩展(632G){ F 316G, G 316G }（NTFS）
  disk2 KIOXIA-EXCERIA   250,059,350,016B GPT:
        ESP 1GiB @LBA2048 + MSR 30760 扇区 + 目标 @LBA2129960(=1090539520B) 到盘尾-34
  F: 上有 软件目录 九转还原/九转还原0.6.20/ + 镜像 九转还原/20261004Win10.19045备份.wim
  目标根上有 _zjresy041404.log + restore-task.conf（客户原文）

QEMU 侧映射（run-3disk.ps1）：ST1000=sda、MA=sdb、KIOXIA=nvme0n1；型号/序列号
按客户机设置（QEMU 虚拟盘也能带 model/serial，让救援层与 Windows 看到同名盘）。

产物（tools/vmtest/base/3disk/）：
  disk0-ma.vhd / disk1-st1000.vhd / disk2-kioxia.vhd
  stage/   -> 投放到 F: 的内容（软件目录 + 镜像，含中文名）
  bundle/  -> 契约两文件 + startup.nsh
实现：diskpart 动态 VHD -> Mount-DiskImage -> \\\\.\\PhysicalDriveN 手写分区表 +
ESP（mtools 造，避免 Windows 挂 ESP）-> 卸盘。只写非零块，VHD 不膨胀。
"""
import os
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
MB = r'D:\Prog\ProgIDE\msys64\mingw64\bin'
QIMG = os.path.join(MB, 'qemu-img.exe')
MFORMAT = os.path.join(MB, 'mformat.exe')
MCOPY = os.path.join(MB, 'mcopy.exe')
MMD = os.path.join(MB, 'mmd.exe')
ROOT = os.path.dirname(os.path.dirname(HERE))
BOOT = os.path.join(ROOT, 'bootfiles')
BASE = os.path.join(HERE, 'base', '3disk')
WIM = os.path.join(HERE, 'drill-images', 'test.wim')
EXE = os.path.join(ROOT, 'dist', 'x64', 'SysRecoverUI.exe')
# 可替换镜像（用户真实备份）：ZJ_3DISK_IMAGE=源路径 ZJ_3DISK_IMGNAME=投放名
# ZJ_3DISK_ESP_INDEX=子镜像 index（0=无 ESP 子镜像）
SRC_IMAGE = os.environ.get('ZJ_3DISK_IMAGE') or WIM
IMG_NAME = os.environ.get('ZJ_3DISK_IMGNAME') or '20261004Win10.19045备份.wim'
ESP_INDEX = os.environ.get('ZJ_3DISK_ESP_INDEX') or '2'

SEC = 512
MIB = 2048                      # sectors per MiB
GIB = 1 << 21                   # sectors per GiB
TYPE_ESP = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
TYPE_MSR = 'E3C9E316-0B5C-4DB8-817D-F92DF00215AE'
TYPE_DATA = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'

# 磁盘容量（MiB，取整到 diskpart 粒度；显示口径与 list.txt 一致：1908/932/233GB）
MA_MIB = 1953370
ST_MIB = 953869
KX_MIB = 238475

# KIOXIA 精确 LBA（客户契约：target_offset=1090539520）
ESP_FIRST = 2048
ESP_SECTORS = 1 << 21
MSR_FIRST = ESP_FIRST + ESP_SECTORS
MSR_SECTORS = 30760
DATA_FIRST = 2129960
assert MSR_FIRST + MSR_SECTORS == DATA_FIRST

NSH = ("fs0:\\EFI\\ZJRESTORE\\vmlinuz-zjrestore.efi "
       "initrd=\\EFI\\ZJRESTORE\\initramfs-zjrestore.cpio.gz "
       "console=tty0 console=ttyS0 nvme_core.io_timeout=1\r\n")

# 客户原文契约（UTF-8、LF；别动字段——这就是现场证据）。
# @IMG@/@ESP@ 替换为本次测试镜像（默认客户原文值）。
ZJLOG_T = """action=restore
log_time=2026-10-04T14:04:57
software_version=0.6.20
software_path=F:\\九转还原\\九转还原0.6.20\\x64\\SysRecoverUI.exe
target_disk_name=KIOXIA-EXCERIA SSD
target_disk_serial=0000_0000_0000_0000_8CE3_8E03_006E_F5DE.
target_disk_size=250059350016
target_part_offset=1090539520
target_part_size=248968793600
target_fs=NTFS
target_vol_label=
image_path=F:\\九转还原\\@IMG@
image_index=1
esp_index=@ESP@
repair_boot=1
pt_type=gpt
contract_version=1
"""

CONF_T = """action=restore
pt_type=gpt
image_part_guid=
image_rel_path=九转还原/@IMG@
image_path=F:\\九转还原\\@IMG@
image_index=1
esp_index=@ESP@
target_guid=990ad452-8684-496f-a939-62218fb83a7c
target_offset=1090539520
target_size=248968793600
target_disk_serial=0000_0000_0000_0000_8CE3_8E03_006E_F5DE.
repair_boot=1
partition_count=1
software_dir=F:\\九转还原\\九转还原0.6.20
contract_version=1
"""


def zjlog_text():
    return ZJLOG_T.replace('@IMG@', IMG_NAME).replace('@ESP@', ESP_INDEX)


def conf_text():
    return CONF_T.replace('@IMG@', IMG_NAME).replace('@ESP@', ESP_INDEX)


def run(*args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write('FAILED: %s\n%s\n%s\n' % (
            ' '.join(args), r.stdout.decode('utf-8', 'replace'),
            r.stderr.decode('utf-8', 'replace')))
        raise SystemExit(1)


def align(v, a=MIB):
    return (v + a - 1) // a * a


def chs(lba):
    c, h, s = lba // (255 * 63), (lba // 63) % 255, lba % 63 + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return bytes([h, (s | ((c >> 8) << 6)) & 0xFF, c & 0xFF])


def mbr_entry(boot, ptype, first, size):
    return struct.pack('<B3sB3sII', boot, chs(first), ptype,
                       chs(first + size - 1), first, size)


def mbr_finish(mbr):
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr)


def build_mbr_primaries(total, parts):
    """parts: [(ptype, first, size, boot)] -> 512B MBR（守护性不用）。"""
    mbr = bytearray(512)
    off = 446
    for ptype, first, size, boot in parts:
        mbr[off:off + 16] = mbr_entry(boot, ptype, first, size)
        off += 16
    return mbr_finish(mbr)


def build_mbr_st1000(total):
    """D(300G primary) + 扩展(632G){ F 316G, G 316G } —— 复刻客户 EBR 链。
    返回 (mbr, ebr1, ebr2, layout)。"""
    d_first = MIB
    d_size = 300 * GIB
    ext_first = align(d_first + d_size)
    ext_size = total - ext_first - 64
    ext_end = ext_first + ext_size - 1
    f_first = ext_first + MIB                 # EBR1 占扩展区头 1MiB
    f_size = 316 * GIB
    g_ebr = align(f_first + f_size)           # EBR2（1MiB 对齐间隙里）
    g_first = g_ebr + MIB
    g_size = ext_end + 1 - g_first
    assert d_first + d_size <= ext_first and g_size > 0

    mbr = bytearray(512)
    mbr[446:462] = mbr_entry(0x00, 0x07, d_first, d_size)
    mbr[462:478] = mbr_entry(0x00, 0x0F, ext_first, ext_size)

    ebr1 = bytearray(512)
    ebr1[446:462] = mbr_entry(0x00, 0x07, f_first - ext_first, f_size)
    ebr1[462:478] = mbr_entry(0x00, 0x0F, g_ebr - ext_first,
                              ext_end - g_ebr + 1)
    ebr2 = bytearray(512)
    ebr2[446:462] = mbr_entry(0x00, 0x07, g_first - g_ebr, g_size)
    layout = dict(d=(d_first, d_size), ext=(ext_first, ext_size),
                  f=(f_first, f_size), g=(g_first, g_size),
                  ebr1=ext_first, ebr2=g_ebr)
    return mbr_finish(mbr), mbr_finish(ebr1), mbr_finish(ebr2), layout


def guid_bytes(s):
    return uuid.UUID(s).bytes_le


def gpt_entry(type_guid, first, last, name):
    e = guid_bytes(type_guid)
    e += uuid.uuid4().bytes_le
    e += struct.pack('<QQQ', first, last, 0)
    nm = name.encode('utf-16-le')
    e += nm.ljust(72, b'\x00')[:72]
    return e.ljust(128, b'\x00')


def build_gpt(total_sectors, parts):
    entries = b''.join(gpt_entry(*p) for p in parts)
    entries = entries.ljust(128 * 128, b'\x00')
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(my_lba, alt_lba):
        h = b'EFI PART'
        h += struct.pack('<I', 0x00010000)
        h += struct.pack('<I', 92)
        h += struct.pack('<I', 0)
        h += struct.pack('<I', 0)
        h += struct.pack('<QQ', my_lba, alt_lba)
        h += struct.pack('<QQ', 34, total_sectors - 34)
        h += disk_guid
        h += struct.pack('<Q', 2)
        h += struct.pack('<I', 128)
        h += struct.pack('<I', 128)
        h += struct.pack('<I', entries_crc)
        h = h.ljust(92, b'\x00')
        crc = zlib.crc32(h) & 0xFFFFFFFF
        h = h[:16] + struct.pack('<I', crc) + h[20:]
        return h.ljust(512, b'\x00')

    primary = header(1, total_sectors - 1) + entries
    backup = entries + header(total_sectors - 1, 1)
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0x00, b'\x00\x02\x00', 0xEE,
                               b'\xFF\xFF\xFF', 1,
                               min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xAA'
    return bytes(mbr), primary, backup


def create_vhd(path, mebibytes):
    if os.path.exists(path):
        os.remove(path)
    sp = os.path.join(BASE, 'dp_create.txt')
    with open(sp, 'w') as f:
        f.write('create vdisk file="%s" maximum=%d type=expandable\n'
                % (path, mebibytes))
    run('diskpart.exe', '/s', sp)


def attach_vhd(path):
    ps = ("(Mount-DiskImage -ImagePath '%s' | Out-Null); "
          "(Get-DiskImage -ImagePath '%s' | Get-Disk).Number" % (path, path))
    r = subprocess.run(['powershell', '-NoProfile', '-Command', ps],
                       capture_output=True, text=True)
    nums = [ln.strip() for ln in r.stdout.splitlines() if ln.strip().isdigit()]
    if not nums:
        sys.stderr.write('attach failed: %s\n%s\n' % (r.stdout, r.stderr))
        raise SystemExit(1)
    return int(nums[-1])


def total_sectors_of(path):
    r = subprocess.run(['powershell', '-NoProfile', '-Command',
                        "(Get-DiskImage -ImagePath '%s' | Get-Disk).Size" % path],
                       capture_output=True, text=True)
    nums = [ln.strip() for ln in r.stdout.splitlines() if ln.strip().isdigit()]
    return int(nums[-1]) // SEC


def detach_vhd(path):
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    "Dismount-DiskImage -ImagePath '%s' | Out-Null" % path],
                   capture_output=True)


def write_physical(disk_no, chunks):
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.windll.kernel32
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                wintypes.DWORD, ctypes.c_void_p,
                                wintypes.DWORD, wintypes.DWORD,
                                wintypes.HANDLE]
    k32.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong,
                                     ctypes.c_void_p, wintypes.DWORD]
    k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                              ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    INVALID = ctypes.c_void_p(-1).value
    h = k32.CreateFileW(r'\\.\PhysicalDrive%d' % disk_no,
                        0x80000000 | 0x40000000, 0x3, None, 3, 0, None)
    if not h or h == INVALID:
        sys.stderr.write('CreateFile PhysicalDrive%d failed %d\n'
                         % (disk_no, k32.GetLastError()))
        raise SystemExit(1)
    try:
        for off, blob in chunks:
            pos = 0
            while pos < len(blob):
                chunk = blob[pos:pos + (4 << 20)]
                buf = ctypes.create_string_buffer(chunk, len(chunk))
                rw = wintypes.DWORD(0)
                k32.SetFilePointerEx(h, off + pos, None, 0)
                if not k32.WriteFile(h, ctypes.cast(buf, ctypes.c_void_p),
                                     len(chunk), ctypes.byref(rw), None):
                    sys.stderr.write('WriteFile @%d failed %d\n'
                                     % (off + pos, k32.GetLastError()))
                    raise SystemExit(1)
                pos += len(chunk)
    finally:
        k32.CloseHandle(h)


def nonzero_chunks(offset, path):
    out = []
    size = os.path.getsize(path)
    with open(path, 'rb') as f:
        pos = 0
        while pos < size:
            blob = f.read(2 << 20)
            if not blob:
                break
            if blob.count(0) != len(blob):
                out.append((offset + pos, blob))
            pos += len(blob)
    return out


def build_esp_image(path, files, dirs):
    if os.path.exists(path):
        os.remove(path)
    run(QIMG, 'create', '-f', 'raw', path, str(ESP_SECTORS * SEC))
    run(MFORMAT, '-i', path, '-F', '-v', 'ESP', '::')
    for d in dirs:
        run(MMD, '-i', path, '::' + d)
    for src, dst in files:
        run(MCOPY, '-o', '-i', path, src, '::' + dst)


def make_ma(path):
    create_vhd(path, MA_MIB)
    n = attach_vhd(path)
    try:
        total = total_sectors_of(path)
        p1 = MIB
        s1 = 1154 * GIB
        p2 = align(p1 + s1)
        s2 = 503 * GIB
        p3 = align(p2 + s2)
        s3 = total - p3 - 64
        mbr = build_mbr_primaries(
            total, [(0x07, p1, s1, 0x80), (0x07, p2, s2, 0x00),
                    (0x07, p3, s3, 0x00)])
        write_physical(n, [(0, mbr)])
        print('%s: disk%d total=%d; D=%d+%d H=%d+%d I=%d+%d' %
              (path, n, total, p1, s1, p2, s2, p3, s3))
    finally:
        detach_vhd(path)


def make_st(path):
    create_vhd(path, ST_MIB)
    n = attach_vhd(path)
    try:
        total = total_sectors_of(path)
        mbr, ebr1, ebr2, L = build_mbr_st1000(total)
        chunks = [(0, mbr), (L['ebr1'] * SEC, ebr1), (L['ebr2'] * SEC, ebr2)]
        write_physical(n, chunks)
        print('%s: disk%d total=%d; D=%d+%d F=%d+%d G=%d+%d' %
              (path, n, total, L['d'][0], L['d'][1], L['f'][0], L['f'][1],
               L['g'][0], L['g'][1]))
    finally:
        detach_vhd(path)


def make_kx(path, esp_files, esp_dirs):
    create_vhd(path, KX_MIB)
    n = attach_vhd(path)
    try:
        total = total_sectors_of(path)
        assert DATA_FIRST < total - 34, 'VHD too small'
        parts = [
            (TYPE_ESP, ESP_FIRST, ESP_FIRST + ESP_SECTORS - 1,
             'EFI system partition'),
            (TYPE_MSR, MSR_FIRST, MSR_FIRST + MSR_SECTORS - 1,
             'Microsoft reserved partition'),
            (TYPE_DATA, DATA_FIRST, total - 34, 'Basic data partition'),
        ]
        mbr, primary, backup = build_gpt(total, parts)
        esp_img = os.path.join(BASE, 'esp-kx.img')
        build_esp_image(esp_img, esp_files, esp_dirs)
        chunks = [(0, mbr), (SEC, primary)]
        chunks += nonzero_chunks(ESP_FIRST * SEC, esp_img)
        chunks.append(((total - 33) * SEC, backup))
        write_physical(n, chunks)
        os.remove(esp_img)
        print('%s: disk%d total=%d; ESP=%d+%d MSR=%d+%d DATA=%d+%d' %
              (path, n, total, ESP_FIRST, ESP_SECTORS, MSR_FIRST, MSR_SECTORS,
               DATA_FIRST, total - 34 - DATA_FIRST + 1))
    finally:
        detach_vhd(path)


def write_text_file(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main():
    os.makedirs(BASE, exist_ok=True)
    for f in (SRC_IMAGE, EXE,
              os.path.join(BOOT, 'vmlinuz-zjrestore'),
              os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz')):
        if not os.path.exists(f):
            print('missing %s' % f)
            return 1

    # 契约文件（客户原文）+ UEFI shell 启动脚本
    write_text_file(os.path.join(BASE, 'bundle', '_zjresy041404.log'),
                    zjlog_text())
    write_text_file(os.path.join(BASE, 'bundle', 'restore-task.conf'),
                    conf_text())
    with open(os.path.join(BASE, 'bundle', 'startup.nsh'), 'w',
              newline='') as f:
        f.write(NSH)

    # 投放到 F: 的内容（中文名；prep 阶段整目录 copy）
    stage = os.path.join(BASE, 'stage')
    shutil.rmtree(stage, ignore_errors=True)
    st = os.path.join(stage, '九转还原')
    os.makedirs(os.path.join(st, '九转还原0.6.20', 'x64'), exist_ok=True)
    shutil.copy2(EXE, os.path.join(st, '九转还原0.6.20', 'x64',
                                   'SysRecoverUI.exe'))
    shutil.copy2(SRC_IMAGE, os.path.join(st, IMG_NAME))
    print('stage image: %s (%.2f GB)' %
          (IMG_NAME, os.path.getsize(SRC_IMAGE) / (1 << 30)))

    esp_files = [
        (os.path.join(BASE, 'bundle', 'startup.nsh'), 'startup.nsh'),
        (os.path.join(BOOT, 'vmlinuz-zjrestore'),
         'EFI/ZJRESTORE/vmlinuz-zjrestore.efi'),
        (os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz'),
         'EFI/ZJRESTORE/initramfs-zjrestore.cpio.gz'),
    ]
    make_ma(os.path.join(BASE, 'disk0-ma.vhd'))
    make_st(os.path.join(BASE, 'disk1-st1000.vhd'))
    make_kx(os.path.join(BASE, 'disk2-kioxia.vhd'), esp_files,
            ['EFI', 'EFI/ZJRESTORE'])
    print('DONE')
    return 0


if __name__ == '__main__':
    sys.exit(main())
