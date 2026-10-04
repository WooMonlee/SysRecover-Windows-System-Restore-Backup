# -*- coding: utf-8 -*-
"""mk-customer-layout.py - 复刻 2026-10-03 客户机的磁盘结构（Netac 256G + HKVSN 2T）。

为什么做这个：
  客户机 = 系统盘 GPT（ESP 1GiB + MSR 16MiB + C: 237.46GiB @ 非 1MiB 对齐）
  + 数据盘 GPT（D/E/F/G 四个 NTFS）。要区分"结构问题"还是"第三方软件问题"，
  先用同结构跑一遍我们自己的还原（run-cust-drill.ps1），再把同结构磁盘给 VM
  装 Windows + 易数一键还原复现。

客户结构（来自 list.txt / restore-task.conf，精确到扇区）：
  磁盘0 Netac 256G（238.47 GiB）:
    Part1 ESP  FAT32  1GiB    @ LBA 40        （40+1GiB+16MiB == 2129960，正好）
    Part2 MSR  16MiB          @ LBA 2097192
    Part3 C:   NTFS   237.46GiB @ LBA 2129960 (1090539520B)  497988199 扇区
    C: 尾 + 1 == 备份 GPT 起点（整盘 500118192 扇区 = 256,060,514,304 B）
  磁盘1 HKVSN 2T（1907.73 GiB）:
    D 300GiB / E 500GiB / F 550GiB / G ≈557.7GiB（NTFS，未格式化）

产物（tools/vmtest/base/）：
  custdisk0.vhd  干净系统盘（ESP 空 FAT32、C: 未格式化）  -> 转 vmdk 给 VM
  custdrill.vhd  同结构 + ESP 内含救援文件/契约/test.wim  -> run-cust-drill.ps1
  custdata.vhd   数据盘（4 个 NTFS 区未格式化，VM 内自行格式化）

实现：diskpart 建动态 VHD -> Mount-DiskImage -> 用 \\\\.\\PhysicalDriveN 直写
      GPT + ESP 镜像（ESP 用 mtools 造，避免在 Windows 里挂 ESP）-> 卸载。
      只写非零块（ESP 镜像大部分是洞），VHD 不会膨胀。
"""
import os
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
BASE = os.path.join(HERE, 'base')
WIM = os.path.join(HERE, 'drill-images', 'test.wim')

SEC = 512
TYPE_ESP = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
TYPE_MSR = 'E3C9E316-0B5C-4DB8-817D-F92DF00215AE'
TYPE_DATA = 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7'

# 客户机精确参数
ESP_FIRST = 40
ESP_SECTORS = 1 << 21                 # 1 GiB
MSR_FIRST = ESP_FIRST + ESP_SECTORS   # 2097192
MSR_SECTORS = 1 << 15                 # 16 MiB
C_FIRST = MSR_FIRST + MSR_SECTORS     # 2129960
C_SECTORS = 497988199                 # 254,969,957,888 B
# 磁盘0 总扇区（diskpart 的 MB 取整会略大，C: 尾之后留 1MiB 级尾巴，无碍）

MENU_NSH = ("fs0:\\EFI\\ZJRESTORE\\vmlinuz-zjrestore.efi "
            "initrd=\\EFI\\ZJRESTORE\\initramfs-zjrestore.cpio.gz "
            "console=ttyS0 nvme_core.io_timeout=1\r\n")

CONF_TXT = """action=restore
pt_type=gpt
target_offset=1090539520
target_size=254969957888
target_disk_serial=CUSTDRILL0001
image_path=Z:/images/test.wim
image_index=1
esp_index=2
repair_boot=1
contract_version=1
software_dir=X:\\ZJRESTORE
"""


def run(*args):
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write('FAILED: %s\n%s\n%s\n' % (
            ' '.join(args), r.stdout.decode('utf-8', 'replace'),
            r.stderr.decode('utf-8', 'replace')))
        raise SystemExit(1)


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
    """parts: [(type, first, last, name)] -> (mbr, primary_area, backup_area)
    primary_area 覆盖 LBA1..LBA33（header + 128 entries，共 34*512 的前 33 个）；
    实际返回 (mbr512, lba1_to_33 字节, 备份 entries + 备份 header)。"""
    entries = b''.join(gpt_entry(*p) for p in parts)
    entries = entries.ljust(128 * 128, b'\x00')
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(my_lba, alt_lba):
        h = b'EFI PART'
        h += struct.pack('<I', 0x00010000)   # rev 1.0
        h += struct.pack('<I', 92)           # header size
        h += struct.pack('<I', 0)            # crc (稍后填)
        h += struct.pack('<I', 0)            # reserved
        h += struct.pack('<QQ', my_lba, alt_lba)
        h += struct.pack('<QQ', 34, total_sectors - 34)  # first/last usable
        h += disk_guid
        h += struct.pack('<Q', 2)            # entries LBA
        h += struct.pack('<I', 128)          # entry count
        h += struct.pack('<I', 128)          # entry size
        h += struct.pack('<I', entries_crc)
        h = h.ljust(92, b'\x00')
        crc = zlib.crc32(h) & 0xFFFFFFFF
        h = h[:16] + struct.pack('<I', crc) + h[20:]
        return h.ljust(512, b'\x00')

    primary = header(1, total_sectors - 1) + entries        # LBA1 + LBA2..33
    backup = entries + header(total_sectors - 1, 1)         # LBA(total-33..total-1)
    # 保护性 MBR
    mbr = bytearray(512)
    mbr[446:462] = struct.pack('<B3sB3sII', 0x00, b'\x00\x02\x00', 0xEE,
                               b'\xFF\xFF\xFF', 1, min(total_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xAA'
    return bytes(mbr), primary, backup, total_sectors


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


def detach_vhd(path):
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    "Dismount-DiskImage -ImagePath '%s' | Out-Null" % path],
                   capture_output=True)


