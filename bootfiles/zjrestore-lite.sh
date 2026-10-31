#!/bin/sh
# zjrestore-lite.sh — 轻量还原脚本（生产版）
# S99zjrestore 已把 _zjresy*.log / restore-task.conf / zjtools 放到 /tmp，
# 并把「目标分区设备」作为 $1 传入（日志写在目标分区根，故日志所在分区即目标）。
# 控制台无中文字库 → 消息全 ASCII。日志尽量持久化到非目标分区（重启后可取）。
set -u
LOG=/tmp/zjrestore.log
# export：zjswap-undo.sh（子进程）要把 SWAPUNDO 行追加进同一份日志，否则
# 反向交换失败时 /tmp 已清、控制台已滚 → 现场一行证据都没有（PIT-141）。
export LOG
# 注意：控制台输出一律走 **stderr**（1>&2）——函数返回值/设备名等靠 stdout，
# 而 `ESP_DEV=$(find_esp_dev)` 这类命令替换会捕获函数内**所有 stdout**；若
# say 走 stdout，"mnt <dev>" 屏幕标记会被一起捕获污染 ESP_DEV（2026-10-05 客户
# 实测：esp mount failed 'ZJ:   mnt /dev/sda1 /dev/sda1'）。stderr 同样打到控制台，
# 屏幕/串口日志不受影响。
# say 单源实现收进 /zjlog.sh（P0-B/PIT-145）：控制台 "ZJ: " 前缀不变（断言
# 契约），文件行改带前缀时间戳 —— 5 个救援脚本合并成一条可分辨的时间线。
ZJPREFIX=ZJ
. /zjlog.sh
# 黑匣子探测表（2026-10-04 客户失败的教训）：每个分区的 mount/open 结果都记在
# 这里（与 init 共用同一文件），失败时随日志写到所有可写面 —— "为什么找不到"
# 从此有据可查（客户那次只剩屏幕视频、扫描阶段没有任何文件证据）。
PROBE=/tmp/zj-probe.txt
[ -f "$PROBE" ] || : > "$PROBE" 2>/dev/null
probe(){ echo "$(date '+%F %T') $*" >> "$PROBE" 2>/dev/null; say "PROBE $*"; }
# 结果状态（写进 ZJRESTORE-status.txt 回执）：关键步骤更新；失败退出前 FAILED；
# 成功置 OK。init 用 /tmp/zj-bb.done 判断"脚本是否已经扫过"，避免重复全扫。
RESULT=UNKNOWN
STEP=boot
# 兜底日志窝（用户 2026-10-04 规格 3）：软件目录找不到时，在非目标分区认领一个
# ZJRESTORE-logs/ 并**保持挂载**，整个流程持续镜像日志（防中途断电/被杀导致
# "哪里都没有日志"）。收集（日志按钮）后会连同其它散落日志一起清掉。
BB_HOME=""
# 目标分区设备：**在脚本开头就置空**（PIT-138）。zz-blackbox 模式会在
# "解析目标分区"之前就调用 bb_home_drop / bb_esp_drop(→find_esp_dev)，而后者
# 里有裸引用 —— set -u 下引用未定义变量会直接 rc=2，导致 init 的兜底黑匣子
# 写盘失败（2026-10-12 现场：`blackbox sweep start` 之后没有任何落盘）。
TARGET_DEV=""
# zz-blackbox：init 在"脚本没跑/没来得及写盘"时调用的黑匣子模式（见函数区）。
ZZ_BLACKBOX=0
[ "${1:-}" = "zz-blackbox" ] && ZZ_BLACKBOX=1
# 失败排查用：把内核环形缓冲最近 80 行打进日志（坏盘 I/O、驱动报错都在里面）
say_dmesg(){
    dmesg 2>/dev/null | tail -n 80 | while IFS= read -r _l; do say "  |$_l"; done
}
# 控制台文本转储（/dev/vcs*；用户 2026-10-05 规格）：把"屏幕内容"也存进日志。
# 内核/救援的输出都在 VT；vcs 是字符矩阵（每格 1 字节），粗折行即可。
dump_console(){
    _got=0
    for _v in /dev/vcs /dev/vcs1 /dev/vcs2; do
        [ -r "$_v" ] || continue
        say "---- console text ($_v) ----"
        head -c 4096 "$_v" 2>/dev/null | tr '\000' ' ' | fold -w 80 | head -n 60 \
            | while IFS= read -r _l; do say "  #$_l"; done
        _got=1
    done
    [ "$_got" = "1" ] || say "(console text unavailable: no readable /dev/vcs *)"
}
# wimlib 错误码 → 人话（排查 apply 失败/回退原因用；常量见 third_party/wimlib/wimlib.h）
rc_name(){
    case "$1" in
        37) echo "MKDIR(create dir failed)" ;;
        46) echo "NTFS_3G(libntfs-3g open/write failed: dirty volume? write error? lib bug?)" ;;
        47) echo "OPEN" ;;
        48) echo "OPENDIR" ;;
        50) echo "READ" ;;
        51) echo "READLINK" ;;
        59) echo "SET_SECURITY(failed to set Windows ACL)" ;;
        68) echo "UNSUPPORTED(this mode not supported)" ;;
        69) echo "UNSUPPORTED_FILE" ;;
        72) echo "WRITE" ;;
        *) echo "" ;;
    esac
}
# 日志 + apply 采样/输出一起留档（离线分析卡顿用）
save_diag(){
    cp "$LOG" "$1/zjrestore-debug.log" 2>/dev/null || return 1
    cp /tmp/apply_io.log "$1/zjrestore-io.log" 2>/dev/null
    cp /tmp/apply.out "$1/zjrestore-apply.out" 2>/dev/null
    return 0
}

say "zjrestore-lite start"

# 取第一个 _zjresy*.log（不用 `ls ... | head -1`：那种写法遇到含空格的文件名会出问题）
TARGET_LOG=""
for _f in /tmp/_zjresy*.log; do
    [ -f "$_f" ] && { TARGET_LOG="$_f"; break; }
done
TASK_CONF="/tmp/restore-task.conf"
if [ "$ZZ_BLACKBOX" != "1" ] && [ -z "$TARGET_LOG" ] && [ ! -f "$TASK_CONF" ]; then
    RESULT=FAILED; STEP=no-task
    say "no log and no conf, abort"
    exit 1
fi
[ -n "$TARGET_LOG" ] || say "warn: no _zjresy log, using conf only"
[ -f "$TASK_CONF" ] || say "warn: no conf, using log only"

# 取值为防呆都去掉尾部 \r：契约正常是 LF（task.cpp 用 \n），但若有人用记事本
# 编辑过/别处生成的 CRLF 版本，路径/数字会带 \r 导致静默失配（如 image not found）。
get_log(){
    [ -n "$TARGET_LOG" ] || return 0
    grep "^$1=" "$TARGET_LOG" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r'
}
get_task(){
    if [ -f "$TASK_CONF" ]; then
        _v=$(grep "^$1=" "$TASK_CONF" 2>/dev/null | head -1 | cut -d= -f2- | tr -d '\r')
        [ -n "$_v" ] && { echo "$_v"; return; }
    fi
    get_log "$1"
}

# 契约版本握手（问题清单 G1）：Windows 侧在 conf / _zjresy 日志里写 `contract_version=<n>`；
# 救援层只认自己的 `ZJ_CONTRACT`。不匹配就**明确报错**（而不是字段语义漂移后静默出错）。
# 老任务没有这个键 → 视为 1（引入该键之前的版本），保持向后兼容。
ZJ_CONTRACT=1
_CV=$(get_task contract_version)
[ -n "$_CV" ] || _CV=1
if [ "$ZZ_BLACKBOX" != "1" ] && [ "$_CV" != "$ZJ_CONTRACT" ]; then
    RESULT=FAILED; STEP=contract-mismatch
    say "ERROR: contract_version mismatch: task=$_CV rescue=$ZJ_CONTRACT"
    say "ERROR: 程序与救援层版本不匹配（契约 v$_CV vs v$ZJ_CONTRACT），请用同一版本重新暂存"
    exit 1
fi
say "contract_version=$_CV (ok)"

IMAGE_INDEX=$(get_task image_index)
[ -n "$IMAGE_INDEX" ] || IMAGE_INDEX=1
# 方案 C（用户 2026-09-30 规格）：镜像里 ESP 分区备份子镜像的 index。
# Windows 暂存时从镜像内容发现后写进契约；0/缺省 = 镜像里没有（绝大多数任务）。
ESP_INDEX=$(get_task esp_index)
case "$ESP_INDEX" in ''|0) ESP_INDEX="" ;; esac
# ZJEA 修复子镜像的 index（PIT-122）。**必须与 esp_index 一起在开头解析**：
# A 引擎（engine=swap）带子镜像时要在"镜像扫描"之前就能判断要不要找镜像
# （PIT-138：原实现在脚本尾部才读 → 前面判不到）。
EA_INDEX=$(get_task ea_index)
case "$EA_INDEX" in ''|0) EA_INDEX="" ;; esac
# 加密镜像（ZJENC1）：Windows 暂存阶段已把 ESP/EA 子镜像解到**目标分区**的
# ZJRESTORE/{espfix,eafix}，救援层只做拷贝（它读不了加密容器）。空 = 旧行为。
ESP_FIX_DIR=$(get_task esp_fix_dir)
EA_FIX_DIR=$(get_task ea_fix_dir)
REPAIR_BOOT=$(get_task repair_boot)
PT_TYPE=$(get_task pt_type)
TARGET_OFFSET=$(get_task target_offset)
[ -n "$TARGET_OFFSET" ] || TARGET_OFFSET=$(get_log target_part_offset)
TARGET_SIZE=$(get_task target_size)
[ -n "$TARGET_SIZE" ] || TARGET_SIZE=$(get_log target_part_size)
IMAGE_PATH=$(get_task image_path)
say "offset=$TARGET_OFFSET size=$TARGET_SIZE pt=$PT_TYPE index=$IMAGE_INDEX repair=$REPAIR_BOOT"
say "image=$IMAGE_PATH"
[ -n "$ESP_INDEX" ] && say "esp subimage: index=$ESP_INDEX (restore to ESP after main apply)"
# A 引擎（0.7）：engine=swap 时目标分区已由 Windows 侧解压出 ~new（含 .zj-done），
# Linux 侧只做改名交换，不做 mkntfs / wimlib apply。缺省 block = 产线 B 引擎。
ENGINE=$(get_task engine)
case "$ENGINE" in swap|block) ;; *) ENGINE=block ;; esac
say "engine=$ENGINE"

