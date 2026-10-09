#!/bin/sh
# zjswap.sh — A 引擎（0.7）交换引擎：把 <目标>:\~new 换入原位，旧树移入 ~old。
#
# 用法: zjswap.sh <目标分区设备> <挂载点>
# 前置（Windows 侧保证）：~new 已由 `SysRecover.exe extract --all` 完整解压，
#   并在成功后写入完成标记 `~new/.zj-done`（内容：内容量字节 + 时间）。
#
# 失败安全（用户 2026-10-09 定案，docs/23 L1/D2）：
#   · 任何检查不过 → **报错退出、绝不碰旧树**（旧系统原样可启动）；
#   · 解压失败/中断（含断电）→ 标记缺失 → 拒绝交换；孤儿 ~new 由下次
#     Windows 启动检测并清理（提示用户换镜像重试）；**不做断点续解**（首版）。
#   · 交换自身中断 → 半新半旧（P0 记录、P1 事务化；见 docs/23 L3）。
#
# 特殊顶层项（docs/23 L2/D3）：回收站/系统卷信息/页文件删除旧（新树自带或
#   系统首启重建），不进 ~old。
set -u

T="${1:?target dev}"; M="${2:?mount point}"
say(){ echo "SWAP: $*" 1>&2; }

mkdir -p "$M"
if ! mount -t ntfs3 "$T" "$M" 2>/dev/null; then
    ntfs-3g -o force "$T" "$M" || { say "mount FAILED ($T)"; exit 1; }
fi

# ── L1：先验完成标记，再动任何东西（fail-closed）──
if [ ! -f "$M/~new/.zj-done" ]; then
    say "ERROR: ~new missing or incomplete (.zj-done absent) -> refuse; old system untouched"
    umount "$M" 2>/dev/null
    exit 2
fi
say "marker ok: $(cat "$M/~new/.zj-done" 2>/dev/null | tr -d '\r\n')"

# ── 旧顶层项 → ~old（特殊项删旧）──
mkdir -p "$M/~old" || { say "mkdir ~old FAILED"; umount "$M" 2>/dev/null; exit 1; }
for d in "$M"/* "$M"/.[!.]*; do
    [ -e "$d" ] || continue
    b=$(basename "$d")
    case "$b" in ~new|~old|.|..) continue;; esac
    case "$b" in
        '$RECYCLE.BIN'|'System Volume Information'|pagefile.sys|swapfile.sys|hiberfil.sys)
            rm -rf "$d" 2>/dev/null || say "WARN: rm old $b failed"
            continue;;
    esac
    mv "$d" "$M/~old/" 2>/dev/null || say "WARN: move old $b failed"
done

# ── ~new 顶层项 → 原位 ──
for d in "$M/~new"/* "$M/~new"/.[!.]*; do
    [ -e "$d" ] || continue
    b=$(basename "$d")
    [ "$b" = ".zj-done" ] && continue
    if [ -e "$M/$b" ]; then
        rm -rf "$M/$b" 2>/dev/null || say "WARN: clear conflict $b failed"
    fi
    mv "$d" "$M/$b" 2>/dev/null || say "WARN: move new $b failed"
done
rmdir "$M/~new" 2>/dev/null
sync
umount "$M" 2>/dev/null
say "swap done (old tree kept at ~old; first boot should delete it)"
exit 0
