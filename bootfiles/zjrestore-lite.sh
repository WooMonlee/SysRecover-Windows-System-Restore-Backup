#!/bin/sh
# zjrestore-lite.sh — 轻量还原脚本（生产版）
# S99zjrestore 已把 _zjresy*.log / restore-task.conf / zjtools 放到 /tmp，
# 并把「目标分区设备」作为 $1 传入（日志写在目标分区根，故日志所在分区即目标）。
# 控制台无中文字库 → 消息全 ASCII。日志尽量持久化到非目标分区（重启后可取）。
set -u
LOG=/tmp/zjrestore.log
say(){ echo "ZJ: $*"; echo "$(date '+%F %T') $*" >> "$LOG"; }
# 日志 + apply 采样/输出一起留档（离线分析卡顿用）
save_diag(){
    cp "$LOG" "$1/zjrestore-debug.log" 2>/dev/null || return 1
    cp /tmp/apply_io.log "$1/zjrestore-io.log" 2>/dev/null
    cp /tmp/apply.out "$1/zjrestore-apply.out" 2>/dev/null
    return 0
}

say "zjrestore-lite start"

TARGET_LOG=$(ls /tmp/_zjresy*.log 2>/dev/null | head -1)
TASK_CONF="/tmp/restore-task.conf"
if [ -z "$TARGET_LOG" ] && [ ! -f "$TASK_CONF" ]; then
    say "no log and no conf, abort"
    exit 1
fi
[ -n "$TARGET_LOG" ] || say "warn: no _zjresy log, using conf only"
[ -f "$TASK_CONF" ] || say "warn: no conf, using log only"

get_log(){
    [ -n "$TARGET_LOG" ] || return 0
    grep "^$1=" "$TARGET_LOG" 2>/dev/null | head -1 | cut -d= -f2-
}
get_task(){
    if [ -f "$TASK_CONF" ]; then
        _v=$(grep "^$1=" "$TASK_CONF" 2>/dev/null | head -1 | cut -d= -f2-)
        [ -n "$_v" ] && { echo "$_v"; return; }
    fi
    get_log "$1"
}

IMAGE_INDEX=$(get_task image_index)
[ -n "$IMAGE_INDEX" ] || IMAGE_INDEX=1
REPAIR_BOOT=$(get_task repair_boot)
PT_TYPE=$(get_task pt_type)
TARGET_OFFSET=$(get_task target_offset)
[ -n "$TARGET_OFFSET" ] || TARGET_OFFSET=$(get_log target_part_offset)
TARGET_SIZE=$(get_task target_size)
[ -n "$TARGET_SIZE" ] || TARGET_SIZE=$(get_log target_part_size)
IMAGE_PATH=$(get_task image_path)
say "offset=$TARGET_OFFSET size=$TARGET_SIZE pt=$PT_TYPE index=$IMAGE_INDEX repair=$REPAIR_BOOT"
say "image=$IMAGE_PATH"