# 设备枚举（用户 2026-10-04 规格）：内置盘分区 → 可移动盘分区 → **无分区的可移动
# 整盘**（光盘/未分区 U 盘）。U 盘/光盘放最后但**不丢**（镜像可能合法放在上面，
# 这些情况必须被记录）。sysfs 全空时才退回 /proc/partitions。
is_removable(){
    _rp=$(readlink -f "/sys/class/block/$1" 2>/dev/null)
    [ -n "$_rp" ] || return 1
    if [ -f "$_rp/removable" ]; then _rd="$_rp"; else _rd="$(dirname "$_rp")"; fi
    [ "$(cat "$_rd/removable" 2>/dev/null)" = "1" ]
}
list_parts(){
    _n=0
    for _p in /sys/class/block/*; do
        [ -f "$_p/partition" ] || continue
        _b=$(basename "$_p")
        if ! is_removable "$_b"; then echo "/dev/$_b"; _n=$((_n + 1)); fi
    done
    for _p in /sys/class/block/*; do
        [ -f "$_p/partition" ] || continue
        _b=$(basename "$_p")
        if is_removable "$_b"; then echo "/dev/$_b"; _n=$((_n + 1)); fi
    done
    for _p in /sys/class/block/*; do
        [ -f "$_p/partition" ] && continue
        _b=$(basename "$_p")
        case "$_b" in loop*|ram*|dm-*) continue ;; esac
        if is_removable "$_b"; then echo "/dev/$_b"; _n=$((_n + 1)); fi
    done
    if [ "$_n" -eq 0 ] && [ -r /proc/partitions ]; then
        awk 'NR>2 && $4 != "" {print "/dev/" $4}' /proc/partitions
    fi
}

mnt_dev(){
    _dev="$1"; _mnt="$2"
    # 屏幕级"进行中"标记（2026-10-05）：挂死时最后一屏就是卡住的设备
    say "  mnt $_dev"
    : > /tmp/zjmnt.err
    _fs=$(timeout 30 blkid -s TYPE -o value "$_dev" 2>/dev/null)
    case "$_fs" in
        ntfs)
            # 内核 ntfs3 优先：吞吐远高于 ntfs-3g(FUSE)，且没有 FUSE 逐文件开销
            # （实测同一台机 rm -rf/sync 走 FUSE 会卡 20s 级）。挂不上再退 ntfs-3g。
            timeout 60 mount -t ntfs3 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err; _r=$?
            [ $_r -eq 0 ] && return 0
            [ $_r -eq 124 ] && echo " TIMEOUT(60s:ntfs3)" >> /tmp/zjmnt.err
            timeout 60 ntfs-3g -o remove_hiberfile "$_dev" "$_mnt" 2>>/tmp/zjmnt.err; _r=$?
            [ $_r -eq 0 ] && return 0
            [ $_r -eq 124 ] && echo " TIMEOUT(60s:ntfs-3g)" >> /tmp/zjmnt.err
            timeout 60 ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t ntfs-3g "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        vfat|fat|fat32)
            timeout 60 mount -t vfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        exfat)
            timeout 60 mount -t exfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        # 光盘/ISO/UDF —— 镜像**可能就在光盘上**（用户 2026-09-23 反馈：还原系统
        # 拒绝光盘上的镜像在逻辑上说不通）。内核 sr_mod/isofs/udf 都在包内
        # （实测救援层能列出 sr0），这里显式指定，别只依赖 blkid 的猜测。
        iso9660|udf|cd9660)
            timeout 60 mount -t iso9660 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t udf "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        "")
            # blkid 认不出类型（用户 2026-09-23 实测：D: 分区报 fs= 空，内核自动
            # 探测只试了 ntfs3/exfat 两次、都失败 → 镜像找不到，停在 #）。这里把
            # 常见文件系统**逐个显式试**，尤其 FUSE 的 ntfs-3g —— 它比内核 ntfs3
            # 宽容，能挂某些引导扇区被改过的 NTFS（ntfs3 会报 "Primary boot
            # signature is not NTFS"）。
            timeout 60 mount -t ntfs3 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 ntfs-3g -o remove_hiberfile "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t ntfs-3g "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t exfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t vfat "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t iso9660 "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount -t udf "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
        *)
            timeout 60 mount -t "$_fs" "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            # 未知/非标准类型再兜两下（同上理由）
            timeout 60 ntfs-3g -o force "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            timeout 60 mount "$_dev" "$_mnt" 2>>/tmp/zjmnt.err && return 0
            ;;
    esac
    return 1
}

# 设备是否已经挂着（不关心挂载点）：重复 mount 同一设备的行为因文件系统而异
# （有的允许、有的 EBUSY）——扫描循环统一先查 /proc/mounts 再决定挂不挂。
is_mounted(){
    awk -v d="$1" '$1==d{f=1} END{exit f?0:1}' /proc/mounts 2>/dev/null
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
# 找软件目录分区并**记住挂载点**。注意 SOFT_MNT 必须在**当前 shell** 里赋值 →
# 不能再写成 `_m=$(find_soft_dir)`（命令替换跑在子 shell，缓存赋值会丢；后续
# 每次重扫时挂载点已被第一次占用 → 挂载失败 → 报 "software dir not found"，
# 于是软件目录里的日志永远停在第一次快照 —— 客户日志停在 found image 的真凶）。
# 成功返回 0，调用方直接读全局 $SOFT_MNT。
find_soft_dir(){
    if [ -n "$SOFT_MNT" ] && [ -d "$SOFT_MNT/$SD_REL" ]; then
        return 0
    fi
    [ -n "$SD_REL" ] || return 1
    SOFT_MNT=""
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        # fd/floppy 也跳过（QEMU 软驱无软件目录，挂它纯浪费时间）
        case "$_d" in *sr*|*loop*|*cdrom*|*fd*|*floppy*) continue ;; esac
        [ "$_d" = "${TARGET_DEV:-}" ] && continue
        is_mounted "$_d" && continue
        mkdir -p /tmp/zj_soft
        if mnt_dev "$_d" /tmp/zj_soft; then
            if [ -d "/tmp/zj_soft/$SD_REL" ]; then
                probe "softscan dev=$_d mount=ok dir=yes"
                SOFT_MNT="/tmp/zj_soft"
                return 0
            fi
            probe "softscan dev=$_d mount=ok dir=no"
            umount /tmp/zj_soft 2>/dev/null
        else
            probe "softscan dev=$_d mount=FAIL err=$(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ' | head -c 150)"
        fi
    done
    return 1
}
# ── 日志实时镜像（2026-10-01 客户实测教训）──
# /tmp 是 RAM，重启即失；以前只在固定点 persist_log，一旦脚本卡死（坏盘 I/O、
# mkntfs、apply…），软件目录里只剩"最后一个固定点"的旧快照 —— 客户那两份日志
# 都停在 found image，卡在哪一步无从得知。这里起后台循环，每 2 秒把完整日志 +
# apply 输出镜像到软件目录；脚本退出（含失败路径的 EXIT trap）时停掉。
MIRROR_STARTED=0
start_log_mirror(){
    [ "$MIRROR_STARTED" = "1" ] && return 0
    [ -n "$SOFT_MNT" ] || [ -n "$BB_HOME" ] || return 0
    : > /tmp/zj_mirror.on
    (
        while [ -f /tmp/zj_mirror.on ]; do
            if [ -n "$SOFT_MNT" ]; then
                cp "$LOG" "$SOFT_MNT/$SD_REL/logs/zjrestore-debug.log" 2>/dev/null
                cp /tmp/apply.out "$SOFT_MNT/$SD_REL/logs/zjrestore-apply.out" 2>/dev/null
                cp /tmp/apply_io.log "$SOFT_MNT/$SD_REL/logs/zjrestore-io.log" 2>/dev/null
            fi
            if [ -n "$BB_HOME" ]; then
                cp "$LOG" "$BB_HOME/zjrestore-debug.log" 2>/dev/null
                cp /tmp/apply.out "$BB_HOME/zjrestore-apply.out" 2>/dev/null
                cp /tmp/apply_io.log "$BB_HOME/zjrestore-io.log" 2>/dev/null
            fi
            sleep 2
        done
    ) &
    MIRROR_STARTED=1
    say "log mirror: on (every 2s)"
}
stop_log_mirror(){
    [ "$MIRROR_STARTED" = "1" ] || return 0
    rm -f /tmp/zj_mirror.on
    MIRROR_STARTED=0
}
# 把日志复制到 <软件目录>/logs/；软件目录找不到时退到兜底日志窝 BB_HOME
# （用户 2026-10-04 规格 3：先找地方放，保证"最终一定能找到日志"）。
persist_log(){
    [ -f "$LOG" ] || return 1
    if find_soft_dir; then
        _t="$SOFT_MNT/$SD_REL/logs"
        mkdir -p "$_t" 2>/dev/null
        if save_diag "$_t"; then
            sync
            say "debug log saved: <software_dir>/logs/"
            start_log_mirror
            return 0
        fi
        say "debug log NOT persisted (write failed)"
        return 1
    fi
    if [ -n "$BB_HOME" ] && save_diag "$BB_HOME"; then
        sync
        say "debug log saved: fallback ZJRESTORE-logs"
        start_log_mirror
        return 0
    fi
    say "debug log NOT persisted (software dir not found; see ZJRESTORE-probe.txt)"
    return 1
}
# ── 黑匣子（2026-10-04 客户失败的教训）──
# 原则：**任何失败都必须留下文件，且落在 Windows 侧能找到的地方**（用户不再
# 需要录像/截图）。证据面（按可达顺序）：
#   · 软件目录 <software_dir>/logs/（persist_log，原有）
#   · 每个可挂载分区根：ZJRESTORE-last.log + ZJRESTORE-probe.txt + ZJRESTORE-status.txt
#   · ESP（FAT 根下有 EFI/ZJRESTORE）→ 写进 \EFI\ZJRESTORE\logs\（常驻、Windows 可读）
# 失败时全量写（EXIT trap）；成功时只写软件目录 + ESP（**不写还原后的目标盘**）。
# /tmp/zj-bb.done 记录已写设备：init 用它判断"脚本是否已经扫过"，避免重复全扫。
BB_DONE=/tmp/zj-bb.done
: > "$BB_DONE" 2>/dev/null
bb_status_text(){
    echo "time=$(date '+%F %T')"
    echo "build=$(cat /zjrescue-version 2>/dev/null)"
    echo "result=$RESULT"
    echo "step=$STEP"
    echo "target=${TARGET_DEV:-}"
    echo "image=${IMAGE_PATH:-}"
}
bb_drop(){
    [ -d "$1" ] || return 1
    _t="$1"
    # ESP：写进常驻救援目录，别在 ESP 根乱扔文件
    if [ -d "$1/EFI/ZJRESTORE" ]; then
        mkdir -p "$1/EFI/ZJRESTORE/logs" 2>/dev/null && _t="$1/EFI/ZJRESTORE/logs"
    fi
    cp "$LOG" "$_t/ZJRESTORE-last.log" 2>/dev/null || return 1
    cp "$PROBE" "$_t/ZJRESTORE-probe.txt" 2>/dev/null
    bb_status_text > "$_t/ZJRESTORE-status.txt" 2>/dev/null
    for _f in mkntfs.out:ZJRESTORE-mkntfs.out apply.out:ZJRESTORE-apply.out esp.out:ZJRESTORE-esp.out; do
        [ -s "/tmp/${_f%%:*}" ] && cp "/tmp/${_f%%:*}" "$_t/${_f#*:}" 2>/dev/null
    done
    sync 2>/dev/null
    return 0
}
# 黑匣子专用挂载：短超时、少回退（失败路径上别被坏盘拖死）
bb_mount(){
    mkdir -p /tmp/zj_bb
    umount /tmp/zj_bb 2>/dev/null
    # 每次调用重置错误收集（失败原因不再丢 /dev/null —— 全败时回吐给日志，PIT-147）
    : > /tmp/zjbb.err
    _fs=$(timeout 20 blkid -s TYPE -o value "$1" 2>/dev/null)
    case "$_fs" in
        vfat|fat|fat32) timeout 20 mount -t vfat "$1" /tmp/zj_bb 2>/tmp/zjbb.err && return 0 ;;
        exfat) timeout 20 mount -t exfat "$1" /tmp/zj_bb 2>/tmp/zjbb.err && return 0 ;;
        ntfs|"")
            timeout 30 mount -t ntfs3 "$1" /tmp/zj_bb 2>/tmp/zjbb.err && return 0
            timeout 30 ntfs-3g -o force "$1" /tmp/zj_bb 2>>/tmp/zjbb.err && return 0
            [ -n "$_fs" ] || { timeout 20 mount -t vfat "$1" /tmp/zj_bb 2>>/tmp/zjbb.err && return 0; } ;;
        *) timeout 20 mount "$1" /tmp/zj_bb 2>/tmp/zjbb.err && return 0 ;;
    esac
    if [ -s /tmp/zjbb.err ]; then
        say "bb_mount failed: $1 (fs=${_fs:-?}) $(head -n 2 /tmp/zjbb.err | tr '\n' ' ')"
    else
        say "bb_mount failed: $1 (fs=${_fs:-?})"
    fi
    return 1
}
bb_sweep(){
    # 黑匣子开扫前，先把"屏幕内容"（控制台文本）也收进日志（只做一次）
    if [ ! -f /tmp/zj-vcs.done ]; then
        : > /tmp/zj-vcs.done
        dump_console
    fi
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *loop*|*ram*|*sr*|*fd*|*floppy*) continue ;; esac
        grep -qx "$_d" "$BB_DONE" 2>/dev/null && continue
        # 已经挂着的（镜像/软件/目标分区）直接用现挂载点，别再挂一次
        _mp=$(awk -v d="$_d" '$1==d{print $2; exit}' /proc/mounts 2>/dev/null)
        if [ -n "$_mp" ]; then
            if bb_drop "$_mp"; then
                echo "$_d" >> "$BB_DONE"
                say "blackbox: log -> $_mp ($_d, already mounted)"
            fi
            continue
        fi
        if bb_mount "$_d"; then
            if bb_drop /tmp/zj_bb; then
                echo "$_d" >> "$BB_DONE"
                say "blackbox: log -> $_d"
            else
                say "blackbox: write FAILED on $_d"
            fi
            umount /tmp/zj_bb 2>/dev/null
        else
            say "blackbox: mount FAILED $_d (no place to write log)"
        fi
    done
    return 0
}
bb_esp_drop(){
    _e=$(find_esp_dev 2>/dev/null)
    [ -n "$_e" ] || return 0
    _mp=$(awk -v d="$_e" '$1==d{print $2; exit}' /proc/mounts 2>/dev/null)
    if [ -n "$_mp" ]; then
        bb_drop "$_mp" && say "blackbox: log -> $_e (ESP)"
        return 0
    fi
    if bb_mount "$_e"; then
        bb_drop /tmp/zj_bb && say "blackbox: log -> $_e (ESP)"
        umount /tmp/zj_bb 2>/dev/null
    fi
}
# 兜底日志窝落盘（PIT-137，用户 2026-10-08 规格："以后只写一个固定地方，
# 不要每个分区乱放"）：固定三处 —— ①软件目录（persist_log 已写）②兜底窝
# <数据盘>\ZJRESTORE-logs\（此刻没有就现认领一个非目标分区）③ESP 的
# \EFI\ZJRESTORE\logs\（UEFI，格式化也幸存）。Windows 侧每次启动会把这些
# 收集进 logs\collected 并清掉源头。
bb_home_drop(){
    # 屏幕内容（控制台文本）也收进日志（只做一次；无文件可写时的唯一线索）
    if [ ! -f /tmp/zj-vcs.done ]; then
        : > /tmp/zj-vcs.done
        dump_console
    fi
    if [ -z "$BB_HOME" ]; then
        # 同一次引导里前一次已认领的兜底窝直接复用：否则 zz-blackbox 二次进入时
        # 它还挂着（会被下面 is_mounted 跳过），其它分区又都被占 → 拿不到日志面
        # （PIT-138 现场：init 报 `WARN: no writable surface found for logs`）。
        _hb=$(awk '$2=="/tmp/zj_home2"{print $1; exit}' /proc/mounts 2>/dev/null)
        if [ -n "$_hb" ] && mkdir -p /tmp/zj_home2/ZJRESTORE-logs 2>/dev/null; then
            BB_HOME=/tmp/zj_home2/ZJRESTORE-logs
            say "fallback log home (reused): $_hb -> ZJRESTORE-logs/"
        fi
    fi
    if [ -z "$BB_HOME" ]; then
        for _d in $(list_parts); do
            [ -b "$_d" ] || continue
            case "$_d" in *loop*|*ram*|*sr*|*cdrom*|*fd*|*floppy*) continue ;; esac
            # ⚠️ 必须 `${TARGET_DEV:-}`：zz-blackbox 模式下本函数在"解析目标分区"
            #    之前就被调用，裸引用未定义变量会让 set -u 直接 rc=2 → init 的兜底
            #    黑匣子写盘失败（PIT-138，2026-10-12 现场）。
            [ "$_d" = "${TARGET_DEV:-}" ] && continue
            is_mounted "$_d" && continue
            mkdir -p /tmp/zj_home2
            if mnt_dev "$_d" /tmp/zj_home2; then
                if mkdir -p /tmp/zj_home2/ZJRESTORE-logs 2>/dev/null; then
                    BB_HOME=/tmp/zj_home2/ZJRESTORE-logs
                    say "fallback log home (late): $_d -> ZJRESTORE-logs/"
                    break
                fi
                umount /tmp/zj_home2 2>/dev/null
            fi
        done
    fi
    [ -n "$BB_HOME" ] && bb_drop "$BB_HOME" && say "blackbox: log -> $BB_HOME (fallback home)"
    return 0
}
bb_final(){
    if [ "$RESULT" = "OK" ]; then
        persist_log
        bb_esp_drop
    else
        # 失败时把内核环形缓冲打进日志（坏盘 I/O、ata/nvme reset、ntfs 报错都在
        # 这里）——"从记录中评估"（用户 2026-10-05 规格）。
        say_dmesg
        persist_log
        # PIT-137：只写"兜底窝 + ESP"（不再扫每个分区根乱放）
        bb_home_drop
        bb_esp_drop
    fi
    type unlock_cleanup >/dev/null 2>&1 && unlock_cleanup
    stop_log_mirror
    # ── P3（PIT-148）失败退场决策：按 STAGE 决定 reboot / retry / menu ──
    if [ "$RESULT" = "FAILED" ] && [ "${ZZ_BLACKBOX:-0}" != "1" ]; then
        case $STAGE in
            0)
                say "recover decision: reboot (target untouched, STEP=$STEP)"
                echo reboot > /tmp/zj-recover-decision
                ;;
            1)
                say "recover decision: menu (system files in place, boot files unverified)"
                echo menu > /tmp/zj-recover-decision
                ;;
            2)
                say "recover decision: reboot (boot files verified, new system should boot)"
                echo reboot > /tmp/zj-recover-decision
                ;;
            3|4)
                _n=$(cat /tmp/zj-retry 2>/dev/null || echo 0)
                _n=$((_n + 1))
                echo "$_n" > /tmp/zj-retry
                if [ "$_n" -le 2 ]; then
                    say "recover decision: retry (attempt $_n/2, stage=$STAGE STEP=$STEP)"
                    echo retry > /tmp/zj-recover-decision
                else
                    say "recover decision: menu (retries exhausted, stage=$STAGE)"
                    echo menu > /tmp/zj-recover-decision
                fi
                ;;
            *)
                say "recover decision: menu (unknown stage=$STAGE)"
                echo menu > /tmp/zj-recover-decision
                ;;
        esac
    fi
}
trap 'bb_final' EXIT
# zz-blackbox 模式：init 调 `zjrestore-lite.sh zz-blackbox <reason>`；只做全盘
# 日志落盘（逻辑单源，init 不重复实现一套），不碰契约/还原。
if [ "$ZZ_BLACKBOX" = "1" ]; then
    RESULT=FAILED
    STEP="${2:-interrupted}"
    say "blackbox sweep start (reason=$STEP)"
    trap - EXIT
    # PIT-137：只写"兜底窝 + ESP"（不再扫每个分区根乱放）
    bb_home_drop
    bb_esp_drop
    say "blackbox sweep done (home+ESP)"
    exit 0
fi

# ── P3 失败自动回退（PIT-148）：目标可启动性阶段标记 ──
# STAGE 由主流程在关键跃迁处更新；EXIT trap（bb_final）在 RESULT=FAILED 时据它
# 写 /tmp/zj-recover-decision（reboot|retry|menu），由 init 自动退场——不再把
# 用户丢在裸 # shell：
#   0=未动目标  1=文件已就位但引导未验证  2=引导已验证（可启动）
#   3=已格式化未 apply（B 引擎，可安全重跑）  4=returnold 进行中（phase 可续做）
STAGE=0

# ── 目标分区：优先用 S99 传入的设备，但**必须过 offset 校验** ──
# 为什么要校验（2026-10-04）：参照机日志里出现过 `target from log location:
# /dev/sda1` 而契约目标在别的设备 —— 只要哪个分区根上残留一份 _zjresy*.log
# （拷日志去别处、旧测试），init 就会把"第一个找到日志的分区"当目标；不校验
# 的话格式化的是**错误的分区**。校验不过就回退按 offset 精确匹配。
dev_offset(){
    # 首选 **sysfs `start`**（单位=512B 扇区）：GPT 分区、MBR 主分区、EBR 逻辑盘
    # 全都准确，且不依赖 blkid 行为（复刻测试实证：GPT 盘上 blkid 的
    # PART_ENTRY_OFFSET 可能为空，而 MBR 兜底对 GPT 只能读到保护性 MBR → 0，
    # 会把**正确的目标**误判成 stale log 拒掉）。
    _st=$(cat "/sys/class/block/$(basename "$1")/start" 2>/dev/null)
    if [ -n "$_st" ]; then echo $((_st * 512)); return 0; fi
    _sec=$(timeout 30 blkid -s PART_ENTRY_OFFSET -o value "$1" 2>/dev/null)
    if [ -n "$_sec" ]; then echo $((_sec * 512)); return 0; fi
    # 兜底：blkid 没给出时读 MBR 分区表（先验扇区 0 的 55aa；整盘名用 sysfs 父目录）。
    # 每个外部命令都套 timeout：读软驱/坏块设备上的裸 dd 会在 TCG 里阻塞几十秒
    # 而上层毫无输出（PIT-142：returnold 演练卡在本函数，日志停在 match by offset）。
    _name=$(basename "$1")
    _disk="/dev/$(basename "$(dirname "$(readlink -f "/sys/class/block/$_name")")")"
    _sig=$(timeout 10 dd if="$_disk" bs=1 skip=510 count=2 2>/dev/null | od -A n -t x1 | tr -d ' \n')
    _num=$(echo "$_name" | grep -oE '[0-9]+$')
    [ "$_sig" = "55aa" ] && [ -n "$_num" ] && [ -b "$_disk" ] || return 1
    _lba=$(timeout 10 dd if="$_disk" bs=1 skip=$((446 + (_num - 1) * 16 + 8)) count=4 2>/dev/null | od -A n -t u4 | tr -d ' ')
    [ -n "$_lba" ] || return 1
    echo $((_lba * 512))
}
# 分区字节数（sysfs size 单位=512B 扇区；GPT/MBR/EBR 逻辑盘全通用）。
# 用途：offset 匹配的**第二判据**（多盘首分区都在 1MiB，只比 offset 会选错盘）。
dev_size(){
    _sz=$(cat "/sys/class/block/$(basename "$1")/size" 2>/dev/null)
    [ -n "$_sz" ] && echo $((_sz * 512))
}
target_by_offset(){
    TARGET_DEV=""
    # 先把候选设备打出来：这一段是"日志缺失/被拒后"的唯一路径，一旦卡住或
    # 空转，没有这行就只能看到上一条 say（PIT-142 现场：日志停在 "match by
    # offset" 之后零输出，无从判断是扫到 fd0 卡住还是 list_parts 为空）。
    say "  target_by_offset: scanning (parts=$(list_parts 2>/dev/null | tr '\n' ' '))"
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        # sr/loop/cdrom 之外补 fd/floppy：QEMU 模拟软驱 /dev/fd0 上的裸 dd 会
        # 阻塞几十秒且零输出（PIT-142 现场诱因之一；真实目标永不可能是软驱）。
        case "$_d" in *sr*|*loop*|*cdrom*|*fd*|*floppy*) continue ;; esac
        _off=$(dev_offset "$_d")
        if [ -z "$_off" ]; then
            say "  (skip $_d: no offset)"
        else
            _sz=$(dev_size "$_d")
            say "  probe $_d off=$_off size=${_sz:-?} (want $TARGET_OFFSET/${TARGET_SIZE:-?})"
            if [ "$_off" = "$TARGET_OFFSET" ]; then
                if [ -n "$TARGET_SIZE" ] && [ -n "$_sz" ] && [ "$_sz" != "$TARGET_SIZE" ]; then
                    # 同 offset 但 size 不符 = 不是目标（2026-10-06 实测教训：两块盘
                    # 首分区都在 1MiB，且 Linux 给两块 IDE 盘的 sd 字母会变 —— 只比
                    # offset 会把**镜像盘**误认成目标盘；补 size 校验杜绝）
                    say "  WARN: $_d offset matches but size=$_sz != $TARGET_SIZE -> skip"
                    probe "target-reject dev=$_d offset-ok size-mismatch $_sz/$TARGET_SIZE"
                    continue
                fi
                TARGET_DEV="$_d"; return 0
            fi
        fi
    done
    return 1
}
TARGET_DEV="${1:-}"
if [ -n "$TARGET_DEV" ] && [ -b "$TARGET_DEV" ] && [ -n "$TARGET_OFFSET" ]; then
    _doff=$(dev_offset "$TARGET_DEV")
    _dsz=$(dev_size "$TARGET_DEV")
    if [ -n "$_doff" ] && [ "$_doff" != "$TARGET_OFFSET" ]; then
        say "WARN: candidate $TARGET_DEV offset=$_doff != contract $TARGET_OFFSET -> reject (stale log?)"
        probe "target-reject dev=$TARGET_DEV offset=$_doff want=$TARGET_OFFSET"
        TARGET_DEV=""
    elif [ -n "$TARGET_SIZE" ] && [ -n "$_dsz" ] && [ "$_dsz" != "$TARGET_SIZE" ]; then
        say "WARN: candidate $TARGET_DEV size=$_dsz != contract $TARGET_SIZE -> reject (stale log?)"
        probe "target-reject dev=$TARGET_DEV size=$_dsz want=$TARGET_SIZE"
        TARGET_DEV=""
    fi
fi
if [ -n "$TARGET_DEV" ] && [ -b "$TARGET_DEV" ]; then
    say "target from log location: $TARGET_DEV (offset verified)"
else
    say "target from log rejected/absent -> match by offset"
    target_by_offset
    if [ -z "$TARGET_DEV" ] && [ -n "$TARGET_OFFSET" ]; then
        # A1：设备/分区枚举可能晚到（控制器 probe、U 盘/HDD 唤醒）—— 第二轮再给一次机会
        say "target not found on first pass -> retry after 3s"
        sleep 3
        target_by_offset
    fi
    [ -n "$TARGET_DEV" ] || { RESULT=FAILED; STEP=target-not-found; say "ERROR: target partition not found (offset=$TARGET_OFFSET)"; exit 1; }
fi
say "target dev: $TARGET_DEV"
STEP=target-found

# 兜底日志窝（用户 2026-10-04 规格 3）：尽早认领一个非目标可写分区并**保持挂载**，
# 让日志从此刻起持续镜像到那里 —— 就算之后中途断电/被杀，也有可收集的日志。
# 软件目录能找到时不用它（日志照旧进软件目录）。收集（日志按钮）后会被清掉。
claim_bb_home(){
    [ -n "$BB_HOME" ] && return 0
    if find_soft_dir; then
        start_log_mirror
        return 0
    fi
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *loop*|*ram*|*sr*|*cdrom*|*fd*|*floppy*) continue ;; esac
        [ "$_d" = "$TARGET_DEV" ] && continue
        is_mounted "$_d" && continue
        mkdir -p /tmp/zj_home
        if mnt_dev "$_d" /tmp/zj_home; then
            # 这个分区也许就是软件目录所在（之前瞬时打不开）→ 直接当软件目录用
            if [ -n "$SD_REL" ] && [ -d "/tmp/zj_home/$SD_REL" ]; then
                SOFT_MNT=/tmp/zj_home
                say "log home: software dir found late on $_d"
                start_log_mirror
                return 0
            fi
            if mkdir -p /tmp/zj_home/ZJRESTORE-logs 2>/dev/null; then
                BB_HOME=/tmp/zj_home/ZJRESTORE-logs
                save_diag "$BB_HOME" 2>/dev/null
                say "fallback log home: $_d -> ZJRESTORE-logs/ (software dir not found)"
                start_log_mirror
                return 0
            fi
            umount /tmp/zj_home 2>/dev/null
        fi
    done
    say "WARN: no fallback log home found (all non-target mounts failed)"
    return 1
}

# ── 能力自检（P1，PIT-146）：动手之前先验证"本环境能不能干这件事" ──
# 用户 2026-10-14 口径：不能老是"出问题 → 卡 # → 用户发日志 → 补一个"——
# 打包缺件（wimlib/mkntfs/ntfs-3g/blkid 被裁）、引导脚本缺失这类**源头缺陷**，
# 必须在 mkntfs/apply/改名之前以结构化 CAPS 行报告失败，而不是中途 rc=xxx 才发现。
# 致命项缺失 → RESULT=FAILED STEP=caps-missing 退出（此时还没碰目标分区）；
# 非致命（内存小 / offset 空 / 日志出口未 claim）→ 仅 `CAPS warn:` 照常继续。
_capbin(){ # $1=命令：不存在 / 无法执行(126/127) 视为缺失；--version 返回其它码不误判
    command -v "$1" >/dev/null 2>&1 || return 1
    timeout 10 "$1" --version >/dev/null 2>&1
    case $? in 126|127) return 1 ;; esac
    return 0
}
_wim_ok(){ # wimlib 解析与 L758 同口径（PATH → /tmp/zjtools 老底座兜底）
    _wl="$(command -v wimlib-imagex 2>/dev/null)"
    [ -n "$_wl" ] || _wl=/tmp/zjtools/wimlib-imagex
    [ -f "$_wl" ] || return 1
    timeout 10 "$_wl" --version >/dev/null 2>&1
    case $? in 126|127) return 1 ;; esac
    return 0
}
caps_selfcheck(){
    _bad=""
    _warn=""
    if [ -b "$TARGET_DEV" ]; then
        # count=0 = 只做 O_WRONLY 打开探测、零字节写入（无副作用，不开销）——
        # 提前暴露"目标只读/被写保护"，否则要等 mkntfs 才以怪异 rc 失败
        if ! dd if=/dev/zero of="$TARGET_DEV" bs=512 count=0 2>/dev/null; then
            _bad="$_bad target-write"
        fi
    else
        _bad="$_bad target-not-blockdev"
    fi
    _capbin blkid   || _bad="$_bad blkid"      # mnt_dev 认文件系统（busybox 版已按 PIT-047 移除）
    _capbin ntfs-3g || _bad="$_bad ntfs-3g"    # B 引擎 libntfs-3g 写卷 + 各挂载兜底 + undo FUSE 专用
    if [ "$UNDO_MARK" = "1" ]; then
        # 反向交换：只要 undo 脚本 + 挂载能力（不碰镜像/mkntfs/wimlib）
        [ -f /zjswap-undo.sh ] || _bad="$_bad zjswap-undo.sh"
    elif [ "$ENGINE" = "swap" ]; then
        # A 引擎主交换：zjswap.sh；契约带 ESP/EA 子镜像时仍需 wimlib 解包（PIT-138）
        [ -f /zjswap.sh ] || _bad="$_bad zjswap.sh"
        if [ -n "$ESP_INDEX" ] || [ -n "$EA_INDEX" ]; then
            _wim_ok || _bad="$_bad wimlib-imagex(needed-by-esp/ea)"
        fi
        # 加密镜像：ESP/EA 已由 Windows 暂存到目标盘，只需 cp（不读密文镜像）
        if [ -n "$ESP_FIX_DIR" ] || [ -n "$EA_FIX_DIR" ]; then
            command -v cp >/dev/null 2>&1 || _bad="$_bad cp(needed-by-encrypted-esp/ea)"
        fi
    else
        # B 引擎：wimlib + mkntfs
        _wim_ok || _bad="$_bad wimlib-imagex"
        _capbin mkntfs || _bad="$_bad mkntfs"
    fi
    # 块设备可见（存储模块已载）：target 已找到通常意味着非空，仍防打包/驱动回归
    [ -n "$(list_parts)" ] || _bad="$_bad blockdevs(none-visible)"
    # 非致命观察项
    [ -n "$TARGET_OFFSET" ] || _warn="$_warn task(target_part_offset-empty)"
    _mt=$(awk '/^MemTotal/{print $2}' /proc/meminfo 2>/dev/null)
    if [ -n "$_mt" ] && [ "$_mt" -lt 409600 ]; then
        _warn="$_warn mem($((_mt / 1024))MB<400,PIT-113)"
    fi
    if [ -z "$BB_HOME" ] && [ -z "$SOFT_MNT" ]; then
        # claim_bb_home 没 claim 到：末尾 bb_final 还会全盘扫一次，但中途失败的
        # 证据可能只在屏幕/内存里 → 响亮警告
        _warn="$_warn log-home(none-claimed)"
    fi
    say "CAPS engine=$ENGINE undo=$UNDO_MARK target=$TARGET_DEV"
    [ -n "$_warn" ] && say "CAPS warn:$_warn"
    if [ -n "$_bad" ]; then
        say "CAPS result=MISSING:$_bad"
        say "ERROR: rescue environment incomplete -> abort BEFORE touching target (initramfs packaging?)"
        RESULT=FAILED
        STEP=caps-missing
        persist_log
        exit 1
    fi
    say "CAPS result=ok"
    return 0
}
claim_bb_home

# ── 返回旧系统（action=returnold，PIT-139）──
# A 引擎 keepOld 备返的撤销：Windows 侧只做「暂存契约 + 部署引导 + 单次启动 +
# 重启」，目录反向交换（根 -> ~/.new 后丢弃；~/.old -> 根）**一律在这里完成** ——
# 在 Windows 里改名正在运行的系统树不可回滚，见 docs/23 §5（方案 C）。
# 放在镜像扫描**之前**：这条路不需要镜像、不 mkntfs、不 wimlib apply。
# 前置失败安全由 zjswap-undo.sh 把守（~/.old 缺失/不含 Windows → exit 2 绝不碰盘）。
ACTION=$(get_task action)
UNDO_MARK=0
if [ "$ACTION" = "returnold" ]; then
    UNDO_MARK=1
    say "action=returnold: reverse swap on target (no image, no mkntfs, no apply)"
elif [ "$ACTION" = "restore" ] || [ -z "$ACTION" ]; then
    # 安全网（PIT-139 T3）：契约里写着 restore/空，目标上却留着自家反向交换
    # 的阶段标记 —— 典型是"返回旧系统"被中断后，用户又在 Windows 侧重新
    # 暂存了一次普通还原。此时不接管就会先 mkntfs 把 ~old（仅存的旧系统）
    # 抹掉；接管则先完成反向交换，用户再重来一次还原即可。
    # （契约文件本身由 zjswap-undo.sh L5 留在根，正常续做不走这条 ——
    #  那条路径契约里仍是 action=returnold。）
    # （BIOS 下引导命脉 grldr/grldr.mbr/menu.lst 由 zjswap-undo.sh 留在根，
    #  正是为了这一刻仍能进救援层。）
    _ud=/tmp/zj_undo_chk
    mkdir -p "$_ud"
    if timeout 60 mount -t ntfs3 "$TARGET_DEV" "$_ud" 2>/dev/null || mnt_dev "$TARGET_DEV" "$_ud"; then
        if [ -f "$_ud/~new/.zj-undo-phase" ]; then
            UNDO_MARK=1
            say "safety net: ~/.new/.zj-undo-phase found on target -> resume return-old"
        fi
        umount "$_ud" 2>/dev/null
    fi
fi

# 能力自检（P1，PIT-146）：returnold / A 引擎交换 / B 引擎格式化三条路全覆盖 ——
# 在动 target（改名/mkntfs/apply）之前确认环境能力，缺件则带 CAPS 报告 fail-closed
caps_selfcheck

if [ $UNDO_MARK -eq 1 ]; then
    STEP=returnold-swap
    STAGE=4
    ZJSWAPU="$(command -v zjswap-undo.sh 2>/dev/null)"
    [ -n "$ZJSWAPU" ] || ZJSWAPU=/zjswap-undo.sh
    if [ ! -f "$ZJSWAPU" ]; then
        RESULT=FAILED; STEP=returnold-missing
        say "ERROR: zjswap-undo.sh not found in initramfs"
        persist_log
        exit 1
    fi
    # 反向交换是**同一卷上的大规模 rename + rm**；后台日志镜像每 2s 还在
    # 往同一卷（$BB_HOME 常在目标分区上）写文件 —— 并发写入者与批量删除
    # 并存没有好处，先停（PIT-141）。收尾的 persist_log 会按需重新拉起。
    stop_log_mirror
    _s0=$(date +%s)
    sh "$ZJSWAPU" "$TARGET_DEV" /tmp/zj_swap_undo_mnt
    _urc=$?
    _s1=$(date +%s)
    say "return-old rc=$_urc elapsed=$((_s1 - _s0))s"
    if [ $_urc -ne 0 ]; then
        RESULT=FAILED; STEP=returnold-failed
        case $_urc in
            2) say "~old absent or has no Windows -> nothing to return; target untouched" ;;
            3) say "leftover ~new present -> previous return-old interrupted; re-run from the boot menu" ;;
            *) say "zjswap-undo.sh failed rc=$_urc" ;;
        esac
        say_dmesg
        persist_log
        exit 1
    fi
    # 引导补救已由本段隐式完成：旧系统的 bootmgr/Boot/Windows 随 ~/.old 回到根，
    # 与它们是配套的一套（BIOS）；UEFI 下 ESP 全程未动，BCD 仍指向分区 GUID +
    # \Windows，而 \Windows 现在又是旧系统了 —— 两条链都不需要再跑 bootfix。
    # （bootfix 载荷在 L5 已随 ~/.new 被删除，此时再跑必失败，故直接收尾。）
    say "RETURN-OLD DONE: old system back at root; new tree discarded"
    RESULT=OK
    STEP="done"
    persist_log
    exit 0
fi

# 过了引导阶段：删掉目标分区根目录的引导期日志（/init 跑脚本前写的，PIT-059）。
# 成功还原会把目标分区格式化，它本来也会消失；如果它还在，说明引导阶段就没走完，
# 用户可把该文件发回排错。诊断日志此后都写软件目录（persist_log）。
mkdir -p /tmp/zj_bl
if timeout 60 mount -t ntfs3 "$TARGET_DEV" /tmp/zj_bl 2>/dev/null || mnt_dev "$TARGET_DEV" /tmp/zj_bl; then
    rm -f /tmp/zj_bl/zjrestore-boot.log 2>/dev/null
    sync
    umount /tmp/zj_bl 2>/dev/null
    say "boot-phase log cleared from target root"
fi

# ── 定位镜像文件（扫描所有分区找 IMAGE_PATH；跳过目标分区）──
# 拆成函数 + 两轮扫描：模块加载后磁盘/分区可能出现晚、或某个分区瞬时打不开
# （2026-09-04 客户"image not found"快速退出的最大嫌疑）——第二轮给一次机会。
# A 引擎（engine=swap）**主 apply 不需要**镜像文件：Windows 侧已解压完成，这里
# 只做交换；但契约带 esp_index/ea_index 时子镜像仍要从镜像里解 —— 见下方
# "A 引擎带子镜像"补扫段（PIT-138）。
IMG_FILE=""
IMG_REL=$(echo "$IMAGE_PATH" | cut -d: -f2- | tr '\\' '/' | sed 's|^/*||')
SRC_M="/tmp/zj_img"
mkdir -p "$SRC_M"
: > /tmp/zj_img.fail
# WIMLIB **无条件**提前解析（PIT-138，2026-10-12 现场日志定位）：A 引擎（swap）
# 不 apply 主镜像，但契约带 esp_index/ea_index 时仍要用它解子镜像。原实现只在
# B 引擎分支里赋值 → swap 下 `$WIMLIB` 未定义，`set -u` 直接 rc=2 中断在 ESP 段
# （现场：swap rc=0 → `restore esp subimage idx=2` → 之后既无 `esp restored`
# 也无 `RESTORE DONE`、无状态回执，init 报 `restore script returned (rc=2)`）。
# 位置：新底座（Alpine）在 /usr/bin/wimlib-imagex；旧底座（BG-Rescue）在
# /tmp/zjtools/wimlib-imagex（S99zjrestore 预置）。两种都兼容。
WIMLIB="$(command -v wimlib-imagex 2>/dev/null)"
[ -n "$WIMLIB" ] || WIMLIB=/tmp/zjtools/wimlib-imagex
scan_image(){
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        # ⚠️ 这里**不能**跳过光驱（sr*）：镜像可能就在光盘/ISO 上（用户 2026-09-23
        #    反馈）。只跳过 loop/ram 这类伪设备；fd/floppy 也跳（WIM 最小也有几 MB，
        #    软盘装不下镜像）。目标分区扫描那边才该跳过 sr*。
        case "$_d" in *loop*|*ram*|*fd*|*floppy*) continue ;; esac
        [ "$_d" = "$TARGET_DEV" ] && continue
        _rem=""; is_removable "$(basename "$_d")" && _rem="1"
        # 已挂着的（软件目录/兜底日志窝）直接查现挂载点，别再挂一次
        _mp=$(awk -v d="$_d" '$1==d{print $2; exit}' /proc/mounts 2>/dev/null)
        if [ -n "$_mp" ]; then
            if [ -f "$_mp/$IMG_REL" ]; then
                IMG_FILE="$_mp/$IMG_REL"
                probe "image-scan dev=$_d rem=${_rem:-0} mount=ok(already) found=yes"
                say "found image: $IMG_FILE (dev=$_d rem=${_rem:-0})"
                return 0
            fi
            probe "image-scan dev=$_d rem=${_rem:-0} mount=ok(already) found=no"
            say "  no image on $_d (already mounted)"
            continue
        fi
        _typ=$(timeout 30 blkid -s TYPE -o value "$_d" 2>/dev/null)
        if mnt_dev "$_d" "$SRC_M"; then
            if [ -f "$SRC_M/$IMG_REL" ]; then
                IMG_FILE="$SRC_M/$IMG_REL"
                probe "image-scan dev=$_d type=${_typ:-none} rem=${_rem:-0} mount=ok found=yes"
                say "found image: $IMG_FILE (dev=$_d rem=${_rem:-0})"
                return 0
            fi
            probe "image-scan dev=$_d type=${_typ:-none} rem=${_rem:-0} mount=ok found=no"
            say "  no image on $_d (type=${_typ:-none})"
            umount "$SRC_M" 2>/dev/null
        else
            echo "$_d" >> /tmp/zj_img.fail
            if [ -z "$_typ" ]; then
                probe "image-scan dev=$_d type=none rem=${_rem:-0} mount=FAIL err=no filesystem"
                say "skip $_d (no filesystem)"
            else
                probe "image-scan dev=$_d type=$_typ rem=${_rem:-0} mount=FAIL err=$(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ' | head -c 150)"
                say "mount FAILED $_d (type=$_typ; $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ' | head -c 200))"
            fi
        fi
    done
    return 1
}

# ── 加密镜像（ZJENC2）：loop + dm-crypt 映射成块设备（2026-10-29，docs/30 §11）──
# 为什么要在 Linux 侧解密：B 引擎（重启后还原）本来就是在救援层 apply，Windows
# 侧解一份临时明文 = 多占一整份空间 + 多一遍全量读写。这里直接映射：
#   容器：header 4096B（明文）+ meta + 补齐到 1MB，正文 = CBC-ESSIV-SHA256 / 4096B 扇区
#   dm 表：0 <sectors> crypt aes-cbc-essiv:sha256 <keyhex> 0 <loop> 2048 1 sector_size:4096
# 口令从**控制台**读（不回显）；没有控制台/没输入 → fail-closed，绝不动目标分区。
unlock_image(){
    [ -n "$IMG_FILE" ] || return 0
    case "$IMG_FILE" in /dev/mapper/*) return 0 ;; esac
    _magic=$(dd if="$IMG_FILE" bs=1 count=6 2>/dev/null)
    [ "$_magic" = "ZJENC2" ] || return 0

    say "encrypted image detected (ZJENC2) -> password required"
    _zpwd="${ZJ_PASSWORD:-}"
    if [ -z "$_zpwd" ] && [ -r /dev/console ]; then
        printf '\n  Image password: ' > /dev/console
        # shellcheck disable=SC2039
        IFS= read -r -s _zpwd < /dev/console
        printf '\n' > /dev/console
    fi
    if [ -z "$_zpwd" ]; then
        RESULT=FAILED; STEP=enc-password
        say "ERROR: encrypted image requires a password (none entered) - aborted BEFORE touching the target"
        persist_log
        exit 1
    fi
    _salt=$(od -An -tx1 -j 32 -N 16 "$IMG_FILE" | tr -d ' \n\r')
    _iters=$(od -An -tu4 -j 16 -N 4 "$IMG_FILE" | tr -d ' \n\r')
    _plain=$(od -An -tu8 -j 24 -N 8 "$IMG_FILE" | tr -d ' \n\r')
    if [ -z "$_salt" ] || [ -z "$_iters" ] || [ -z "$_plain" ]; then
        RESULT=FAILED; STEP=enc-header
        say "ERROR: cannot parse encrypted image header"
        persist_log
        exit 1
    fi
    # PBKDF2-HMAC-SHA256(口令, salt, iters) → **64B**：encKey(32) + macKey(32)。
    # 必须用 openssl kdf（enc -P 只给 key+iv=48B，拿不到 macKey，也就没法验头 MAC）。
    _k=$(openssl kdf -keylen 64 -kdfopt digest:SHA256 -kdfopt "pass:$_zpwd" \
        -kdfopt "hexsalt:$_salt" -kdfopt "iter:$_iters" PBKDF2 2>/dev/null |
        tr -d ' \r\n:' | tr 'A-F' 'a-f')
    _zpwd=""
    if [ "${#_k}" -ne 128 ]; then
        RESULT=FAILED; STEP=enc-key
        say "ERROR: key derivation failed (openssl kdf len=${#_k}, expected 128 hex)"
        persist_log
        exit 1
    fi
    _key=${_k%????????????????????????????????????????????????????????????????}
    _mkey=${_k#????????????????????????????????????????????????????????????????}
    # 头 MAC 校验（错口令在这里就拦住，而不是等到 wimlib 报"不是 WIM"）
    _hmac=$(od -An -tx1 -j 48 -N 32 "$IMG_FILE" | tr -d ' \n\r')
    _calc=$(head -c 48 "$IMG_FILE" |
        openssl dgst -sha256 -mac HMAC -macopt "hexkey:$_mkey" 2>/dev/null |
        sed 's/.*= //' | tr -d ' \n\r' | tr 'A-F' 'a-f')
    if [ "$_calc" != "$_hmac" ]; then
        RESULT=FAILED; STEP=enc-password
        say "ERROR: wrong image password (header MAC mismatch)"
        say "  (ask again: this encrypted image cannot be restored without the password)"
        persist_log
        exit 1
    fi
    say "encrypted image: password ok (header MAC verified)"
    for _m in loop dm_mod dm_crypt cbc essiv aesni_intel; do
        modprobe "$_m" 2>/dev/null
    done
    _loop=$(losetup -f 2>/dev/null)
    [ -n "$_loop" ] || _loop=/dev/loop0
    if ! losetup -r "$_loop" "$IMG_FILE"; then
        RESULT=FAILED; STEP=enc-loop
        say "ERROR: losetup failed on $_loop"
        persist_log
        exit 1
    fi
    ZJ_LOOP="$_loop"
    # 正文按 512B 扇区向上取整（与 Windows 侧、以及容器实际大小一致；
    # 多算一点会让 dm 表越过 loop 设备末尾 → reload ioctl EINVAL）
    _body=$(( (_plain + 511) / 512 * 512 ))
    _sec=$(( _body / 512 ))
    if ! dmsetup create zjenc --table "0 $_sec crypt aes-cbc-essiv:sha256 $_key 0 $_loop 2048"; then
        RESULT=FAILED; STEP=enc-dm
        say "ERROR: dmsetup create failed (dm-crypt mapping rejected)"
        dmsetup remove zjenc 2>/dev/null
        losetup -d "$_loop" 2>/dev/null
        ZJ_LOOP=""
        persist_log
        exit 1
    fi
    dmsetup mknodes 2>/dev/null
    _key=""
    IMG_FILE=/dev/mapper/zjenc
    if [ ! -b "$IMG_FILE" ]; then
        RESULT=FAILED; STEP=enc-node
        say "ERROR: /dev/mapper/zjenc not created"
        unlock_cleanup
        persist_log
        exit 1
    fi
    say "encrypted image unlocked: $IMG_FILE (loop=$ZJ_LOOP sectors=$_sec)"
}

unlock_cleanup(){
    [ -n "${ZJ_LOOP:-}" ] || return 0
    dmsetup remove zjenc 2>/dev/null
    losetup -d "$ZJ_LOOP" 2>/dev/null
    ZJ_LOOP=""
}

if [ "$ENGINE" != "swap" ]; then
scan_image
if [ -z "$IMG_FILE" ]; then
    say "image not found on first pass -> retry after 3s"
    sleep 3
    scan_image
fi
# A1 第三轮：只在"出现过挂载失败"时再等 10s 重试（瞬时 I/O 错误恢复/设备唤醒）
if [ -z "$IMG_FILE" ] && [ -s /tmp/zj_img.fail ]; then
    say "image still not found (mount failures seen) -> final retry after 10s"
    sleep 10
    scan_image
fi
[ -n "$IMG_FILE" ] || { RESULT=FAILED; STEP=image-not-found; say "ERROR: image not found: $IMAGE_PATH"; exit 1; }
unlock_image
STEP=image-found
persist_log

# ── 镜像内容预检（I-1，2026-10-06 实测定稿；docs/19 批次 G）──
# **规则：镜像里必须有 \Windows\system32\winload.exe（Win7+ 的加载器）**，
# 否则在**动目标分区之前**拒绝，杜绝"格式化完才发现起不来"。覆盖：
#   · Vista 以前（XP/2003/2000/98）系统镜像 → 没有 winload → 拒绝 ✓
#   · 安装源类"镜像"（如 \I386 结构；实测某 XP esd 连 ntoskrnl 都没有）→ 拒绝 ✓
#   · 合成/局部镜像（演练盘 test.wim）→ 拒绝（演练素材已补 dummy winload.exe）
# 实现：`extract` 单个路径探测（solid ESD 只解相关块）——
#   ⚠️ 不能用 `dir`：实测在 solid ESD 上会**卡死**（2026-10-06）。
#   ⚠️ Linux 侧 wimlib 路径匹配**区分大小写**（Windows 侧不区分）：真实镜像
#      里是 `System32`（大写 S），必须**双大小写各探一次**，否则会误拒（2026-10-06 实测）。
# 带 timeout：探测卡住/超时按"跳过"处理（fail-open，只记日志不误杀）。
# ⚠️ `$WIMLIB` 已在本段之前**无条件**解析（PIT-138），此处直接可用
#    （set -u 下引用未定义变量会直接 rc=2 退出，2026-10-06 / 2026-10-12 实测踩到）。
mkdir -p /tmp/zj_pc
_pc_ok=0
for _p in '/Windows/System32/winload.exe' '/Windows/system32/winload.exe'; do
    if timeout 240 "$WIMLIB" extract "$IMG_FILE" "$IMAGE_INDEX" \
            "$_p" --dest-dir=/tmp/zj_pc --no-acls --no-attributes \
            > /tmp/zjpc.out 2>&1; then
        _pc_ok=1
        break
    fi
done
if [ "$_pc_ok" = "1" ]; then
    say "image precheck: winload.exe present (ok)"
else
    RESULT=FAILED; STEP=unsupported-image
    say "  (probe out: $(tail -c 200 /tmp/zjpc.out 2>/dev/null | tr '\n' ' '))"
    say "ERROR: image lacks \\Windows\\system32\\winload.exe -> not a Windows 7+ system image"
    say "ERROR: (pre-Vista system / install-source image / partial image) - aborted BEFORE touching the target"
    persist_log
    exit 1
fi

# ── Linux 无法正确还原的内容预检（1019 案 W1 + W2，2026-10-19；**动目标分区之前**）──
# B 引擎（block）在 Linux 侧用 wimlib（NTFS-3G 块模式）apply，两类内容还原不了：
#   ① 特定重解析点（PIT-135 + 1019 修订：非微软 tag、junction 落非目录文件等）
#      → rc=58 中止在 **mkntfs 之后**（目标分区半成品）；
#   ② EFS 加密文件（W2 实测）→ apply **rc=0 但文件变 0 字节**（wimlib 只打
#      "Ignoring EFS-encrypted data"）—— 静默数据丢失。
# 这里在 mkntfs 之前拦截：检出 → 拒绝（引导改走 A 引擎「还原后重启」或 PE
# 就地还原）；**扫描无法完成 → 也拒绝**（fail-closed —— 放行等于赌它们不发生，
# 赌输了就是格盘后失败/数据丢失，见 1019 案用户哲学"杜绝格完盘才失败"）。
# 快路径：Windows 暂存侧已扫描干净（reparse_scan=0 **且** efs_scan=0 —— 两键都
# 是 0 才算证书：0.7.70 版 exe 只写 reparse_scan，缺 efs_scan 时必须全量扫描，
# 否则旧证书会漏掉 EFS）且镜像字节数一致（image_size == 实际 stat）→ 直接认证。
# 老契约（缺键）/大小不符 → 走兜底扫描。
# 兜底扫描：`dir --detailed` 只读元数据（不写盘；用法见 imagex.c:4388——
# WIMFILE [IMAGE] 之后跟 --detailed）。awk 单遍同时数 "Attributes" 哨兵行、
# 非微软重解析点（tag 十六进制小写无前导零：长度<8 或 =8 且首字符 [0-7]）与
# EFS 加密项（imagex.c:2602-2615 的属性行 `    FILE_ATTRIBUTE_ENCRYPTED is set`）。
# ⚠️ busybox 无 PIPESTATUS：子壳里 `echo "DIRRC=$?" >&2` 把 rc 带出来。
# ⚠️ `$WIMLIB` 已在镜像扫描段之前无条件解析（PIT-138），此处直接可用。
_rs_cert=0
if [ "$(get_task reparse_scan)" = "0" ] && [ "$(get_task efs_scan)" = "0" ]; then
    _fp=$(get_task image_size)
    _cur=$(stat -c %s "$IMG_FILE" || echo "")
    if [ -n "$_fp" ] && [ "$_cur" = "$_fp" ]; then
        _rs_cert=1
        say "reparse precheck: certified clean by staging side (size=$_fp matches)"
    elif [ -n "$_fp" ]; then
        say "reparse precheck: recorded size $_fp != current ${_cur:-?} -> full scan"
    fi
fi
if [ "$_rs_cert" = "0" ]; then
    say "reparse precheck: scanning image metadata (may take a while) ..."
    ( timeout 180 "$WIMLIB" dir "$IMG_FILE" "$IMAGE_INDEX" --detailed; echo "DIRRC=$?" >&2 ) \
        2> /tmp/zj_rep.err | awk '
/^Attributes / { seen++ }
/^Reparse Tag / {
    t = $NF; sub(/^0x/, "", t);
    if (length(t) < 8 || (length(t) == 8 && t ~ /^[0-7]/)) {
        bad++; if (bad <= 5) printf "BADTAG 0x%s\n", t
    }
}
/^    FILE_ATTRIBUTE_ENCRYPTED is set/ { enc++ }
END { printf "RES SEEN=%d BAD=%d ENC=%d\n", seen, bad, enc }' > /tmp/zj_rep.res
    _rc=$(tail -n 1 /tmp/zj_rep.err | sed -n 's/^DIRRC=//p')
    _seen=$(sed -n 's/^RES SEEN=\([0-9]*\) .*/\1/p' /tmp/zj_rep.res)
    _bad=$(sed -n 's/^RES SEEN=[0-9]* BAD=\([0-9]*\) .*$/\1/p' /tmp/zj_rep.res)
    _enc=$(sed -n 's/^RES SEEN=[0-9]* BAD=[0-9]* ENC=\([0-9]*\)$/\1/p' /tmp/zj_rep.res)
    if [ -n "$_bad" ] && [ "$_bad" != "0" ]; then
        RESULT=FAILED; STEP=reparse-unsettable
        say "ERROR: image contains reparse point(s) the Linux rescue cannot write:"
        sed -n 's/^BADTAG /  bad tag /p' /tmp/zj_rep.res
        say "ERROR: use 'restore then reboot' (Windows-side extract) or in-place restore from PE"
        persist_log
        exit 1
    fi
    if [ -n "$_enc" ] && [ "$_enc" != "0" ]; then
        RESULT=FAILED; STEP=efs-encrypted
        say "ERROR: image contains EFS-encrypted content the Linux rescue cannot restore:"
        say "  encrypted entries: $_enc (they would be restored EMPTY)"
        say "ERROR: use 'restore then reboot' (Windows-side extract) or in-place restore from PE"
        persist_log
        exit 1
    fi
    if [ "$_rc" != "0" ] || [ -z "$_seen" ] || [ "$_seen" = "0" ]; then
        RESULT=FAILED; STEP=reparse-unsettable
        say "ERROR: reparse precheck could not complete (dir rc=${_rc:-?} entries=${_seen:-?})"
        say "  (probe out: $(tail -c 200 /tmp/zj_rep.err | tr '\n' ' '))"
        say "ERROR: cannot certify image is safe for Linux apply - aborted BEFORE touching the target"
        persist_log
        exit 1
    fi
    say "reparse precheck: ok (entries=$_seen, no unsettable reparse tags, no EFS)"
fi
fi # end engine != swap (image scan + precheck)

# ── A 引擎带子镜像：补一次镜像扫描（PIT-138，2026-10-12 现场日志定位）──
# 上面那段只在 `ENGINE != swap` 时跑，所以 swap 下 IMG_FILE 恒为 "" ——
# 即使下面 ESP/EA 段跑起来也拿不到镜像路径。这里给 A 引擎补上：
# 找得到 → 照常解子镜像；找不到 → **只 WARN 并跳过子镜像，不中断主 swap**
# （子镜像失败本就是非致命的，与 ESP 段的既有语义一致）。
if [ "$ENGINE" = "swap" ]; then
    if [ -n "$ESP_INDEX" ] || [ -n "$EA_INDEX" ]; then
        scan_image
        if [ -z "$IMG_FILE" ]; then
            say "image not found on first pass -> retry after 3s"
            sleep 3
            scan_image
        fi
        if [ -z "$IMG_FILE" ]; then
            say "WARN: image not found -> skip ESP/EA subimage restore (main swap unaffected)"
            ESP_INDEX=""
            EA_INDEX=""
        else
            STEP=image-found
            persist_log
        fi
    fi
fi

# ── ESP 子镜像（方案 C，用户 2026-09-30 规格 / 无忧 66 楼）──
# 主镜像里可能含名为 ESP 的子镜像（备份时勾选 --esp 并入，契约键 esp_index）。
# apply 完主系统后用 **$IMG_FILE + $ESP_INDEX** 直接从挂载中的镜像分区恢复到
# ESP 分区（所以镜像分区 umount 要挪到那之后，见下方 apply done 处）。
# 没有 esp_index 时整段不跑（绝大多数任务不带），完全不受影响。

# ── A 引擎：改名交换（engine=swap）──
# Windows 侧已把镜像解压到目标分区的 ~new\（含完成标记 .zj-done）；Linux 侧
# 只需调用 zjswap.sh：旧顶层项 → ~old\、~new 顶层项 → 原位。不 mkntfs、不
# wimlib apply。失败安全：~new 缺 .zj-done 或已有 ~old → 拒绝并保留旧系统。
# 交换完成后目标分区已有完整新系统文件，但 bootmgr/BCD 可能仍缺（万能镜像
# 常不带 \Boot）—— bootfix 段照常执行。
if [ "$ENGINE" = "swap" ]; then
    STEP=swapping
    say "A-engine swap: ~new -> root (no mkntfs, no apply)"
    ZJSWAP="$(command -v zjswap.sh 2>/dev/null)"
    [ -n "$ZJSWAP" ] || ZJSWAP=/zjswap.sh
    if [ ! -f "$ZJSWAP" ]; then
        RESULT=FAILED; STEP=swap-missing
        say "ERROR: zjswap.sh not found in initramfs"
        persist_log
        exit 1
    fi
    # zjswap 自己挂载目标（ntfs3 → ntfs-3g 兜底），不需要预先挂载
    _s0=$(date +%s)
    sh "$ZJSWAP" "$TARGET_DEV" /tmp/zj_swap_mnt
    swap_rc=$?
    _s1=$(date +%s)
    say "swap rc=$swap_rc elapsed=$((_s1 - _s0))s"
    if [ $swap_rc -ne 0 ]; then
        RESULT=FAILED; STEP=swap-failed
        case $swap_rc in
            2) say "~new missing or incomplete (.zj-done absent) - extract was not done or failed" ;;
            3) say "leftover ~old present - previous swap interrupted or user chose keep; handle on next Windows start" ;;
            *) say "zjswap.sh failed rc=$swap_rc" ;;
        esac
        say_dmesg
        persist_log
        exit 1
    fi
    say "swap done: ~new -> root, old -> ~old"
    STAGE=1
    # 未格式化 → PBR 原封不动（与 keep 模式同款处理：只在扇区 1 全 0 时才重写）
    KEPT_FS=1
    TARGET_FS=ntfs
