#!/usr/bin/env bash
# mk-drill.sh — 【已由 mk-drill.py 取代，这里只做兼容转调】
#
# 为什么换掉 bash 版（2026-09-24）：
#   1. mtools 的 `mformat` 用**文件大小**决定卷大小。对多分区磁盘镜像，它会把整盘
#      当成一个卷 → 三个分区 FAT 相互覆盖（症状：sda2/sda3 变 "non DOS media"、
#      sda1 里 grldr/menu.lst 凭空消失）。正确做法是每个分区单独建精确大小的镜像
#      再拼回磁盘 —— 见 mk-drill.py。
#   2. 旧脚本还引用了已删除的 bootfiles/restore.sh（救援脚本 zjrestore-lite.sh 现在
#      打进 initramfs 内，引导分区不再需要它）。
#   3. 旧脚本注释里的 sda3 偏移 822704128 是错的（正确 = 1606848*512 = 822706176），
#      曾误导排查。
#
# 用法（不变）：先备好 drill-images/test.wim，然后 ./mk-drill.sh
set -e
cd "$(dirname "$0")"
exec python mk-drill.py "$@"
