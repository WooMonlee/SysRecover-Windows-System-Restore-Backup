#!/bin/sh
# setup-swap.sh - First-boot setup: build the drill disk's initial filesystem
# layout for the A-engine (swap / returnold) QEMU drills.
# Invoked by init when zjsetup=N is on the kernel command line (lab only).
#
# Usage: setup-swap.sh [mode] [bulk]
#   mode=1  交换前态：旧系统在根 + ~/.new（新系统）+ ~/.new/.zj-done
#           → 下一跳跑正向 swap（zjrestore-lite.sh 按契约 action=restore）
#   mode=2  交换后态：新系统在根 + ~/.old（旧系统 + .zj-keepold）
#           → 下一跳跑 returnold（契约 action=returnold）
#   mode=3  交换后 + 反向交换已中断（~/.new/.zj-undo-phase phase=2，根上只剩
#           引导命脉与契约）→ 下一跳跑 returnold 的续做路径（L5 被跳过）
#   bulk=N  在"下一跳要被删/被搬"的那棵树里塞 N 个小文件（默认 0）——
#           PIT-142 规模演练。见文件末 plant_bulk。
#
# PIT-139（2026-10-13）：mode=2/3 是「返回旧系统」（方案 C）演练的初态。
# 正向 swap 后的真实终态由 zjswap.sh 产生；这里按同样布局**直接**造出来，
# 免去宿主 Windows 无法挂载救援层刚写的 NTFS、因而没法在两跳之间改契约的麻烦
# （宿主改不了，就让第二跳从"造好的后态"起步）。

MODE="${1:-1}"
BULK="${2:-0}"

# say/mnt_ntfs 单源（P0-B/PIT-145）：SETUP: 前缀 + 文件行落 /tmp/zjrestore.log
# （以前只打控制台不落文件 —— 演练两跳之间 /tmp 里有一条完整时间线）。
ZJPREFIX=SETUP
. /zjlog.sh

# Load ntfs modules
for m in nls_cp437 nls_iso8859-1 nls_utf8 ntfs; do
    modprobe "$m" 2>/dev/null
done
modprobe ntfs3 2>/dev/null

say "swap drill setup: formatting sda2 as NTFS (mode=$MODE)"
if ! mkntfs -F -L ZJSYS /dev/sda2 2>&1; then
    say "mkntfs failed"
    exit 1
fi
say "mkntfs done"

# Mount（mnt_ntfs 收敛：每步 timeout 60 + 错误落 /tmp/zjmnt.err，PIT-112 纪律，
# P0-B/PIT-145 —— 原裸 mount 挂死会静默卡到 init 的 timeout 900 才暴露）
mkdir -p /tmp/zjsetup
mnt_ntfs /dev/sda2 /tmp/zjsetup || {
    say "mount failed: $(head -1 /tmp/zjmnt.err 2>/dev/null)"
    exit 1
}

M=/tmp/zjsetup

# ── 公共：旧系统 / 新系统的可辨识文件 ──
# 用内核文件名当"内容标记"，QEMU 断言直接 grep 文件内容即可判断"哪棵树在根"。
plant_old() {   # plant_old <dir>
    mkdir -p "$1/Windows/System32"
    echo "OLD_KERNEL"   > "$1/Windows/System32/ntoskrnl.exe"
    echo "OLD_BOOTMGR"  > "$1/bootmgr"
    echo "old user data" > "$1/old_userfile.txt"
}
plant_new() {   # plant_new <dir>
    mkdir -p "$1/Windows/System32"
    echo "NEW_KERNEL"   > "$1/Windows/System32/ntoskrnl.exe"
    echo "NEW_BOOTMGR"  > "$1/bootmgr"
    echo "new user data" > "$1/new_userfile.txt"
}