else
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
TARGET_FS=$(timeout 30 blkid -s TYPE -o value "$TARGET_DEV" 2>/dev/null)
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
    if timeout 60 mount -t ntfs3 "$TARGET_DEV" /tmp/zj_wipe 2>/tmp/zjwipe.err; then
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
        if timeout 60 ntfs-3g -o force "$TARGET_DEV" /tmp/zj_wipe 2>>/tmp/zjwipe.err \
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
    [ "$START_LBA" -gt 0 ] 2>/dev/null || {
        START_LBA=2048
        say "WARN: START_LBA invalid (target_part_offset='${TARGET_OFFSET:-empty}') -> use 2048"
    }
    say "mkntfs $TARGET_DEV (start_lba=$START_LBA)"
    umount "$TARGET_DEV" 2>/dev/null
    timeout 30 dd if="$TARGET_DEV" of=/tmp/pbr_orig.bin bs=512 count=1 2>/dev/null
    timeout 300 mkntfs -f -S 63 -H 255 --partition-start "$START_LBA" "$TARGET_DEV" > /tmp/mkntfs.out 2>&1
    mkfs_rc=$?
    while IFS= read -r line; do say "[mkntfs] $line"; done < /tmp/mkntfs.out
    if [ $mkfs_rc -eq 124 ]; then
        say "WARN: mkntfs TIMEOUT after 300s (disk I/O hang?)"
        say_dmesg
    fi
    [ $mkfs_rc -eq 0 ] || { RESULT=FAILED; STEP=mkntfs-failed; say "ERROR: mkntfs rc=$mkfs_rc"; exit 1; }
    STAGE=3
    _b0=$((START_LBA & 0xFF)); _b1=$(((START_LBA >> 8) & 0xFF))
    _b2=$(((START_LBA >> 16) & 0xFF)); _b3=$(((START_LBA >> 24) & 0xFF))
    printf "\\$(printf '%03o' $_b0)\\$(printf '%03o' $_b1)\\$(printf '%03o' $_b2)\\$(printf '%03o' $_b3)" \
        | timeout 30 dd of="$TARGET_DEV" bs=1 seek=28 count=4 conv=notrunc 2>/dev/null
    _hid=$(timeout 30 dd if="$TARGET_DEV" bs=1 skip=28 count=4 2>/dev/null | od -An -tu4 | tr -d ' ')
    if [ "$_hid" = "$START_LBA" ]; then
        say "hidden sectors = $_hid (OK)"
    else
        say "ERROR: hidden=$_hid want=$START_LBA"
    fi
