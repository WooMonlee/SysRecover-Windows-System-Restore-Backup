#!/usr/bin/env bash
# mk-testdisk.sh — 重建/刷新 base/testdisk.raw（512MB，FAT32 活动分区 + GRUB4DOS 链）
# 用法: ./mk-testdisk.sh   （幂等；已存在则只刷新引导文件与 MBR）
set -e
cd "$(dirname "$0")"

QIMG=/d/Prog/ProgIDE/msys64/mingw64/bin/qemu-img.exe
MCOPY=/d/Prog/ProgIDE/msys64/mingw64/bin/mcopy.exe
MFORMAT=/d/Prog/ProgIDE/msys64/mingw64/bin/mformat.exe
BOOT=/d/Prog/_Project/SysRecover/bootfiles
IMG=base/testdisk.raw
OFFSET=1048576   # 分区起始 2048 扇区 × 512

if [ ! -f "$IMG" ]; then
    mkdir -p base
    "$QIMG" create -f raw "$IMG" 512M
    "$MFORMAT" -i "$IMG@@$OFFSET" -F -v ZJTEST ::
fi

[ -f base/menu.lst ] || printf 'timeout 3\ndefault 0\ntitle zjrestore-test\nkernel /vmlinuz-zjrestore console=tty0 loglevel=7\ninitrd /initramfs-zjrestore.cpio.gz\n' > base/menu.lst

# 先写 grldr.mbr 引导代码，再写分区表（顺序不能反，见 mk-drill.sh 注释：
# grldr.mbr 覆盖 0..8191 会把偏移 446 的分区表清零 → grldr 找不到分区）。
python - <<'EOF'
gr = open(r'D:\Prog\_Project\SysRecover\bootfiles\grldr.mbr','rb').read()
img = bytearray(open('base/testdisk.raw','rb').read())
img[0:len(gr)] = gr
open('base/testdisk.raw','wb').write(img)
EOF

python - <<'EOF'
import struct
begin, length = 2048, 512*1024*1024//512 - 2048
# 活动 + FAT32-LBA + 正确 CHS（grldr.mbr 会校验；CHS 错报 non-MS: skip）
entry = struct.pack('<B3sB3sII', 0x80, b'\x00\x21\x00', 0x0C, b'\xfe\xff\xff', begin, length)
img = bytearray(open('base/testdisk.raw','rb').read())
img[446:462] = entry
img[510:512] = b'\x55\xaa'
open('base/testdisk.raw','wb').write(img)
EOF

"$MCOPY" -o -i "$IMG@@$OFFSET" "$BOOT/grldr" ::grldr
"$MCOPY" -o -i "$IMG@@$OFFSET" base/menu.lst ::menu.lst
"$MCOPY" -o -i "$IMG@@$OFFSET" "$BOOT/vmlinuz-zjrestore" ::vmlinuz-zjrestore
"$MCOPY" -o -i "$IMG@@$OFFSET" "$BOOT/initramfs-zjrestore.cpio.gz" ::initramfs-zjrestore.cpio.gz
# 注：bootfiles/restore.sh 已不存在（救援脚本打进 initramfs 内）——2026-09-24 修正。
#     本脚本仅供老的单分区引导测试；更完整的 BIOS 冒烟见 bios-smoke.ps1。

echo "testdisk.raw ready."
