#!/usr/bin/env bash
# mk-drill.sh — 重建还原演练盘 drill.raw（2GB，3 个 FAT32 分区，全镜像均为项目原版文件）
#
# 布局（扇区，512B）：
#   sda1  2048..206847     100MB  引导分区（grldr.mbr/grldr/menu.lst/vmlinuz/initramfs/restore.sh）
#   sda2  206848..1606847  683MB  还原目标分区（根目录放 _zjresy 恢复日志 = 任务靶子）
#   sda3  1606848..4194303 1.2GB  镜像分区（images/test.wim + restore-task.conf）
#
# 演练时 /init（Alpine 底座）会：找日志 → 找任务/镜像 → mkntfs 格式化 sda2
#   → wimlib apply → PBR 引导代码回写 → reboot（QEMU -no-reboot 时自动退出 = 完成信号）
set -e
cd "$(dirname "$0")"

MB=/d/Prog/ProgIDE/msys64/mingw64/bin
QIMG=$MB/qemu-img.exe
MCOPY=$MB/mcopy.exe
MFORMAT=$MB/mformat.exe
MDIR=$MB/mdir.exe
PY=$MB/python.exe
BOOT=/d/Prog/_Project/SysRecover/bootfiles
IMG=base/drill.raw
WIM=${ZJ_DRILL_WIM:-drill-images/test.wim}
# 演练用 initramfs：Alpine 底座的生产 initramfs（自带 /init + 工具 + 模块）
INITRD=$BOOT/initramfs-zjrestore.cpio.gz

S1_OFF=$((2048*512))        # 1048576
S2_OFF=$((206848*512))      # 105906176
S3_OFF=$((1606848*512))     # 822704128
S1_LEN=204800
S2_LEN=1400000
S3_LEN=2587456

if [ ! -f "$WIM" ]; then
    echo "缺 $WIM —— 先用产品 CLI 制作："
    echo "  dist/SysRecover.exe backup --source <小目录>/ --dest drill-images/test.wim --compress fast --yes"
    exit 1
fi

rm -f "$IMG"
"$QIMG" create -f raw "$IMG" 2G

# 先写 grldr.mbr 引导代码，再写分区表。顺序不能反：grldr.mbr 有 8192 字节，
# 覆盖 0..8191 会把偏移 446 的分区表清零，grldr 便找不到任何分区
# （症状：Try (hd0): non-MS: skip ×N → Cannot find GRLDR in all drives）。
"$PY" - <<'EOF'
gr = open(r'D:\Prog\_Project\SysRecover\bootfiles\grldr.mbr','rb').read()
img = bytearray(open('base/drill.raw','rb').read())
img[0:len(gr)] = gr
open('base/drill.raw','wb').write(img)
print('grldr.mbr -> MBR code ok')
EOF

"$PY" - <<'EOF'
import struct
TOTAL = 2*1024*1024*1024//512
parts = [
    (0x80, 2048,   204800),    # sda1 active
    (0x00, 206848, 1400000),   # sda2
    (0x00, 1606848, TOTAL-1606848),  # sda3
]
def chs(lba):
    # 标准换算，柱面超 1023 钳位
    c, h, s = lba//(255*63), (lba//63)%255, lba%63+1
    if c > 1023: c, h, s = 1023, 254, 63
    return bytes([h, (s | ((c>>8)<<6)) & 0xFF, c & 0xFF])
img = bytearray(open('base/drill.raw','rb').read())
off = 446
for active, begin, length in parts:
    img[off:off+16] = struct.pack('<B3sB3sII', active, chs(begin), 0x0C, chs(begin+length-1), begin, length)
    off += 16
img[510:512] = b'\x55\xaa'
open('base/drill.raw','wb').write(img)
print('MBR: 3 partitions written')
EOF

"$MFORMAT" -i "$IMG@@$S1_OFF" -F -v ZJBOOT ::
"$MFORMAT" -i "$IMG@@$S2_OFF" -F -v ZJSYS ::
"$MFORMAT" -i "$IMG@@$S3_OFF" -F -v ZJIMG ::