fi

# ── 应用镜像 ──
# 应用模式：
#   dir   = 先用内核 ntfs3 把目标分区挂成目录，再 apply 到目录（内核回写、
#           无 FUSE/逐文件 fsync，通常最快）。**隐患**：写目录时 Windows ACL
#           （安全描述符）不一定能完整恢复，故不做默认。
#   block = wimlib 直接写块设备（libntfs-3g 用户态模式）——ACL 正确，保持默认。
# 应用模式：block（产线；= wimlib NTFS 卷模式经 libntfs-3g 直写，能完整写回
# Windows ACL/属性/ADS/短名/创建时间/重解析点）。dir/dirfuse 仅为**实验室测试
# 钩子**（须由内核 cmdline `zjapply=` 显式指定）：目录写会系统性丢/篡改上述元数据
# （PIT-102 实测：ACL 变 Everyone 完全控制、junction 变 symlink/普通文件等），
# 故**不提供降级方案** —— block 失败即 fail-closed 报错（见下方 apply 失败分支）。
APPLY_MODE=${ZJ_APPLY_MODE:-block}
[ "$APPLY_MODE" = "dirfuse" ] && { APPLY_MODE="dir"; ZJ_APPLY_FS="fuse"; }
APPLY_FS=${ZJ_APPLY_FS:-auto}
STEP=applying
say "apply image -> $TARGET_DEV (index $IMAGE_INDEX, mode=$APPLY_MODE fs=$APPLY_FS)"
# wimlib 位置已在镜像扫描段之前**无条件**解析（PIT-138），此处只打印确认。
say "wimlib: $WIMLIB"
# 诊断：内存 / 镜像大小 / 目标容量（wimlib 卡住时先看这些）
say "mem: $(free 2>/dev/null | tr '\n' ' ')"
# 低内存预警（2026-10-04 实测）：192MB 虚拟机里 wimlib 会在 apply **最后阶段**
# （全部文件写完后设置安全描述符时）被 OOM 杀掉（rc=137）—— 等于整盘白写。
# 512MB 实测正常。这里提前把风险写进日志/屏幕，用户能直接看到原因。
_mem_kb=$(awk '/^MemTotal:/{print $2}' /proc/meminfo 2>/dev/null)
if [ -n "$_mem_kb" ] && [ "$_mem_kb" -lt 400000 ] 2>/dev/null; then
    say "WARN: low memory (${_mem_kb}kB RAM) - wimlib apply may be OOM-killed; increase VM RAM"
