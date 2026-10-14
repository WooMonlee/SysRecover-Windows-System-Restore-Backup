#!/bin/sh
# zjlog.sh — 救援层统一诊断底座（P0-B，PIT-145）
#
# 背景：5 个救援脚本（init / zjrestore-lite.sh / zjswap.sh / zjswap-undo.sh /
# setup-swap.sh）各复制了一份 say()，落盘行为渐渐漂移：init/lite 的文件行无
# 前缀、undo 带前缀、swap/setup 只打控制台不落文件 —— 多脚本合并成的
# /tmp/zjrestore.log 里分不清谁说的、还缺行。统一到本文件单源实现。
#
# 用法（调用方在 source 前设前缀，前缀是断言/录像取证的契约，不许改）：
#   ZJPREFIX=SWAPUNDO
#   . /zjlog.sh
#
# 约定：
#   · 控制台行 = "<PREFIX>: msg"（QEMU 断言 grep 它，如 `SWAPUNDO:`/`ZJ:`）；
#   · 文件行   = "YYYY-mm-dd HH:MM:SS <PREFIX>: msg" 追加到
#     ${LOG:-/tmp/zjrestore.log} —— 5 个脚本合并成一条时间线（LOG 未导出
#     的子进程也能落同一份：兜底路径相同）；
#   · 文件写失败必须静默（日志面可能未就绪/只读）—— say 永远 return 0，
#     不能因落盘失败让调用方的 `if say` 判断走偏，也兼容 `set -u`。
#
# 只放 say。不放 run/die：现有调用点是 "say ERROR → umount → exit N" 的
# 多步形态，硬套 die 只会增加 diff 噪音 —— 需要时再加，别为统一而统一。
# 带 timeout 的 NTFS 挂载纪律（PIT-112"挂死不可静默" + PIT-144 FUSE 优先）
# 也收敛在这里：三个裸 mount 的脚本（zjswap/zjswap-undo/setup-swap）共用。

: "${ZJPREFIX:=ZJ}"

say(){
    echo "$ZJPREFIX: $*" 1>&2
    echo "$(date '+%F %T') $ZJPREFIX: $*" >> "${LOG:-/tmp/zjrestore.log}" 2>/dev/null
    return 0
}

# mnt_ntfs <dev> <mnt> [ntfs3|fuse] —— 返回 0/1；错误进 /tmp/zjmnt.err
#   ntfs3（默认）：内核驱动优先，吞吐高（与 zjrestore-lite.sh::mnt_dev 同序）；
#   fuse：ntfs-3g 优先 —— **zjswap-undo.sh 必须用这个**（PIT-144：ntfs3 在
#         大规模 rename 风暴后会留幽灵索引项，rm -rf 必失败）。
# 每一步都带 timeout 60（PIT-112：ntfs3 挂坏设备会静默挂死 400s 级）；
# 成功打一行 `  mnt <dev> ok (<drv>)` —— "这次是 FUSE 还是内核驱动"是
# PIT-144 类问题的定罪要素，必须留痕。
mnt_ntfs(){
    : > /tmp/zjmnt.err
    if [ "${3:-ntfs3}" = fuse ]; then
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
    else
        timeout 60 mount -t ntfs3 "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (ntfs3)"; return 0; }
        timeout 60 ntfs-3g -o force "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    fi
    timeout 60 mount -t ntfs-3g "$1" "$2" 2>>/tmp/zjmnt.err && { say "  mnt $1 ok (fuse)"; return 0; }
    return 1
}