# sda1: 引导链。Alpine 底座自带 /init，无需 rdinit；加 console=ttyS0 便于
# -nographic 观察（FRAMEBUFFER_CONSOLE 也内建，VGA 同样可见）。
printf 'timeout 3\ndefault 0\ntitle zjrestore-test\nkernel /vmlinuz-zjrestore console=tty0 console=ttyS0,115200 loglevel=7 nvme_core.io_timeout=1\ninitrd /initramfs-zjrestore.cpio.gz\n' > base/menu-drill.lst
"$MCOPY" -i "$IMG@@$S1_OFF" "$BOOT/grldr" ::grldr
"$MCOPY" -i "$IMG@@$S1_OFF" base/menu-drill.lst ::menu.lst
"$MCOPY" -i "$IMG@@$S1_OFF" "$BOOT/vmlinuz-zjrestore" ::vmlinuz-zjrestore
"$MCOPY" -i "$IMG@@$S1_OFF" "$INITRD" ::initramfs-zjrestore.cpio.gz
"$MCOPY" -i "$IMG@@$S1_OFF" "$BOOT/restore.sh" ::restore.sh

# sda2: 恢复日志（= 靶子；/init 靠它定位目标分区）
cat > /tmp/_zjresy-drill.log <<EOF
action=restore
log_time=2026-09-09 22:30:00
software_version=0.1.0
software_path=D:\\ZJRESTORE\\SysRecover.exe
target_disk_name=QEMU HARDDISK DRILL
target_disk_serial=QEMU-DRILL-0001
target_disk_size=2147483648
target_part_offset=105906176
target_part_size=1400000
target_fs=FAT32
target_vol_label=ZJSYS
image_path=D:/images/test.wim
image_index=1
repair_boot=1
pt_type=mbr
EOF
"$MCOPY" -i "$IMG@@$S2_OFF" /tmp/_zjresy-drill.log ::_zjresy-drill.log

# sda3: 镜像 + 任务文件
"$MB/mmd.exe" -i "$IMG@@$S3_OFF" ::images
"$MCOPY" -i "$IMG@@$S3_OFF" "$WIM" ::images/test.wim

# 引导补全文件（生产由 Windows 侧生成到 ZJRESTORE\bootfix\；演练造假文件）
# ZJ_DRILL_NOBOOTFIX=1 → 完全不放 bootfix（pbr-probe.py 需要目标分区没有
# \bootmgr，这样引导代码才会报 "BOOTMGR is missing" 作为成功信号）。
BFDIR=base/bootfix
if [ "${ZJ_DRILL_NOBOOTFIX:-0}" != "1" ]; then
    if [ ! -f "$BFDIR/bootmgr" ]; then
        mkdir -p "$BFDIR/Boot/fonts"
        printf 'FAKE-BOOTMGR' > "$BFDIR/bootmgr"
        printf 'FAKE-BCD'     > "$BFDIR/Boot/BCD"
        printf 'FAKE-FONT'    > "$BFDIR/Boot/fonts/chs_boot.ttf"
    fi
    "$MB/mmd.exe" -i "$IMG@@$S3_OFF" ::bootfix
    "$MCOPY" -s -i "$IMG@@$S3_OFF" "$BFDIR"/* ::bootfix/
else
    echo "(ZJ_DRILL_NOBOOTFIX=1: skip bootfix)"
fi

cat > /tmp/restore-task.conf <<EOF
action=restore
pt_type=mbr
target_offset=105906176
target_size=716800000
target_disk_serial=QEMU-DRILL-0001
image_path=D:/images/test.wim
image_index=1
repair_boot=1
EOF
"$MCOPY" -i "$IMG@@$S3_OFF" /tmp/restore-task.conf ::restore-task.conf

echo "== drill.raw ready =="
"$MDIR" -i "$IMG@@$S2_OFF" ::
"$MDIR" -i "$IMG@@$S3_OFF" ::