fi
say "img bytes: $(ls -l "$IMG_FILE" 2>/dev/null | awk '{print $5}')"
_target_sectors=$(cat "/sys/class/block/$(basename "$TARGET_DEV")/size" 2>/dev/null)
say "target sectors: $_target_sectors"
persist_log

# 目录模式挂载（ZJ_APPLY_FS: auto=ntfs3 优先失败退 FUSE；ntfs3/fuse=强制）
mount_apply_dir(){
    umount /tmp/zj_apply 2>/dev/null
    mkdir -p /tmp/zj_apply
    case "$APPLY_FS" in
        ntfs3)
            timeout 60 mount -t ntfs3 "$TARGET_DEV" /tmp/zj_apply 2>/tmp/zjapply.err && return 0
            ;;
        fuse)
            timeout 60 ntfs-3g -o remove_hiberfile "$TARGET_DEV" /tmp/zj_apply 2>>/tmp/zjapply.err && return 0
            timeout 60 ntfs-3g -o force "$TARGET_DEV" /tmp/zj_apply 2>>/tmp/zjapply.err && return 0
            timeout 60 mount -t ntfs-3g "$TARGET_DEV" /tmp/zj_apply 2>>/tmp/zjapply.err && return 0
            ;;
        *)
            mnt_dev "$TARGET_DEV" /tmp/zj_apply && return 0
            ;;
    esac
    return 1
}