# 设备枚举：优先 sysfs（只取分区），退回 /proc/partitions
list_parts(){
    _n=0
    for _p in /sys/class/block/*; do
        [ -f "$_p/partition" ] || continue
        echo "/dev/$(basename "$_p")"
        _n=$((_n + 1))
    done
    if [ "$_n" -eq 0 ] && [ -r /proc/partitions ]; then
        awk 'NR>2 && $4 != "" {print "/dev/" $4}' /proc/partitions
    fi
}

mnt_dev(){
    _dev="$1"; _mnt="$2"
    : > /tmp/zjmnt.err
    _fs=$(blkid -s TYPE -o value "$_dev" 2>/dev/null)
    case "$_fs" in
        ntfs)
            # 内核 ntfs3 优先：吞吐远高于 ntfs-3g(FUSE)，且没有 FUSE 逐文件开销
            # （实测同一台机 rm -rf/sync 走 FUSE 会卡 20s 级）。挂不上再退 ntfs-3g。
            mount -t ntfs3 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ntfs-3g -o remove_hiberfile "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t ntfs-3g "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        vfat|fat|fat32)
            mount -t vfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        exfat)
            mount -t exfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        # 光盘/ISO/UDF —— 镜像**可能就在光盘上**（用户 2026-09-23 反馈：还原系统
        # 拒绝光盘上的镜像在逻辑上说不通）。内核 sr_mod/isofs/udf 都在包内
        # （实测救援层能列出 sr0），这里显式指定，别只依赖 blkid 的猜测。
        iso9660|udf|cd9660)
            mount -t iso9660 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t udf "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        "")
            # blkid 认不出类型（用户 2026-09-23 实测：D: 分区报 fs= 空，内核自动
            # 探测只试了 ntfs3/exfat 两次、都失败 → 镜像找不到，停在 #）。这里把
            # 常见文件系统**逐个显式试**，尤其 FUSE 的 ntfs-3g —— 它比内核 ntfs3
            # 宽容，能挂某些引导扇区被改过的 NTFS（ntfs3 会报 "Primary boot
            # signature is not NTFS"）。
            mount -t ntfs3 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ntfs-3g -o remove_hiberfile "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t ntfs-3g "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t exfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t vfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t iso9660 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount -t udf "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        *)
            mount -t "$_fs" "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            # 未知/非标准类型再兜两下（同上理由）
            ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
    esac
    return 1
}

# ── 诊断日志落点（PIT-059）──
# 写到**软件目录**下（software_dir，由 Windows 侧写进 conf）：还原成功也保留，
# 供日后排查 —— 用户在软件目录里就能找到并打包发回。软件在目标盘/只读介质上时，
# Windows 侧会把 software_dir 设成数据盘 <数据盘>\ZJRESTORE（同样保留）。
ZJRDIR=ZJRESTORE
SOFTWARE_DIR=$(get_task software_dir)
# 回退：conf 没找到时 get_task 会退回读 _zjresy*.log，而日志里只有 software_path
# （exe 全路径）→ 取它所在目录当软件目录（否则日志落不到 <软件目录>/logs/）。
if [ -z "$SOFTWARE_DIR" ]; then
    _sp=$(get_task software_path)
    [ -n "$_sp" ] && SOFTWARE_DIR=$(echo "$_sp" | sed 's|\\[^\\]*$||')
fi
SD_REL=$(echo "$SOFTWARE_DIR" | cut -d: -f2- | tr '\\' '/' | sed 's|^/*||')
SOFT_MNT=""
find_soft_dir(){
    [ -n "$SOFT_MNT" ] && { echo "$SOFT_MNT"; return 0; }
    [ -n "$SD_REL" ] || return 1
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *sr*|*loop*|*cdrom*) continue ;; esac
        [ "$_d" = "${TARGET_DEV:-}" ] && continue
        mkdir -p /tmp/zj_soft
        if mnt_dev "$_d" /tmp/zj_soft; then
            if [ -d "/tmp/zj_soft/$SD_REL" ]; then
                SOFT_MNT="/tmp/zj_soft"
                echo "$SOFT_MNT"
                return 0
            fi
            umount /tmp/zj_soft 2>/dev/null
        fi
    done
    return 1
}
# 把日志复制到 <软件目录>/logs/
persist_log(){
    [ -f "$LOG" ] || return 1
    _m=$(find_soft_dir)
    if [ -z "$_m" ]; then
        say "debug log NOT persisted (software dir not found)"
        return 1
    fi
    _t="$_m/$SD_REL/logs"
    mkdir -p "$_t" 2>/dev/null
    if save_diag "$_t"; then
        sync
        say "debug log saved: <software_dir>/logs/"
        return 0
    fi
    say "debug log NOT persisted (write failed)"
    return 1
}
trap persist_log EXIT

# ── 目标分区：优先用 S99 传入的设备；否则按 offset 匹配 ──
TARGET_DEV="${1:-}"
if [ -n "$TARGET_DEV" ] && [ -b "$TARGET_DEV" ]; then
    say "target from log location: $TARGET_DEV"
else
    TARGET_DEV=""
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *sr*|*loop*|*cdrom*) continue ;; esac
        _sec=$(blkid -s PART_ENTRY_OFFSET -o value "$_d" 2>/dev/null)
        if [ -n "$_sec" ]; then
            _off=$((_sec * 512))
        else
            _num=$(echo "$_d" | grep -oE '[0-9]+$')
            _disk=$(echo "$_d" | sed 's/[0-9]*$//')
            _lba=$(dd if="$_disk" bs=1 skip=$((446 + (_num - 1) * 16 + 8)) count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
            _off=$(( ${_lba:-0} * 512 ))
        fi
        say "  probe $_d off=$_off (want $TARGET_OFFSET)"
        [ "$_off" = "$TARGET_OFFSET" ] && { TARGET_DEV="$_d"; break; }
    done
fi
[ -n "$TARGET_DEV" ] || { say "ERROR: target partition not found (offset=$TARGET_OFFSET)"; exit 1; }
say "target dev: $TARGET_DEV"

# 过了引导阶段：删掉目标分区根目录的引导期日志（/init 跑脚本前写的，PIT-059）。
# 成功还原会把目标分区格式化，它本来也会消失；如果它还在，说明引导阶段就没走完，
# 用户可把该文件发回排错。诊断日志此后都写软件目录（persist_log）。
mkdir -p /tmp/zj_bl
if mount -t ntfs3 "$TARGET_DEV" /tmp/zj_bl 2>/dev/null || mnt_dev "$TARGET_DEV" /tmp/zj_bl; then
    rm -f /tmp/zj_bl/zjrestore-boot.log 2>/dev/null
    sync
    umount /tmp/zj_bl 2>/dev/null
    say "boot-phase log cleared from target root"
fi

# ── 定位镜像文件（扫描所有分区找 IMAGE_PATH；跳过目标分区）──
IMG_FILE=""
IMG_REL=$(echo "$IMAGE_PATH" | cut -d: -f2- | tr '\\' '/' | sed 's|^/*||')
SRC_M="/tmp/zj_img"
mkdir -p "$SRC_M"
for _d in $(list_parts); do
    [ -b "$_d" ] || continue
    # ⚠️ 这里**不能**跳过光驱（sr*）：镜像可能就在光盘/ISO 上（用户 2026-09-23
    #    反馈）。只跳过 loop/ram 这类伪设备。目标分区扫描那边才该跳过 sr*。
    case "$_d" in *loop*|*ram*) continue ;; esac
    [ "$_d" = "$TARGET_DEV" ] && continue
    _typ=$(blkid -s TYPE -o value "$_d" 2>/dev/null)
    if mnt_dev "$_d" "$SRC_M"; then
        if [ -f "$SRC_M/$IMG_REL" ]; then
            IMG_FILE="$SRC_M/$IMG_REL"
            say "found image: $IMG_FILE"
            break
        fi
        say "  no image on $_d (type=${_typ:-none})"
        umount "$SRC_M" 2>/dev/null
    else
        say "mount FAILED $_d (type=${_typ:-none}; $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' '))"
    fi
done
[ -n "$IMG_FILE" ] || { say "ERROR: image not found: $IMAGE_PATH"; exit 1; }
persist_log

# ── 准备目标分区（PIT-053 修正了 PIT-052 的结论）──
# 默认 mkntfs 快速格式化。曾一度以为"Windows 引导代码不认 mkntfs 的布局"而
# 改成"保留原 NTFS 只清空文件"，那是个**误诊**：真实原因是我们移植引导区时
#   ① 只写了扇区 0 的 426 字节，漏了扇区 1..8（引导代码会读入 16 扇区后
#      `jmp 0x226` 跳到扇区 1 → 跳进全 0 → 黑屏）；
#   ② 以及后来才发现的 `dd skip=84` 错位（PIT-049）；
#   ③ 叠加 hidden sectors 写错（`--partition-start` 不对时引导代码会按错误
#      的绝对偏移读盘）。
# 2026-09-18 用 QEMU 直接 chainload 一个「mkntfs + 完整引导区 + 空根目录」的
# 分区，屏幕正确显示 "BOOTMGR is missing" —— 证明 mkntfs 布局完全可用。
# 见 tools/vmtest/README 的 PBR 探针测试。
TARGET_FS=$(blkid -s TYPE -o value "$TARGET_DEV" 2>/dev/null)
say "target fs = ${TARGET_FS:-unknown}"
# 目标分区准备方式：
#   format = mkntfs 快速格式化（默认）。2026-09-18 QEMU 实测确认可引导：
#            mkntfs 分区 + 完整微软引导区（扇区0代码 + 扇区1..8）→ 引导代码
#            能读到根目录并报 "BOOTMGR is missing"（探针镜像故意不放 bootmgr）。
#            前提是 mkntfs 必须带正确的 --partition-start/-H/-S（hidden sectors）。
#            优点：干净、无残留（wimlib 卷模式不会 rc=46）、无 FUSE 清空的卡顿。
#   keep   = 保留原 NTFS 只清空文件（历史路径，已被上面取代；留作兜底/对比）。
TARGET_MODE=${ZJ_TARGET_MODE:-format}
say "target prepare mode = $TARGET_MODE"
# 大字横幅：控制台只能用内核内置字体（ASCII），中文字形不在里面、会显示成方框，
# 所以**屏上提示一律用英文**（中文细节全部写进日志文件）。
say "======================================================"
say "   SysRecover  -  SYSTEM RESTORE IN PROGRESS"
say "   DO NOT TURN OFF OR REBOOT THE COMPUTER"
say "======================================================"
KEPT_FS=0
if [ "$TARGET_MODE" = "keep" ] && [ "$TARGET_FS" = "ntfs" ]; then
    say "keep existing NTFS, wipe files (no reformat)"
    umount "$TARGET_DEV" 2>/dev/null
    mkdir -p /tmp/zj_wipe
    _w0=$(date +%s)
    _wiped=0
    _left="?"
    # 第 1 轮：内核 ntfs3（快；FUSE 下 rm -rf 大量小文件 + sync 会卡几十秒）
    if mount -t ntfs3 "$TARGET_DEV" /tmp/zj_wipe 2>/tmp/zjwipe.err; then
        cp "$LOG" /tmp/zj_wipe/zjrestore-debug.log 2>/dev/null
        rm -rf /tmp/zj_wipe/* /tmp/zj_wipe/.[!.]* /tmp/zj_wipe/..?* 2>/tmp/zjrm.err
        sync
        _left=$(ls -A /tmp/zj_wipe 2>/dev/null | tr '\n' ' ')
        umount /tmp/zj_wipe 2>/dev/null
        _wiped=1
        # ntfs3 对 junction/重解析点/特殊 ACL 的文件可能删不掉，报错要留痕
        [ -s /tmp/zjrm.err ] && say "ntfs3 rm errors: $(head -n 3 /tmp/zjrm.err 2>/dev/null | tr '\n' ' ')"
    fi
    # 第 2 轮：ntfs3 挂不上、或没删干净（ntfs3 对少数特殊文件/属性可能删不掉）
    # → 必须用 ntfs-3g 再清一遍。否则残留文件会让 wimlib 的 NTFS 卷模式报
    # "File exists" → rc=46 (WIMLIB_ERR_NTFS_3G)。
    if [ "$_wiped" = "0" ] || [ -n "$_left" ]; then
        say "wipe: ntfs3 incomplete (mounted=$_wiped left=[$_left]) -> ntfs-3g pass"
        if ntfs-3g -o force "$TARGET_DEV" /tmp/zj_wipe 2>>/tmp/zjwipe.err \
           || mnt_dev "$TARGET_DEV" /tmp/zj_wipe; then
            cp "$LOG" /tmp/zj_wipe/zjrestore-debug.log 2>/dev/null
            rm -rf /tmp/zj_wipe/* /tmp/zj_wipe/.[!.]* /tmp/zj_wipe/..?* 2>/tmp/zjrm2.err
            sync
            _left=$(ls -A /tmp/zj_wipe 2>/dev/null | tr '\n' ' ')
            umount /tmp/zj_wipe 2>/dev/null
            _wiped=1
            [ -s /tmp/zjrm2.err ] && say "ntfs-3g rm errors: $(head -n 3 /tmp/zjrm2.err 2>/dev/null | tr '\n' ' ')"
        fi
    fi
    if [ "$_wiped" = "1" ]; then
        say "wiped target; remaining root: [$_left] (elapsed $(($(date +%s) - _w0))s)"
        # 残留 = wimlib NTFS 卷模式必失败（rc=46）。这是关键前置条件，必须显眼。
        [ -n "$_left" ] && say "WARN: target root NOT empty -> wimlib NTFS mode will likely fail (rc=46)!"
        KEPT_FS=1
    else
        say "warn: cannot mount target for wiping: $(cat /tmp/zjwipe.err 2>/dev/null | tr '\n' ' ')"
    fi
fi
if [ "$KEPT_FS" = "0" ]; then
    # 退回 mkntfs（非 NTFS 目标）。起始 LBA = 契约 target_part_offset/512：
    # BPB 的「隐藏扇区数」(0x1C) 必须等于它，否则 Windows 引导代码读盘会整体读偏。
    START_LBA=$(( ${TARGET_OFFSET:-0} / 512 ))
    [ "$START_LBA" -gt 0 ] 2>/dev/null || START_LBA=2048
    say "mkntfs $TARGET_DEV (start_lba=$START_LBA)"
    umount "$TARGET_DEV" 2>/dev/null
    dd if="$TARGET_DEV" of=/tmp/pbr_orig.bin bs=512 count=1 2>/dev/null
    mkntfs -f -S 63 -H 255 --partition-start "$START_LBA" "$TARGET_DEV" > /tmp/mkntfs.out 2>&1
    mkfs_rc=$?
    while IFS= read -r line; do say "[mkntfs] $line"; done < /tmp/mkntfs.out
    [ $mkfs_rc -eq 0 ] || { say "ERROR: mkntfs rc=$mkfs_rc"; exit 1; }
    _b0=$((START_LBA & 0xFF)); _b1=$(((START_LBA >> 8) & 0xFF))
    _b2=$(((START_LBA >> 16) & 0xFF)); _b3=$(((START_LBA >> 24) & 0xFF))
    printf "\\$(printf '%03o' $_b0)\\$(printf '%03o' $_b1)\\$(printf '%03o' $_b2)\\$(printf '%03o' $_b3)" \
        | dd of="$TARGET_DEV" bs=1 seek=28 count=4 conv=notrunc 2>/dev/null
    _hid=$(dd if="$TARGET_DEV" bs=1 skip=28 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
    [ "$_hid" = "$START_LBA" ] && say "hidden sectors = $_hid (OK)" \
                               || say "ERROR: hidden=$_hid want=$START_LBA"
fi

# ── 应用镜像 ──
# 应用模式：
#   dir   = 先用内核 ntfs3 把目标分区挂成目录，再 apply 到目录（内核回写、
#           无 FUSE/逐文件 fsync，通常最快）。**隐患**：写目录时 Windows ACL
#           （安全描述符）不一定能完整恢复，故不做默认。
#   block = wimlib 直接写块设备（libntfs-3g 用户态模式）——ACL 正确，保持默认。
APPLY_MODE=${ZJ_APPLY_MODE:-block}
say "apply image -> $TARGET_DEV (index $IMAGE_INDEX, mode=$APPLY_MODE)"
# wimlib 位置：新底座（Alpine）在 /usr/bin/wimlib-imagex；旧底座（BG-Rescue）在
# /tmp/zjtools/wimlib-imagex（S99zjrestore 预置）。两种都兼容。
WIMLIB="$(command -v wimlib-imagex 2>/dev/null)"
[ -n "$WIMLIB" ] || WIMLIB=/tmp/zjtools/wimlib-imagex
say "wimlib: $WIMLIB"
# 诊断：内存 / 镜像大小 / 目标容量（wimlib 卡住时先看这些）
say "mem: $(free 2>/dev/null | tr '\n' ' ')"
say "img bytes: $(ls -l "$IMG_FILE" 2>/dev/null | awk '{print $5}')"
say "target sectors: $(cat /sys/class/block/$(basename "$TARGET_DEV")/size 2>/dev/null)"
persist_log

# 应用目标：dir 模式先挂载（失败则退回块设备）
APPLY_TGT="$TARGET_DEV"
APPLY_MNT=""
if [ "$APPLY_MODE" = "dir" ]; then
    umount "$TARGET_DEV" 2>/dev/null
    mkdir -p /tmp/zj_apply
    if mount -t ntfs3 "$TARGET_DEV" /tmp/zj_apply 2>/tmp/zjapply.err; then
        APPLY_TGT=/tmp/zj_apply
        APPLY_MNT=/tmp/zj_apply
        say "apply target: /tmp/zj_apply (ntfs3 mounted dir)"
    else
        say "ntfs3 mount failed ($(cat /tmp/zjapply.err 2>/dev/null | tr '\n' ' ')) -> block mode"
        APPLY_MODE=block
    fi
fi

# ── NVMe I/O 超时：默认 30s ──
# 虚拟 NVMe（VMware/QEMU）偶发"完成中断丢失"：设备其实**已把完成项写进 CQ**，
# 但驱动没收到中断 → 要等满 io_timeout（30s）才去轮询 CQ，表现为每 N% 卡满 30s
# （dmesg: `nvme nvme0: I/O N QID M timeout, completion polled`）。降到 1s
# （内核参数是整数秒，最小 1，设不了 500ms）：轮询几乎总能立刻在 CQ 找到完成项，
# 卡顿 30s → 1s。（真超时仍走内核既有 abort/reset，与超时值无关。）
for _p in /sys/module/nvme_core/parameters/io_timeout /sys/module/nvme/parameters/io_timeout; do
    if [ -w "$_p" ]; then
        _old=$(cat "$_p" 2>/dev/null)
        echo 1 > "$_p" 2>/dev/null && say "nvme io_timeout: ${_old}s -> $(cat "$_p" 2>/dev/null)s ($_p)"
        break
    fi
done

# 后台采样（每 2 秒）：目标设备累计写入扇区 + 脏页/回写页。
# 用途：还原后把 /tmp/apply_io.log 留在目标分区根，就能离线看出"卡住"时
# 到底是**设备没在写**（IO/驱动问题）还是**写满了在回写**（内核 flush）。
BLK=$(basename "$TARGET_DEV")
: > /tmp/apply_io.log
: > /tmp/apply.running
(
    while [ -f /tmp/apply.running ]; do
        echo "$(date +%s) wr=$(awk '{print $7}' "/sys/class/block/$BLK/stat" 2>/dev/null) dirty=$(awk '/^Dirty:/{print $2}' /proc/meminfo 2>/dev/null) wb=$(awk '/^Writeback:/{print $2}' /proc/meminfo 2>/dev/null) load=$(cut -d' ' -f1 /proc/loadavg 2>/dev/null)" >> /tmp/apply_io.log
        sleep 2
    done
) &
_SMPID=$!

# wimlib 输出重排：把进度行（含 "(NN%)"）渲染成单行进度条，用 \r 原地刷新；
# 其它行原样输出。只按百分比解析，不依赖 wimlib 的单位/语言。
wim_progress(){
    awk '
    function bar(p,   s, i) {
        s = ""
        n = int(p / 5)
        for (i = 0; i < 20; i++) s = s (i < n ? "#" : "-")
        return "[" s "] " p "%"
    }
    {
        line = $0
        if (line == "") next
        if (match(line, /\([0-9]+%\)/)) {
            p = substr(line, RSTART + 1, RLENGTH - 3) + 0
            printf "\rZJ: %s  %s", bar(p), line
            fflush()
        } else {
            printf "\nZJ:  %s\n", line
        }
    }
    END { printf "\n" }'
}
# 进度实时透传控制台（\r 原地刷新），同时存盘；退出码单独写文件
# （管道里 $? 是 tee 的，取不到 wimlib 的）。
# 注意：rc 必须写文件传出去 —— 函数的 stdout 是进度条，$(...) 会把它一起捕获。
run_apply(){
    : > /tmp/apply.rc
    { "$WIMLIB" apply "$IMG_FILE" "$IMAGE_INDEX" "$1" 2>&1; echo "RC=$?" > /tmp/apply.rc; } \
        | tr '\r' '\n' | wim_progress | tee /tmp/apply.out
    _rc=$(sed -n 's/^RC=//p' /tmp/apply.rc 2>/dev/null)
    [ -n "$_rc" ] || _rc=1
    echo "$_rc" > /tmp/apply.exit
}
# 块设备模式用 libntfs-3g 写卷：卷必须是"干净"的。内核 ntfs3 读写挂载（上面清空
# 分区用到了）会把卷标成 dirty，libntfs-3g 会直接失败 → rc=46
# (WIMLIB_ERR_NTFS_3G)。所以 apply 前先清脏标志。
if [ "$APPLY_MODE" = "block" ]; then
    command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
fi
_a0=$(date +%s)
run_apply "$APPLY_TGT"
apply_exit=$(cat /tmp/apply.exit 2>/dev/null)
[ -n "$apply_exit" ] || apply_exit=1
if [ "$apply_exit" != "0" ]; then
    say "apply FAILED rc=$apply_exit; wimlib output head:"
    head -n 10 /tmp/apply.out 2>/dev/null | while IFS= read -r _l; do say "  |$_l"; done
    if [ "$apply_exit" = "46" ]; then
        say "rc=46 (libntfs-3g) -> ntfsfix + retry once"
        command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
        umount "$TARGET_DEV" 2>/dev/null; umount "$APPLY_TGT" 2>/dev/null
        run_apply "$APPLY_TGT"
        apply_exit=$(cat /tmp/apply.exit 2>/dev/null)
        [ -n "$apply_exit" ] || apply_exit=1
        say "retry apply rc=$apply_exit"
    fi
fi
# 最终兜底：块设备模式仍失败 → 挂成目录写入。wimlib 写目录会**覆盖**已存在
# 文件，因此即使清空没删干净也能还原成功。代价：Windows ACL（安全描述符）可能
# 不完整 —— 但"能起来的系统 + ACL 略缺"远好于"还原失败、系统起不来"。
if [ "$apply_exit" != "0" ] && [ "$APPLY_MODE" = "block" ]; then
    say "fallback -> apply to ntfs3-mounted dir (ACLs may be incomplete)"
    mkdir -p /tmp/zj_apply
    umount "$TARGET_DEV" 2>/dev/null
    # 卷脏时 ntfs3 会拒绝挂载（块模式失败后常见）→ 先清脏标志，再用 mnt_dev
    # 兜底（ntfs3 → ntfs-3g -o force）。只用 `mount -t ntfs3` 会在脏卷上失败，
    # 使兜底形同虚设。
    command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
    if mnt_dev "$TARGET_DEV" /tmp/zj_apply; then
        APPLY_MNT=/tmp/zj_apply
        run_apply /tmp/zj_apply
        apply_exit=$(cat /tmp/apply.exit 2>/dev/null)
        [ -n "$apply_exit" ] || apply_exit=1
        say "fallback apply rc=$apply_exit"
    else
        say "fallback mount failed: $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ')"
    fi
fi
_a1=$(date +%s)
say "apply rc=$apply_exit elapsed=$((_a1 - _a0))s mode=$APPLY_MODE"
rm -f /tmp/apply.running
wait "$_SMPID" 2>/dev/null
[ -n "$APPLY_MNT" ] && umount "$APPLY_MNT" 2>/dev/null
# 采样摘要：列出相邻采样间隔 >=10s 的时间点（即"卡住"发生的位置）
say "io samples: $(wc -l < /tmp/apply_io.log 2>/dev/null) (2s interval)"
say "io gaps>=10s: $(awk 'NR>1{d=$1-p; if(d>=10) printf "%d..%d(%ds) ", p, $1, d} {p=$1}' /tmp/apply_io.log 2>/dev/null | tr -d '\n')"
say "io first/last: $(head -1 /tmp/apply_io.log 2>/dev/null) | $(tail -1 /tmp/apply_io.log 2>/dev/null)"
say "--- dmesg (warn/err/blk) ---"
dmesg 2>/dev/null | grep -iE 'error|fail|warn|blocked|timeout|reset|I/O|ntfs|nvme|ata[0-9]|scsi' | tail -n 30 \
    | while IFS= read -r line; do say "  $line"; done
umount "$SRC_M" 2>/dev/null
if [ "$apply_exit" != "0" ]; then
    say "ERROR: apply rc=$apply_exit"
    persist_log
    exit 1
fi

say "apply done"

# ── 引导补全：把 \bootmgr + \Boot\* 拷进目标分区（万能镜像常缺 \Boot\BCD）──
# 文件由 Windows 暂存阶段生成（bcdedit /export 得到可移植 BCD），/init 已拷到
# /tmp/bootfix。这里**用 cp 而不是 wimlib apply**：wimlib 的 NTFS 卷模式遇到
# 已存在文件会报 "File exists"（rc=46 WIMLIB_ERR_NTFS_3G），而镜像里一般已有
# \bootmgr —— cp 会正常覆盖。
if [ -f /tmp/bootfix/bootmgr ]; then
    say "install bootfix files (\\bootmgr + \\Boot)"
    command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
    BFM=/tmp/zj_bf
    mkdir -p "$BFM"
    if mount -t ntfs3 "$TARGET_DEV" "$BFM" 2>/tmp/zjbf.err \
       || ntfs-3g -o force "$TARGET_DEV" "$BFM" 2>>/tmp/zjbf.err \
       || mnt_dev "$TARGET_DEV" "$BFM"; then
        # 先删掉镜像自带的 BCD 及其事务日志：必须保证最终用的是 Windows 侧导出的
        # **可移植** BCD，且不能留下旧日志（否则 bootmgr 载入 BCD 时可能不生效）。
        rm -f "$BFM/Boot/BCD" "$BFM/Boot/BCD.LOG" \
              "$BFM/Boot/BCD.LOG1" "$BFM/Boot/BCD.LOG2" 2>/dev/null
        cp -rf /tmp/bootfix/* "$BFM"/ 2>/tmp/zjbf.err
        bf_rc=$?
        sync
        if [ -f "$BFM/Boot/BCD" ] && [ -f "$BFM/bootmgr" ]; then
            say "bootfix BCD+bootmgr verified on target"
        else
            say "ERROR: bootfix BCD/bootmgr NOT on target after copy"
        fi
        _n=$(ls "$BFM" 2>/dev/null | tr '\n' ' ')
        umount "$BFM" 2>/dev/null
        if [ $bf_rc -eq 0 ]; then
            say "bootfix installed; target root: $_n"
        else
            say "ERROR: bootfix copy rc=$bf_rc: $(cat /tmp/zjbf.err 2>/dev/null | tr '\n' ' ')"
        fi
    else
        say "ERROR: mount target for bootfix failed: $(cat /tmp/zjbf.err 2>/dev/null | tr '\n' ' ')"
    fi
else
    say "WARN: no bootfix files, skip boot files"
fi

# ── 修复引导 ──
# 只格式化分区（不动磁盘 MBR 引导代码），所以关键是恢复分区 PBR 的引导代码。
# 引导代码来源优先级：
#   1) 随包内置 /ntfs-boot-code.bin（微软 NTFS 引导代码 426B，0x54..0x1FD）
#   2) 格式化前备份的原 PBR —— 仅在原 PBR 是 NTFS 且不是 mkntfs 自带代码时可用
#      （否则会自我复制非引导代码，开机黑屏光标闪，PIT-050）
if [ "$REPAIR_BOOT" = "1" ]; then
    if [ "$PT_TYPE" = "gpt" ]; then
        # UEFI/GPT：引导在 ESP 上（UEFI 固件 → \EFI\Microsoft\Boot\bootmgfw.efi →
        # ESP 的 BCD → 本目标分区的 winload.efi）。ESP 全程不动，所以**不需要**
        # 改分区引导区/PBR（那是 BIOS 才要做的）。
        say "UEFI/GPT: ESP untouched, skip PBR/boot-region repair"
    elif [ "$KEPT_FS" = "1" ]; then
        # 保留了原有 NTFS → 分区引导区通常原封未动，无需改写。但要防「上一次
        # 失败还原把引导区写坏」：若扇区 1 全 0（缺 Windows 引导代码的后续部分）
        # 则用内置的完整引导区修复（第一扇区只写代码段，BPB 保持本分区自己的）。
        _s1=$(dd if="$TARGET_DEV" bs=512 skip=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
        if echo "$_s1" | grep -q '[1-9a-f]'; then
            say "boot region kept intact (sector1 non-empty)"
        elif [ -f /ntfs-boot-code.bin ] && [ -f /ntfs-boot-cont.bin ]; then
            say "boot region incomplete -> rewrite built-in boot region"
            dd if=/ntfs-boot-code.bin of="$TARGET_DEV" bs=1 seek=84 count=426 conv=notrunc 2>/dev/null
            dd if=/ntfs-boot-cont.bin of="$TARGET_DEV" bs=512 seek=1 conv=notrunc 2>/dev/null
            sync
            _g=$(dd if="$TARGET_DEV" bs=1 skip=84 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
            [ "$_g" = "fa33c08e" ] && say "boot region rewritten (0x54=$_g)" \
                                   || say "ERROR: boot region rewrite failed (0x54=$_g)"
        else
            say "WARN: boot region incomplete and no built-in blob"
        fi
    elif [ -f /ntfs-boot-code.bin ] && [ -f /ntfs-boot-cont.bin ]; then
        # 这里是 mkntfs 新建的分区：mkntfs 只写它自己的"不可引导"占位代码
        # （扇区里能读到 "This is not a bootable disk..."），必须整体换成微软
        # NTFS 引导区才能引导 Windows。
        # 微软引导区是**两部分**、缺一不可（PIT-053）：
        #   · 扇区 0 的 0x54..0x1FD（426B 代码段；BPB 保留 mkntfs 写好的）
        #   · 扇区 1..8（4096B 后续代码）—— 第一段会把 16 个扇区读进内存，
        #     再 `jmp 0x226` 跳到扇区 1 继续执行。只写第一段 = 跳进全 0 =
        #     开机黑屏光标闪、无任何报错。
        say "write MS NTFS boot region (code 426B + sectors 1..8)"
        dd if=/ntfs-boot-code.bin of="$TARGET_DEV" bs=1 seek=84 count=426 conv=notrunc 2>/dev/null
        dd if=/ntfs-boot-cont.bin of="$TARGET_DEV" bs=512 seek=1 conv=notrunc 2>/dev/null
        # 卷末尾的备份引导扇区也更新，防 ntfsfix 日后从备份"修"回占位代码
        _psz=$(cat "/sys/class/block/$(basename "$TARGET_DEV")/size" 2>/dev/null)
        _last=$(( ${_psz:-0} - 1 ))
        [ "$_last" -gt 0 ] 2>/dev/null && \
            dd if=/ntfs-boot-code.bin of="$TARGET_DEV" bs=1 skip=0 seek=$((_last * 512 + 84)) count=426 conv=notrunc 2>/dev/null
        sync
        _g1=$(dd if="$TARGET_DEV" bs=1 skip=84 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
        _g2=$(dd if="$TARGET_DEV" bs=512 skip=1 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$_g1" = "fa33c08e" ] && echo "$_g2" | grep -q '[1-9a-f]'; then
            say "boot region OK (sector0 0x54=$_g1, sector1 non-empty)"
        else
            say "ERROR: boot region BAD (0x54=$_g1, sector1 empty=$([ -z "$(echo $_g2 | tr -d 0)" ] && echo yes || echo no))"
        fi
    elif command -v ms-sys >/dev/null 2>&1; then
        DISK_DEV=$(echo "$TARGET_DEV" | sed 's/[0-9]*$//')
        ms-sys -f --mbr7 "$DISK_DEV" >> "$LOG" 2>&1
        ms-sys -f --ntfs "$TARGET_DEV" >> "$LOG" 2>&1
        say "boot repair via ms-sys done"
    else
        say "ERROR: no built-in boot blobs (/ntfs-boot-code.bin, /ntfs-boot-cont.bin) and no ms-sys"
    fi
fi

say "========================================="
say "RESTORE DONE: $TARGET_DEV (index $IMAGE_INDEX)"
say "time: $(date '+%F %T')"
say "========================================="

# ── 还原成功后清理本次留下的文件（用户要求：还原完不留痕）──
# 目标分区（C:）不再写任何日志；其它分区上的还原环境（ZJRESTORE/grldr/
# grldr.mbr/menu.lst）与日志一并清掉。**只在成功路径执行**：失败时脚本提前
# 退出，靠 trap persist_log 把日志留在非目标分区，便于排查/重试。
# 只清「我们铺的」分区（有 grldr.mbr 或 ZJRESTORE 才动），绝不碰用户的镜像文件。
cleanup_after_success(){
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        [ "$_d" = "$TARGET_DEV" ] && continue
        case "$_d" in *sr*|*loop*|*cdrom*) continue ;; esac
        mkdir -p /tmp/zj_cl
        if mnt_dev "$_d" /tmp/zj_cl; then
            _did=0
            # 旧版本遗留在数据盘根目录的引导文件（新布局下不会再有）
            if [ -f /tmp/zj_cl/grldr ] || [ -f /tmp/zj_cl/grldr.mbr ] || \
               [ -f /tmp/zj_cl/menu.lst ]; then
                rm -f /tmp/zj_cl/grldr /tmp/zj_cl/grldr.mbr \
                      /tmp/zj_cl/menu.lst 2>/dev/null
                _did=1
            fi
            # 旧版本遗留的救援目录（含 boot/vmlinuz）；日志目录不含它，不会误删
            if [ -f "/tmp/zj_cl/$ZJRDIR/boot/vmlinuz-zjrestore" ]; then
                rm -rf "/tmp/zj_cl/$ZJRDIR" 2>/dev/null
                _did=1
            fi
            [ "$_did" = "1" ] && { sync; say "cleaned legacy boot files on $_d"; }
            # UEFI/GPT：ESP 上的 <ESP>\EFI\$ZJRDIR 是**常驻启动还原模块**，
            # 还原成功后**保留**（开机启动菜单里仍能进救援 —— 用户要求：Windows
            # 蓝屏/引导损坏时也能用）。要清只能由 Windows 侧「删除启动还原」执行。
            # 注意：这说明 ~50MB 内核+initramfs 会长期占 ESP（ESP 一般 ≥100MB）。
            umount /tmp/zj_cl 2>/dev/null
            command -v ntfsfix >/dev/null 2>&1 && ntfsfix -d "$_d" >> "$LOG" 2>&1
        fi
    done
}

persist_log          # 把完整日志落到 ZJRESTORE\logs\（cleanup 会保留该子目录）
trap - EXIT          # 清理阶段不要再触发 persist_log 把日志写回去
cleanup_after_success
sync
reboot -f 2>/dev/null || reboot