# ── PIT-142：规模树 ──────────────────────────────────────────────
# 演练盘一直是 3 个标记文件的小树，而真实 Windows 是 10 万级文件。
# "rm -rf ~/.new 失败"（2026-10-13 真机）在小树上复现不出来，正因如此。
# 用 ash 内建重定向造（不 fork），N 万次循环约 10-20s；写入量小
# （0 字节文件），但 MFT/目录元数据是真实规模的。
plant_bulk() {   # plant_bulk <dir> <n>
    _bd="$1"; _bn="$2"
    case "$_bn" in
        ''|*[!0-9]*) return 0 ;;   # 非数字 / 空 → 不造（防 set -u/算术炸）
    esac
    [ "$_bn" -eq 0 ] && return 0
    mkdir -p "$_bd" || return 1
    _bi=0
    while [ "$_bi" -lt "$_bn" ]; do
        : > "$_bd/f$_bi"
        _bi=$((_bi + 1))
    done
    say "bulk tree: $_bn files under $_bd"
}

# 真机上救援层一启动就认领日志窝（软件目录找不到时落在**目标盘根**，
# PIT-111/137 → `ZJRESTORE-logs/`），而且 PIT-141 之前 log mirror 每 2s 还往
# 里写。演练若不模拟它，L5「把诊断资产搬进 ~/.new」这一步永远测不到 ——
# 2026-10-13 真机 `rm -rf ~/.new` 失败的根因正埋在这里（PIT-142）。
plant_bb_home() {   # plant_bb_home <dir>
    mkdir -p "$1/ZJRESTORE-logs"
    echo "fake rescue log (drill)" > "$1/ZJRESTORE-logs/zjrestore-debug.log"
    echo "result=OK step=drill"    > "$1/ZJRESTORE-status.txt"
    say "planted black-box home under $1 (ZJRESTORE-logs/ + status)"
}

case "$MODE" in
1)
    # 交换前态（既有 run-swap-drill.ps1 用）：旧系统在根，新系统在 ~/.new
    # 正向 swap 的 zjswap.sh 会 rm -rf ~/.new → bulk 塞进 ~new
    plant_old "$M"
    plant_new "$M/~new"
    echo "1024 $(date +%s)" > "$M/~new/.zj-done"
    plant_bulk "$M/~new" "$BULK"
    # bulk 放末尾（别插进 "(mode=N)" —— run-undo-drill.ps1 断言匹配那截字面）
    say "setup done (mode=1): old system at root + ~new + .zj-done on /dev/sda2 bulk=$BULK"
    ;;
2)
    # 交换后态：新系统在根，旧系统在 ~/.old（含 .zj-keepold 备返标记）
    # returnold 的 L5 会把根搬进 ~/.new，L7 再 rm -rf ~/.new → bulk 塞进根
    plant_new "$M"
    plant_old "$M/~old"
    echo "1" > "$M/~old/.zj-keepold"
    plant_bb_home "$M"
    plant_bulk "$M" "$BULK"
    say "setup done (mode=2): new system at root + ~old (with .zj-keepold) on /dev/sda2 bulk=$BULK"
    ;;
3)
    # 反向交换中断态（phase=2：L5 已把根搬进 ~/.new，L6 还没跑）：
    # 根上只剩引导命脉（由 mk-drill.py 写进 sda1，不在这里）与契约；
    # 新系统在 ~/.new，旧系统完好在 ~/.old —— 续做应跳过 L5、只跑 L6/L7。
    # L7 会 rm -rf ~/.new → bulk 塞进 ~new
    plant_new "$M/~new"
    plant_old "$M/~old"
    printf 'phase=2\n' > "$M/~new/.zj-undo-phase"
    plant_bb_home "$M"
    plant_bulk "$M/~new" "$BULK"
    say "setup done (mode=3): interrupted returnold (phase=2) on /dev/sda2 bulk=$BULK"
    ;;
*)
    say "unknown mode=$MODE (expected 1|2|3)"
    umount "$M"
    exit 1
    ;;
esac

sync
umount "$M"

# Update menu.lst on sda1 to remove zjsetup=N (so next boot runs the actual script)
say "updating menu.lst to remove zjsetup=$MODE"
mkdir -p /tmp/zjboot
if timeout 60 mount -t vfat /dev/sda1 /tmp/zjboot 2>>/tmp/zjmnt.err; then
    if [ -f /tmp/zjboot/menu.lst ]; then
        sed -i 's/ zjsetup=[0-9]*//g' /tmp/zjboot/menu.lst
        sync
        say "menu.lst updated"
    fi
    umount /tmp/zjboot
fi

say "SETUP COMPLETE"