# 应用目标：dir 模式先挂载（失败则退回块设备）
APPLY_TGT="$TARGET_DEV"
APPLY_MNT=""
if [ "$APPLY_MODE" = "dir" ]; then
    umount "$TARGET_DEV" 2>/dev/null
    if mount_apply_dir; then
        APPLY_TGT=/tmp/zj_apply
        APPLY_MNT=/tmp/zj_apply
        _mfs=$(awk -v m=/tmp/zj_apply '$2==m{print $3}' /proc/mounts 2>/dev/null)
        say "apply target: /tmp/zj_apply (dir write TEST override, fs=${_mfs:-unknown})"
    else
        say "dir mount failed (fs=$APPLY_FS; $(cat /tmp/zjapply.err 2>/dev/null | tr '\n' ' ')) -> block mode"
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
        if echo 1 > "$_p" 2>/dev/null; then
            say "nvme io_timeout: ${_old}s -> $(cat "$_p" 2>/dev/null)s ($_p)"
        else
            say "WARN: cannot set nvme io_timeout ($_p): 30s completion stalls may persist (PIT-055)"
        fi
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
    command -v ntfsfix >/dev/null 2>&1 && timeout 60 ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
fi
_a0=$(date +%s)
run_apply "$APPLY_TGT"
apply_exit=$(cat /tmp/apply.exit 2>/dev/null)
[ -n "$apply_exit" ] || apply_exit=1
if [ "$apply_exit" != "0" ]; then
    say "apply FAILED rc=$apply_exit $(rc_name "$apply_exit"); wimlib output head:"
    head -n 5 /tmp/apply.out 2>/dev/null | while IFS= read -r _l; do say "  |$_l"; done
    # wimlib 的 [ERROR] 行在输出**末尾**（进度行在前面），必须 tail
    say "apply FAILED wimlib output tail:"
    tail -n 20 /tmp/apply.out 2>/dev/null | while IFS= read -r _l; do say "  |$_l"; done
    if [ "$apply_exit" = "46" ]; then
        say "rc=46 (libntfs-3g) -> ntfsfix + retry once"
        command -v ntfsfix >/dev/null 2>&1 && timeout 60 ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
        umount "$TARGET_DEV" 2>/dev/null; umount "$APPLY_TGT" 2>/dev/null
        run_apply "$APPLY_TGT"
        apply_exit=$(cat /tmp/apply.exit 2>/dev/null)
        [ -n "$apply_exit" ] || apply_exit=1
        say "retry apply rc=$apply_exit"
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
# 镜像分区（$SRC_M）**先不卸载**：下面 ESP 子镜像（方案 C）还要直接用 $IMG_FILE
# apply，统一挪到 ESP 段之后再 umount。
if [ "$apply_exit" != "0" ]; then
    RESULT=FAILED; STEP=apply-failed
    say "ERROR: apply rc=$apply_exit $(rc_name "$apply_exit")"
    say "ERROR: no degraded fallback by design (dir write would lose Windows ACL/attributes/ADS/short names)"
    say "ERROR: target partition is already formatted; reboot to PE and use in-place restore to retry"
    say_dmesg
    persist_log
    exit 1
fi

STEP=apply-done
say "apply done"
STAGE=1
fi # end engine != swap (B-engine prepare+apply)

# ── B 引擎首启清理（PIT-170 边界）：Linux 侧不能写离线 RunOnce，
#    改为把 zj-restore-boot.exe 放进目标 ProgramData，并在“所有用户”启动
#    文件夹放一个 cmd；首次登录时自动删除临时引导项（会请求一次 UAC）。
#    A 引擎已在 Windows 侧注入 RunOnce，不走这里。
if [ "$ENGINE" = "block" ] && [ -f /zj-restore-boot.exe ]; then
    STEP=boot-cleanup
    BCM=/tmp/zj_boot_clean
    mkdir -p "$BCM"
    if timeout 60 mount -t ntfs3 "$TARGET_DEV" "$BCM" 2>/tmp/zjbc.err \
       || ntfs-3g -o force "$TARGET_DEV" "$BCM" 2>>/tmp/zjbc.err \
       || mnt_dev "$TARGET_DEV" "$BCM"; then
        mkdir -p "$BCM/ProgramData/SysRecover" \
                 "$BCM/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup"
        cp /zj-restore-boot.exe "$BCM/ProgramData/SysRecover/zj-restore-boot.exe" \
           2>/tmp/zjbc.err
        cat > "$BCM/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup/SysRecover-RestoreBoot.cmd" <<'ZJCM'