def write_physical(disk_no, chunks):
    """chunks: [(offset, bytes)]；只写非零块（洞不动 → 动态 VHD 不膨胀）。"""
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
            if blob.count(0) != len(blob):   # C 速度的全零检测
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
    print('ESP image: %s (%d files, %d dirs)' % (path, len(files), len(dirs)))


def part_sectors(total):
    """磁盘0 分区表（用实际 VHD 扇区数封底）。"""
    parts = [
        (TYPE_ESP, ESP_FIRST, ESP_FIRST + ESP_SECTORS - 1, 'EFI system partition'),
        (TYPE_MSR, MSR_FIRST, MSR_FIRST + MSR_SECTORS - 1, 'Microsoft reserved partition'),
        (TYPE_DATA, C_FIRST, C_FIRST + C_SECTORS - 1, 'Basic data partition'),
    ]
    assert parts[-1][2] <= total - 34, 'VHD too small for C: partition'
    return parts


def make_disk0(path, mebibytes, esp_files, esp_dirs, label):
    create_vhd(path, mebibytes)
    n = attach_vhd(path)
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-Command',
                            "(Get-DiskImage -ImagePath '%s' | Get-Disk).Size" % path],
                           capture_output=True, text=True)
        total_bytes = int([ln for ln in r.stdout.splitlines() if ln.strip().isdigit()][-1])
        total = total_bytes // SEC
        mbr, primary, backup, total = build_gpt(total, part_sectors(total))
        esp_img = os.path.join(BASE, 'esp-%s.img' % label)
        build_esp_image(esp_img, esp_files, esp_dirs)
        chunks = [(0, mbr), (SEC, primary)]
        chunks += nonzero_chunks(ESP_FIRST * SEC, esp_img)
        chunks.append(((total - 33) * SEC, backup))
        write_physical(n, chunks)
        os.remove(esp_img)
        print('%s: disk%d total=%d sectors; parts=%s' %
              (path, n, total, [(p[1], p[2]) for p in part_sectors(total)]))
    finally:
        detach_vhd(path)


def make_disk1(path, mebibytes):
    create_vhd(path, mebibytes)
    n = attach_vhd(path)
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-Command',
                            "(Get-DiskImage -ImagePath '%s' | Get-Disk).Size" % path],
                           capture_output=True, text=True)
        total = int([ln for ln in r.stdout.splitlines() if ln.strip().isdigit()][-1]) // SEC
        gib = (1 << 30) // SEC
        sizes = [300 * gib, 500 * gib, 550 * gib]
        parts = []
        first = 40
        for s in sizes:
            parts.append((TYPE_DATA, first, first + s - 1, 'Basic data partition'))
            first = (first + s + 2047) // 2048 * 2048
        parts.append((TYPE_DATA, first, total - 34, 'Basic data partition'))
        mbr, primary, backup, total = build_gpt(total, parts)
        write_physical(n, [(0, mbr), (SEC, primary),
                           ((total - 33) * SEC, backup)])
        print('%s: disk%d total=%d sectors; parts=%s' %
              (path, n, total, [(p[1], p[2]) for p in parts]))
    finally:
        detach_vhd(path)


def main():
    os.makedirs(BASE, exist_ok=True)
    if not os.path.exists(WIM):
        print('missing %s' % WIM)
        return 1
    for f in ('vmlinuz-zjrestore', 'initramfs-zjrestore.cpio.gz'):
        if not os.path.exists(os.path.join(BOOT, f)):
            print('missing bootfiles/%s' % f)
            return 1

    # 两小文件先落地（给 ESP 用）
    nsh = os.path.join(BASE, 'cust-startup.nsh')
    with open(nsh, 'w', newline='') as f:
        f.write(MENU_NSH)
    conf = os.path.join(BASE, 'cust-restore-task.conf')
    with open(conf, 'w', newline='\n') as f:
        f.write(CONF_TXT)

    drill_files = [
        (nsh, 'startup.nsh'),
        (os.path.join(BOOT, 'vmlinuz-zjrestore'), 'EFI/ZJRESTORE/vmlinuz-zjrestore.efi'),
        (os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz'),
         'EFI/ZJRESTORE/initramfs-zjrestore.cpio.gz'),
        (conf, 'restore-task.conf'),
        (WIM, 'images/test.wim'),
    ]
    drill_dirs = ['EFI', 'EFI/ZJRESTORE', 'images', 'ZJRESTORE']

    mb0 = (C_FIRST + C_SECTORS + 34 + 2048) * SEC // (1 << 20) + 1
    make_disk0(os.path.join(BASE, 'custdrill.vhd'), mb0,
               drill_files, drill_dirs, 'drill')
    make_disk0(os.path.join(BASE, 'custdisk0.vhd'), mb0,
               [], [], 'clean')
    # 数据盘：1907.73 GiB ≈ 1,953,519 MiB
    make_disk1(os.path.join(BASE, 'custdata.vhd'), 1953519)
    print('DONE')
    return 0


if __name__ == '__main__':
    sys.exit(main())