@echo off
if exist "%ProgramData%\SysRecover\boot-cleanup.done" goto done
"%ProgramData%\SysRecover\zj-restore-boot.exe"
if exist "%ProgramData%\SysRecover\boot-cleanup.done" del "%~f0"
goto :eof
:done
del "%~f0"
ZJCM
        sync
        if [ -s "$BCM/ProgramData/SysRecover/zj-restore-boot.exe" ] && \
           [ -s "$BCM/ProgramData/Microsoft/Windows/Start Menu/Programs/Startup/SysRecover-RestoreBoot.cmd" ]; then
            say "boot cleanup: deployed (A-engine-style helper via startup folder)"
        else
            say "WARN: boot cleanup deploy INCOMPLETE: $(cat /tmp/zjbc.err | tr '\n' ' ')"
        fi
        umount "$BCM" 2>>/tmp/zjbc.err
    else
        say "WARN: boot cleanup target mount failed: $(cat /tmp/zjbc.err | tr '\n' ' ')"
    fi
fi

# ── 引导补全：把 \bootmgr + \Boot\* 拷进目标分区（万能镜像常缺 \Boot\BCD）──
# 文件由 Windows 暂存阶段生成（bcdedit /export 得到可移植 BCD），/init 已拷到
# /tmp/bootfix。这里**用 cp 而不是 wimlib apply**：wimlib 的 NTFS 卷模式遇到
# 已存在文件会报 "File exists"（rc=46 WIMLIB_ERR_NTFS_3G），而镜像里一般已有
# \bootmgr —— cp 会正常覆盖。
if [ -f /tmp/bootfix/bootmgr ]; then
    STEP=bootfix
    say "install bootfix files (\\bootmgr + \\Boot)"
    command -v ntfsfix >/dev/null 2>&1 && timeout 60 ntfsfix -d "$TARGET_DEV" >> "$LOG" 2>&1
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
            STAGE=2
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
    if [ -d /sys/firmware/efi ]; then
        say "no bootfix needed (UEFI boot), skip boot files"
    else
        say "WARN: no bootfix files, skip boot files"
    fi
fi

# ── 修复引导 ──
# 只格式化分区（不动磁盘 MBR 引导代码），所以关键是恢复分区 PBR 的引导代码。
# 引导代码来源优先级：
#   1) 随包内置 /ntfs-boot-code.bin（微软 NTFS 引导代码 426B，0x54..0x1FD）
#   2) 格式化前备份的原 PBR —— 仅在原 PBR 是 NTFS 且不是 mkntfs 自带代码时可用
#      （否则会自我复制非引导代码，开机黑屏光标闪，PIT-050）
if [ "$REPAIR_BOOT" = "1" ]; then
    # 4Kn 检测（PIT-133）：逻辑扇区 >512 的盘，微软不支持 BIOS 引导（4Kn 要求
    # UEFI）。下面的 PBR 移植按 512 字节扇区布局，4Kn+MBR 组合必然无法 BIOS
    # 引导 —— 明确警告（GPT/UEFI 路径不碰 PBR，不受影响）。
    if [ "$PT_TYPE" != "gpt" ]; then
        _dname=$(basename "$TARGET_DEV" | sed 's/p\?[0-9]*$//')
        _lbs=$(cat "/sys/class/block/$_dname/queue/logical_block_size" 2>/dev/null)
        if [ -n "$_lbs" ] && [ "$_lbs" -gt 512 ] 2>/dev/null; then
            say "WARN: target is 4Kn (logical sector=$_lbs): Windows does not support BIOS boot from 4Kn; on a BIOS/MBR machine the restored system will not boot (4Kn requires UEFI/GPT)"
        fi
    fi
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
            if [ "$_g" = "fa33c08e" ]; then
                say "boot region rewritten (0x54=$_g)"
            else
                say "ERROR: boot region rewrite failed (0x54=$_g)"
            fi
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
            say "ERROR: boot region BAD (0x54=$_g1, sector1 empty=$([ -z "$(echo "$_g2" | tr -d 0)" ] && echo yes || echo no))"
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

# ── 恢复 ESP 子镜像到 ESP 分区（方案 C，用户 2026-09-30 规格 / 无忧 66 楼）──
# 只在契约 esp_index 非 0 时才跑；其余任务完全不受影响。
#   · 找设备：优先 GPT 类型 GUID（EFI System），回退 FAT 且根下有 \EFI 目录
#     （DiskGenius 重建可能把类型 GUID 标错，但固件只认路径）；
#   · 离线写（此时 Windows 不在跑）→ 没有 BCD 文件锁问题（对比 PIT-051）；
#   · wimlib **目录模式** apply = 覆盖同名 + 新增，**不删除**其它文件
#     → 我们常驻的 \EFI\ZJRESTORE 不受影响（也不会带回旧副本：Windows 侧
#       捕获时已排除，见 ops.cpp EnsureEspExclusionConfig）；
#   · 失败只报错**不中断**（bootfix 同款先例）：主系统已还原成功，ESP 没修好
#     至少系统文件都在，现场还能排错/重试。
find_esp_dev(){
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *loop*|*ram*) continue ;; esac
        [ "$_d" = "$TARGET_DEV" ] && continue
        # ESP 一定在内置盘：U 盘上恰有 ESP 类型的 FAT 不该被当成目标机 ESP
        is_removable "$(basename "$_d")" && continue
        _t=$(timeout 30 blkid -s PART_ENTRY_TYPE -o value "$_d" 2>/dev/null | tr 'A-Z' 'a-z')
        [ "$_t" = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b" ] && { echo "$_d"; return 0; }
    done
    for _d in $(list_parts); do
        [ -b "$_d" ] || continue
        case "$_d" in *loop*|*ram*) continue ;; esac
        [ "$_d" = "$TARGET_DEV" ] && continue
        is_removable "$(basename "$_d")" && continue
        case "$(timeout 30 blkid -s TYPE -o value "$_d" 2>/dev/null)" in vfat|fat|fat32) ;; *) continue ;; esac
        # 已挂着的（如被认领为兜底日志窝的 ESP）直接用现挂载点判定
        _mp=$(awk -v d="$_d" '$1==d{print $2; exit}' /proc/mounts 2>/dev/null)
        if [ -n "$_mp" ]; then
            [ -d "$_mp/EFI" ] && { echo "$_d"; return 0; }
            continue
        fi
        mkdir -p /tmp/zj_esp_probe
        if mnt_dev "$_d" /tmp/zj_esp_probe; then
            if [ -d /tmp/zj_esp_probe/EFI ]; then
                umount /tmp/zj_esp_probe 2>/dev/null
                echo "$_d"; return 0
            fi
            umount /tmp/zj_esp_probe 2>/dev/null
        fi
    done
    return 1
}
# PIT-195（2026-10-31）：Windows 侧暂存时会把我们的启动项**置顶**到 BootOrder，
# 且指向一份**独立临时内核** `vmlinuz-zjtemp.efi` —— 这样即使固件不认 BootNext
# （现场实测），重启也一定能进救援。代价是必须"只用一次"：救援层一跑起来就把
# 这份临时内核删掉，之后无论成功/失败/断电，固件再试这条启动项都会因文件不存在
# 而跳过 → 回落 Windows/新系统，**不会死循环**。
# 常驻菜单用的 `vmlinuz-zjrestore.efi` 不动。
# 注：这里不额外抑制 stderr —— 失败原因要留在控制台/日志里可追（PIT-147 口径）。
disable_staged_entry(){
    _de=$(find_esp_dev) || _de=
    [ -n "$_de" ] || return 0
    _dm=/tmp/zj_disarm
    mkdir -p "$_dm" || return 0
    if mnt_dev "$_de" "$_dm"; then
        if [ -f "$_dm/EFI/ZJRESTORE/vmlinuz-zjtemp.efi" ]; then
            if rm -f "$_dm/EFI/ZJRESTORE/vmlinuz-zjtemp.efi"; then
                sync
                say "staged boot entry disarmed (temp kernel removed)"
            else
                say "WARN: cannot remove temp kernel (read-only ESP?)"
            fi
        fi
        umount "$_dm"
    fi
    rmdir "$_dm"
    return 0
}
disable_staged_entry

if [ -n "$ESP_FIX_DIR" ]; then
    # 加密镜像（ZJENC1）：ESP 子镜像已由 Windows 解到目标盘的 $ESP_FIX_DIR，
    # 这里只做目录拷贝（救援层没有解密器，读不了 .zje）。
    STEP=esp
    ESP_DEV=$(find_esp_dev)
    if [ -n "$ESP_DEV" ]; then
        ESPM=/tmp/zj_esp_m
        mkdir -p "$ESPM"
        if mnt_dev "$ESP_DEV" "$ESPM"; then
            TGTM=/tmp/zj_tgt_fix
            mkdir -p "$TGTM"
            if mnt_dev "$TARGET_DEV" "$TGTM"; then
                SRC="$TGTM/$ESP_FIX_DIR"
                if [ -d "$SRC" ]; then
                    say "restore esp fix (copy) $ESP_FIX_DIR -> $ESP_DEV"
                    # FAT/ESP 不支持 Unix 权限，`cp -a` 在部分 busybox 上会因
                    # 保留属性失败 → 先 -a，失败退回 -r。
                    if cp -a "$SRC/." "$ESPM/" 2>/tmp/espfix.err || cp -r "$SRC/." "$ESPM/" 2>>/tmp/espfix.err; then
                        sync
                        say "esp restored (copy rc=0)"
                    else
                        say "ERROR: esp copy failed: $(tr '\n' ' ' < /tmp/espfix.err 2>/dev/null | head -c 200)"
                    fi
                else
                    say "WARN: esp fix dir not found: $SRC (skip)"
                fi
                umount "$TGTM" 2>/dev/null
            else
                say "ERROR: target mount failed for esp fix copy"
            fi
            umount "$ESPM" 2>/dev/null
        else
            say "ERROR: esp mount failed: $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ')"
        fi
    else
        say "WARN: task has esp fix dir but no ESP partition found (skip)"
    fi
elif [ -n "$ESP_INDEX" ]; then
    STEP=esp
    ESP_DEV=$(find_esp_dev)
    if [ -n "$ESP_DEV" ]; then
        ESPM=/tmp/zj_esp_m
        mkdir -p "$ESPM"
        if mnt_dev "$ESP_DEV" "$ESPM"; then
            say "restore esp subimage idx=$ESP_INDEX -> $ESP_DEV"
            "$WIMLIB" apply "$IMG_FILE" "$ESP_INDEX" "$ESPM" > /tmp/esp.out 2>&1
            esp_rc=$?
            sync
            if [ $esp_rc -eq 0 ]; then
                say "esp restored (rc=0)"
            else
                say "ERROR: esp restore rc=$esp_rc:"
                head -n 5 /tmp/esp.out 2>/dev/null | while IFS= read -r _l; do say "  |$_l"; done
            fi
            umount "$ESPM" 2>/dev/null
        else
            say "ERROR: esp mount failed: $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ')"
        fi
    else
        say "WARN: task has esp subimage (idx $ESP_INDEX) but no ESP partition found (skip)"
    fi
fi
# ── EA 修复投放（PIT-122，2026-10-06）──
# 镜像里可能含名为 ZJEA 的子镜像（备份时自动并入：带 EA 的文件打包 eapack.dat +
# 首启补写器 zj-ea-apply.exe + 同步策略片段 zj-regpol.bin；契约键 ea_index）。
# 背景：Linux 侧 wimlib apply 丢 Windows EA（PIT-120 客户实测 804 个文件）。
# 这里把修复载荷投放到目标系统，并安装**本地组策略启动脚本钩子** —— 目标系统
# 首次开机时（登录前、SYSTEM）自动用 NtSetEaFile 写回 EA，然后自清理。
# 机理（CSE 声明 / gpt.ini 版本递增 / RunStartupScriptSync+SyncForegroundPolicy
# 同步策略 / 预登录执行 / 每引导重跑 / 文件可写性）已 QEMU 全链实测（PIT-123）。
# 没有 ea_index 时整段不跑（绝大多数任务不带）。
# （EA_INDEX 在脚本开头与 ESP_INDEX 一起解析，见契约解析段。）
if [ -n "$EA_FIX_DIR" ] || [ -n "$EA_INDEX" ]; then
    STEP=ea
    EAD=/tmp/zj_ea
    mkdir -p "$EAD"
    # 加密镜像：ZJEA 子镜像已由 Windows 解到目标盘，拷过来即可（不读密文镜像）；
    # 旧行为：从镜像子镜像 apply 出来。
    if [ -n "$EA_FIX_DIR" ]; then
        say "ea fix: copy $EA_FIX_DIR from target (encrypted image)"
        EAFTM=/tmp/zj_ea_src
        mkdir -p "$EAFTM"
        if mnt_dev "$TARGET_DEV" "$EAFTM"; then
            cp -a "$EAFTM/$EA_FIX_DIR/." "$EAD/" 2>/tmp/eafix.err
            _earc=$?
            umount "$EAFTM" 2>/dev/null
        else
            _earc=1
            cp /tmp/zjmnt.err /tmp/eafix.err 2>/dev/null
        fi
    else
        say "ea subimage: index=$EA_INDEX (deploy first-boot EA restore)"
        "$WIMLIB" apply "$IMG_FILE" "$EA_INDEX" "$EAD" > /tmp/ea.out 2>&1
        _earc=$?
    fi
    if [ "$_earc" -eq 0 ]; then
        # PIT-132：载荷 = 补写器（必需）+ eapack.dat（可选：无 EA 的
        # "悬空引用清理"任务只有补写器）。
        if [ -s "$EAD/zj-ea-apply.exe" ]; then
            _pkb="none"
            [ -s "$EAD/eapack.dat" ] && _pkb="$(wc -c < "$EAD/eapack.dat" | tr -d ' ')B"
            say "ea payload extracted: pack=$_pkb applier=yes"
            EAM=/tmp/zj_ea_m
            mkdir -p "$EAM"
            if mnt_dev "$TARGET_DEV" "$EAM"; then
                mkdir -p "$EAM/ZJRESTORE/ea" \
                         "$EAM/Windows/System32/GroupPolicy/Machine/Scripts/Startup"
                # 来源标记（PIT-156，2026-10-18）：<目标>\ZJRESTORE\source.txt —— 给
                # 首启补写器（归纳日志回软件目录）与 Windows 侧收集器/清理器看的
                # "这份残留是谁放的、软件在哪"。mkdir 成功后立刻写：EA 部署即使
                # 后续某步失败，标记照样在（失败场景同样要能被归纳/清理）。
                {
                    printf 'time=%s\n' "$(date '+%F %T')"
                    printf 'engine=%s\n' "$ENGINE"
                    printf 'action=%s\n' "${ACTION:-restore}"
                    printf 'applier=1\n'
                    printf 'software_dir=%s\n' "$SOFTWARE_DIR"
                    printf 'software_path=%s\n' "$(get_task software_path)"
                    printf 'software_version=%s\n' "$(get_task software_version)"
                    printf 'rescue_build=%s\n' "$(cat /zjrescue-version 2>/dev/null)"
                    printf 'target_dev=%s\n' "$TARGET_DEV"
                    printf 'target_offset=%s\n' "$TARGET_OFFSET"
                    printf 'target_size=%s\n' "$TARGET_SIZE"
                    printf 'image_path=%s\n' "$IMAGE_PATH"
                } > "$EAM/ZJRESTORE/source.txt" \
                    || say "WARN: source.txt write failed (residue marker missing)"
                [ -s "$EAD/eapack.dat" ] && cp "$EAD/eapack.dat" "$EAM/ZJRESTORE/ea/" 2>/dev/null
                [ -s "$EAD/reppack.dat" ] && cp "$EAD/reppack.dat" "$EAM/ZJRESTORE/ea/" 2>/dev/null
                cp "$EAD/zj-ea-apply.exe" "$EAM/ZJRESTORE/ea/" 2>/dev/null
                # 启动脚本：补写器不在（已修复/已放弃/删除）→ 秒退；有包带 --pack，
                # 无包（纯引用清理）直接跑。
                printf '@echo off\r\nif not exist "%%SystemDrive%%\\ZJRESTORE\\ea\\zj-ea-apply.exe" goto :eof\r\nif exist "%%SystemDrive%%\\ZJRESTORE\\ea\\eapack.dat" goto :withpack\r\n"%%SystemDrive%%\\ZJRESTORE\\ea\\zj-ea-apply.exe"\r\ngoto :eof\r\n:withpack\r\n"%%SystemDrive%%\\ZJRESTORE\\ea\\zj-ea-apply.exe" --pack "%%SystemDrive%%\\ZJRESTORE\\ea\\eapack.dat"\r\n' \
                    > "$EAM/Windows/System32/GroupPolicy/Machine/Scripts/Startup/zj-ea-restore.cmd"
                # scripts.ini 合并：[Startup] 段追加我们的条目（保留用户已有条目）。
                # 统一 CRLF→LF 处理再转回，避免 \r 混进插入行（busybox awk 对 \r
                # 转义支持不一的坑）。
                SINI="$EAM/Windows/System32/GroupPolicy/Machine/Scripts/scripts.ini"
                if [ -s "$SINI" ]; then
                    tr -d '\r' < "$SINI" > /tmp/zj_ea_ini0.txt
                    MAXIDX=$(grep -oE '^[0-9]+' /tmp/zj_ea_ini0.txt 2>/dev/null | sort -n | tail -n 1)
                    if [ -n "$MAXIDX" ]; then IDX=$((MAXIDX + 1)); else IDX=0; fi
                    if grep -qi '^\[Startup' /tmp/zj_ea_ini0.txt; then
                        awk -v idx="$IDX" '
                            BEGIN { insec=0; done=0 }
                            /^\[Startup/ { insec=1; print; next }
                            /^\[/ { if (insec && !done) { printf "%scmdLine=zj-ea-restore.cmd\n%sParameters=\n", idx, idx; done=1 } insec=0; print; next }
                            { print }
                            END { if (insec && !done) { printf "%scmdLine=zj-ea-restore.cmd\n%sParameters=\n", idx, idx } }
                        ' /tmp/zj_ea_ini0.txt > /tmp/zj_ea_ini1.txt
                    else
                        cp /tmp/zj_ea_ini0.txt /tmp/zj_ea_ini1.txt
                        printf '[Startup]\n%scmdLine=zj-ea-restore.cmd\n%sParameters=\n' "$IDX" "$IDX" >> /tmp/zj_ea_ini1.txt
                    fi
                    awk '{printf "%s\r\n", $0}' /tmp/zj_ea_ini1.txt > "$SINI"
                    say "ea: scripts.ini merged (idx=$IDX)"
                else
                    printf '[Startup]\r\n0cmdLine=zj-ea-restore.cmd\r\n0Parameters=\r\n' > "$SINI"
                    say "ea: scripts.ini created"
                fi
                # gpt.ini 合并：声明 Scripts/Registry 两个 CSE + 版本号递增 ——
                # 版本必须与注册表 State\...\GPO-List\0\Version 不同，否则 gpsvc
                # 判"无变化"跳过处理（PIT-123 实测踩过，初装写 65537 被跳）。
                # ⚠️ PIT-131：比较只看**低 16 位**（用户版本字）——镜像残留 State
                # 常见 0x00010001（低字 1），若 gpt.ini 也写 0x7FFF0001（低字 1）
                # 即被当作"无变化"，Scripts CSE 永不执行（EA 修复静默失效）。
                # 因此新版本用**时间基低字**（0x7FFF0000 + 秒数%32768），保证与
                # 任何残留/上次部署的版本不同。
                GINI="$EAM/Windows/System32/GroupPolicy/gpt.ini"
                SCRPAIR='{42B5FAAE-6536-11D2-AE5A-0000F87571E3}{40B6664F-4972-11D1-A7CA-0000F87571E3}'
                REGPAIR='{35378EAC-683F-11D2-A89A-00C04FBBCFA2}{D02B1F72-3407-48AE-BA88-E8213C6761F1}'
                if [ -s "$GINI" ]; then
                    tr -d '\r' < "$GINI" > /tmp/zj_ea_gpt0.txt
                    VER=$(grep -o '^Version=[0-9]*' /tmp/zj_ea_gpt0.txt 2>/dev/null | head -1 | cut -d= -f2)
                    if [ -n "$VER" ]; then NVER=$((VER + 1)); else NVER=$((2147418112 + ($(date +%s) % 32768))); fi
                    HASSCR=0; HASREG=0
                    grep -qF "$SCRPAIR" /tmp/zj_ea_gpt0.txt 2>/dev/null && HASSCR=1
                    grep -qF "$REGPAIR" /tmp/zj_ea_gpt0.txt 2>/dev/null && HASREG=1
                    ADD=""
                    [ "$HASSCR" = "0" ] && ADD="${ADD}[$SCRPAIR]"
                    [ "$HASREG" = "0" ] && ADD="${ADD}[$REGPAIR]"
                    if grep -q '^gPCMachineExtensionNames=' /tmp/zj_ea_gpt0.txt; then
                        if [ -n "$ADD" ]; then
                            sed "s#^\(gPCMachineExtensionNames=.*\)\$#\1$ADD#" /tmp/zj_ea_gpt0.txt > /tmp/zj_ea_gpt1.txt
                        else
                            cp /tmp/zj_ea_gpt0.txt /tmp/zj_ea_gpt1.txt
                        fi
                    else
                        awk -v add="gPCMachineExtensionNames=[$SCRPAIR][$REGPAIR]" '
                            BEGIN { done=0 }
                            /^\[General/ { print; print add; done=1; next }
                            { print }
                            END { if (!done) { print "[General]"; print add } }
                        ' /tmp/zj_ea_gpt0.txt > /tmp/zj_ea_gpt1.txt
                    fi
                    if grep -q '^Version=' /tmp/zj_ea_gpt1.txt; then
                        sed "s/^Version=[0-9]*\$/Version=$NVER/" /tmp/zj_ea_gpt1.txt > /tmp/zj_ea_gpt2.txt
                    else
                        cp /tmp/zj_ea_gpt1.txt /tmp/zj_ea_gpt2.txt
                        printf 'Version=%s\n' "$NVER" >> /tmp/zj_ea_gpt2.txt
                    fi
                    awk '{printf "%s\r\n", $0}' /tmp/zj_ea_gpt2.txt > "$GINI"
                    say "ea: gpt.ini merged (version=$NVER scripts=$HASSCR registry=$HASREG)"
                else
                    NVER=$((2147418112 + ($(date +%s) % 32768)))
                    printf '[General]\r\ngPCMachineExtensionNames=[%s][%s]\r\nVersion=%s\r\n' \
                        "$SCRPAIR" "$REGPAIR" "$NVER" > "$GINI"
                    say "ea: gpt.ini created (version=$NVER)"
                fi
                # Registry.pol：追加同步策略记录（已有文件剥掉片段头再 cat 追加；
                # 用户已有记录保留 —— pol 记录可安全串接，重复记录后者生效）。
                POL="$EAM/Windows/System32/GroupPolicy/Machine/Registry.pol"
                FRAG="$EAD/zj-regpol.bin"
                if [ -s "$FRAG" ]; then
                    if [ -s "$POL" ]; then
                        tail -c +9 "$FRAG" > /tmp/zj_ea_frag.bin
                        cat "$POL" /tmp/zj_ea_frag.bin > /tmp/zj_ea_pol.bin
                    else
                        cp "$FRAG" /tmp/zj_ea_pol.bin
                    fi
                    cp /tmp/zj_ea_pol.bin "$POL"
                    say "ea: Registry.pol merged"
                else
                    say "WARN: ea regpol fragment missing (sync policy not set)"
                fi
                sync
                # 落盘复核（失败要在日志里显形，不静默）
                _eaok=1
                [ -s "$EAM/ZJRESTORE/ea/zj-ea-apply.exe" ] || _eaok=0
                [ -s "$EAM/Windows/System32/GroupPolicy/Machine/Scripts/Startup/zj-ea-restore.cmd" ] || _eaok=0
                [ -s "$EAM/Windows/System32/GroupPolicy/Machine/Scripts/scripts.ini" ] || _eaok=0
                [ -s "$EAM/Windows/System32/GroupPolicy/gpt.ini" ] || _eaok=0
                [ -s "$EAM/Windows/System32/GroupPolicy/Machine/Registry.pol" ] || _eaok=0
                if [ "$_eaok" = "1" ]; then
                    say "ea: deployed (payload + GPO hook; applies on first boot)"
                else
                    say "WARN: ea deploy INCOMPLETE (some files missing; EA fix may not run)"
                fi
                umount "$EAM" 2>/dev/null
            else
                say "ERROR: ea mount target failed: $(cat /tmp/zjmnt.err 2>/dev/null | tr '\n' ' ')"
            fi
        else
            say "ERROR: ea payload incomplete in subimage (files: $(ls "$EAD" 2>/dev/null | tr '\n' ' '))"
        fi
    else
        say "ERROR: ea extract failed rc=$_earc: $(tail -n 3 /tmp/ea.out 2>/dev/null | tr '\n' ' ') $(tr '\n' ' ' < /tmp/eafix.err 2>/dev/null | head -c 160)"
    fi
fi
# 镜像分区到此才卸载（ESP/EA 子镜像 apply 直接读 $IMG_FILE，见上面的注释）
umount "$SRC_M" 2>/dev/null

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
        case "$_d" in *sr*|*loop*|*cdrom*|*fd*|*floppy*) continue ;; esac
        is_mounted "$_d" && continue
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
            command -v ntfsfix >/dev/null 2>&1 && timeout 60 ntfsfix -d "$_d" >> "$LOG" 2>&1
        fi
    done
}

# 成功：结果置 OK；日志 + 状态回执落软件目录与 ESP（**不写目标盘** —— 还原后
# 的系统盘不留我们的文件；ESP 的 \EFI\ZJRESTORE\logs\ 是常驻救援模块的一部分）。
RESULT=OK; STEP="done"
persist_log          # 把完整日志落到 <软件目录>\logs\（cleanup 会保留该子目录）
# 成功回执也必须落**软件目录**（BIOS 机器没有 ESP；UEFI 的 ESP 写入也可能失败）：
# 否则 Windows 侧看不到"已成功"，会把待执行标记误报成"任务未执行"（0.6.29 规则）。
_rcpt=0
if [ -n "$SOFT_MNT" ] && [ -d "$SOFT_MNT/$SD_REL" ]; then
    mkdir -p "$SOFT_MNT/$SD_REL/logs" 2>/dev/null
    bb_status_text > "$SOFT_MNT/$SD_REL/logs/ZJRESTORE-status.txt" 2>/dev/null && _rcpt=1
fi
if [ -n "$BB_HOME" ]; then
    bb_status_text > "$BB_HOME/ZJRESTORE-status.txt" 2>/dev/null && _rcpt=1
fi
# 两个直写点全失败 = 哪都没有"成功回执"（若连 ESP 也没落上，下次 Windows 启动
# 会把 pending 标记误报成"任务未执行"）—— 响亮记一笔，别静默（PIT-147）
[ "$_rcpt" = 1 ] || say "WARN: status receipt NOT written (soft-dir & bb-home both failed)"
bb_esp_drop
sync
stop_log_mirror      # 停掉后台日志镜像循环
trap - EXIT          # 清理阶段不要再触发 persist_log 把日志写回去
cleanup_after_success
unlock_cleanup
sync
reboot -f 2>/dev/null || reboot
