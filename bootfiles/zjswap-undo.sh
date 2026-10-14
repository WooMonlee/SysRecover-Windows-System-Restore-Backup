#!/bin/sh
# zjswap-undo.sh — A 引擎「返回旧系统」反向交换（PIT-139，2026-10-13）。
#
# 用法: zjswap-undo.sh <目标分区设备> <挂载点>
# 前置（Windows 侧保证）：
#   · 目标根下有完整旧系统  ~/.old/Windows
#   · 目标根下是完整新系统  /Windows（本次要丢弃的）
#   · 目标根下**没有** ~new（上次交换的暂存目录；残留交给 Windows 启动期清理）
#
# 为什么在 Linux 做而不是 Windows（用户 2026-10-13 定案，方案 C）：
#   反向交换要把**正在运行的系统树**从根目录挪走 —— Windows 里做这件事，
#   explorer/杀软/驱动在 rename 窗口内按路径打开 C:\Windows\... 会立刻失败，
#   机器不可用且无法回滚；救援层里没有任何进程占用，纯同卷改名，秒级、可中断续做。
#
# 失败安全：
#   · 任何前置检查不过 → **报错退出、绝不碰盘**（exit 2/3，根与 ~/.old 都原样）；
#   · 交换中途断电 → 根空但 ~/.old 完好；下次进救援层会**自动续做**（见下
#     「续做」三条），BIOS 下引导命脉留在根（见 L5 注释），仍能进救援层。
#   · 不做 mkntfs、不 apply —— 全程只有 rename。
#
# ── 续做（resume）三条，缺一不可（PIT-139 T3 实测踩坑，2026-10-13）──
#   ① 阶段标记 `.zj-undo-phase` 内容 phase=1（L5 前）/ phase=2（L5 后）：
#      续做时 phase>=2 就**跳过 L5** —— 否则 L5 会把 L6 已经搬回根的旧文件
#      再搬进 ~/.new，L7 一删就是**静默丢数据**（第一版就是这个洞）。
#   ② L3 不能一见 ~/.new 就 exit 3：自家暂存目录（带 .zj-undo-phase）要放行，
#      否则续做永远进不来（第一版第二个洞）。
#   ③ L5 **不搬契约文件**（_zjresy*.log / restore-task.conf|json），留在根：
#      契约被搬进 ~/.new 后，中断时根上什么都没有 → init 找不到任务日志
#      （init 只在找到 log 或 /tmp/restore-task.conf 时才跑救援脚本）→
#      安全网也无从触发 → 用户再也进不去。留根则下次进救援层按 action=
#      returnold 直接续做；成功后由 L7 清掉（见「契约清理」）。
set -u

T="${1:?target dev}"; M="${2:?mount point}"
# 诊断盲点（PIT-141，2026-10-13 用户实测）：原来 say 只写 stderr，控制台一滚
# 就没了，重启后 /tmp 也清空 → 反向交换失败时**日志里一行 SWAPUNDO 都没有**
# （用户那次 rc=1 就查不出是哪一步）。单源实现收进 /zjlog.sh（P0-B/PIT-145）：
# 控制台 SWAPUNDO: 前缀（断言契约）不变，文件行带前缀时间戳追加到 $LOG
# （由调用方 export，见 zjrestore-lite.sh 顶部；未导出时兜底路径相同）。
ZJPREFIX=SWAPUNDO
. /zjlog.sh
# rm/mv 的错误以前被 2>/dev/null 吞掉（只剩 "failed"，没有为什么）——现在取回原因。
runerr(){ _r=$("$@" 2>&1); _rc=$?; [ $_rc -ne 0 ] && say "  cmd err(rc=$_rc): $(echo "$_r" | head -c 300)"; return $_rc; }

# 引导命脉（GRUB4DOS/BIOS 链）：L5 不搬走、L6 不搬回 —— 始终留在根。
# 搬走它们 = 中断后 BIOS 找不到 \grldr.mbr → 再也进不了救援层续做。
# （这三个文件由 Windows 侧 StageReturnOld 无条件重新部署，留在根的就是本版。）
is_bootload(){ case "$1" in grldr|grldr.mbr|menu.lst) return 0;; esac; return 1; }

# 契约文件：留在根（见上「续做③」），成功后才清。
is_contract(){ case "$1" in _zjresy*.log|restore-task.conf|restore-task.json) return 0;; esac; return 1; }

# 系统会自建/自改的瞬时项：不搬迁，直接删（同 zjswap.sh 的特殊项处理）。
is_transient(){ case "$1" in
    '$RECYCLE.BIN'|'System Volume Information'|pagefile.sys|swapfile.sys|hiberfil.sys)
        return 0;; esac; return 1; }

# 诊断资产（PIT-142）：L5 不搬走，留在根 —— 这是 returnold 之后用户唯一能
# 找回日志的地方。真机上软件目录找不到时，日志窝 ZJRESTORE-logs/（PIT-111/
# 137）就落在目标盘根，且 log mirror 还在每 2s 往里写；一旦被搬进 ~/.new：
#   · L7 删 ~/.new 时边删边长 → rm -rf 永远删不干净 → L8 后置校验 exit 1
#     → RESULT: restore FAILED（客户实测，PIT-142 根因）。
# 用户 2026-10-13 规格：日志优先写原软件目录；找不到时用 ~new 之外的兜底
# 目录/临时目录，绝不写进会被删的 ~new —— 这里的"留在根"就是那个兜底。
# （检测线仍在：万一 is_diag 漏了某个名字，L5 后会 WARN 一行，见下方自检。）
is_diag(){ case "$1" in
    ZJRESTORE-logs|ZJRESTORE-*.log|ZJRESTORE-*.txt|ZJRESTORE-*.out|zjrestore-boot.log)
        return 0;; esac; return 1; }

mkdir -p "$M"
# 挂载顺序：ntfs-3g(FUSE) 优先、ntfs3 内核驱动兜底 —— PIT-144（2026-10-14
# 压测实测）：ntfs3 在 L5 的 5 万次 rename 风暴后会留"幽灵索引项"（readdir
# 看不到条目但 rmdir 报 Directory not empty，重挂载也清不掉）→ rm -rf ~new
# 永远失败 → L8 后置校验必失败。本脚本只做 rename + 递归删除，FUSE 的
# 吞吐完全够用（顶层 rename 不随子树规模放大）；正确性优先。
# 实现收敛到 /zjlog.sh::mnt_ntfs（P0-B/PIT-145）：同一顺序 + 每步 timeout 60
# + 错误落 /tmp/zjmnt.err（PIT-112 挂死纪律，不再被 2>/dev/null 静默吞掉）。
mnt_undo(){ mnt_ntfs "$T" "$M" fuse; }
mnt_undo || { say "mount FAILED ($T): $(head -1 /tmp/zjmnt.err 2>/dev/null)"; exit 1; }

# ── L2：旧系统必须完整可用（fail-closed，绝不碰盘）──
if [ ! -d "$M/~old" ]; then
    say "ERROR: ~old absent -> nothing to return; target untouched"
    umount "$M" 2>/dev/null
    exit 2
fi
if [ ! -d "$M/~old/Windows" ]; then
    say "ERROR: ~old has no Windows -> not a usable old system; target untouched"
    umount "$M" 2>/dev/null
    exit 2
fi

# ── L3：~new 残留 —— 自家暂存目录（带阶段标记）放行续做，其它一律交给
#      Windows 启动期的残留处理，这里绝不并存操作 ──
PHASE=0
if [ -e "$M/~new" ]; then
    if [ -f "$M/~new/.zj-undo-phase" ]; then
        PHASE=$(sed -n 's/^phase=//p' "$M/~new/.zj-undo-phase" 2>/dev/null | tr -d '\r\n')
        [ -n "$PHASE" ] || PHASE=1
        case "$PHASE" in ''|*[!0-9]*) PHASE=1;; esac
        say "resume: ~/.new/.zj-undo-phase phase=$PHASE"
    else
        say "ERROR: leftover ~new present (no phase marker) -> handle it on Windows start first (exit 3)"
        umount "$M" 2>/dev/null
        exit 3
    fi
fi

# ── L4：建暂存目录 + 写阶段标记（中断后据此自动续做）──
if ! mkdir -p "$M/~new"; then
    say "ERROR: mkdir ~new FAILED; target untouched"
    umount "$M" 2>/dev/null
    exit 1
fi
if [ "$PHASE" -lt 2 ]; then
    # phase 标记写失败必须响亮（resume 靠它跳过 L5；静默失败 = 续做可能重搬已还原
    # 的旧文件再删掉 → 丢数据）—— WARN 不中断（盘可写时 rename 也会同步失败）
    printf 'phase=1\n' > "$M/~new/.zj-undo-phase" 2>/dev/null \
        || say "ERROR: phase=1 marker write failed (resume may redo completed steps)"
    say "marker: ~/.new/.zj-undo-phase phase=1"
fi

# ── L5：根下（除 ~/.old ~/.new 引导命脉 契约文件）全部改名进 ~/.new ──
if [ "$PHASE" -lt 2 ]; then
    for d in "$M"/* "$M"/.[!.]*; do
        [ -e "$d" ] || continue
        b=$(basename "$d")
        case "$b" in ~new|~old|.|..) continue;; esac
        is_bootload "$b" && continue
        is_contract "$b" && continue
        # 诊断资产必须在 is_transient 之前判定：误入瞬时分支会被 rm 掉（PIT-142）。
        is_diag "$b" && continue
        if is_transient "$b"; then
            runerr rm -rf "$d" || say "WARN: rm transient $b failed"
            continue
        fi
        runerr mv "$d" "$M/~new/" || say "WARN: move root $b -> ~/.new failed"
    done
    printf 'phase=2\n' > "$M/~new/.zj-undo-phase" 2>/dev/null \
        || say "ERROR: phase=2 marker write failed (resume may redo completed steps)"
    say "phase=2 (root moved into ~/.new)"
fi

# ── L5 后自检：诊断资产不该被搬走 ────────────────────────────────
# 真机上软件目录找不到时，日志窝 ZJRESTORE-logs/ 就落在目标盘根（PIT-111/137），
# 且 log mirror 每 2s 往里写。它若被 L5 当"待丢弃的新系统"搬进 ~/.new：
#   * L7 删 ~/.new 时连诊断日志一起销毁（成功路径上反而没了证据）
#   * 真机更糟 —— mirror 还在往 ~/.new/ZJRESTORE-logs 写，rm -rf 边删边长
#     → 删不干净 → L8 后置校验 exit 1 → RESULT: restore FAILED (PIT-142)
# L5 已把根下的条目改名进 ~/.new（本页上方 line 113），所以被搬走的诊断资产
# 现在在 ~/.new/ 下面，不在根。查错路径（$M/ZJRESTORE-logs）会让这个 WARN 永不
# 触发 —— 改为查 ~/.new/ 下面（PIT-142：真机上镜像还在往 ~/.new/ZJRESTORE-logs
# 写，L7 rm -rf 边删边长 → 删不干净 → L8 后置校验 exit 1）。
for _diag in "$M/~new/ZJRESTORE-logs" "$M/~new/ZJRESTORE-status.txt"; do
    [ -e "$_diag" ] && say "WARN: diagnostic asset moved into ~/.new: $_diag (should stay at root)"
done

# ── L6：~/.old/* 改名回根（瞬时项删旧，引导命脉留在 ~/.old 由 L7 清掉）──
for d in "$M/~old"/* "$M/~old"/.[!.]*; do
    [ -e "$d" ] || continue
    b=$(basename "$d")
    case "$b" in .|..) continue;; esac
    if is_transient "$b"; then
        runerr rm -rf "$d" || say "WARN: rm old transient $b failed"
        continue
    fi
    is_bootload "$b" && continue
    if [ -e "$M/$b" ]; then
        runerr rm -rf "$M/$b" || say "WARN: clear conflict $b failed"
    fi
    runerr mv "$d" "$M/$b" || say "WARN: move old $b -> root failed"
done

# 撤销备返：.zj-keepold 会随搬迁回到根 —— 用户已选择"返回旧系统"，语义结束，
# 立即删掉它，否则下次启动扫描又会把它当备返残留再问一遍。
if [ -e "$M/.zj-keepold" ]; then
    rm -f "$M/.zj-keepold" 2>/dev/null || say "WARN: remove .zj-keepold failed"
fi

# ── L7：清掉丢弃的新系统、~/.old，以及（续做③留在根的）契约文件 ──
# 契约清理：返回旧系统已成功，`action=returnold` 的任务不该再被执行一次；
# 留着它，下次误入救援层会撞 L2（~old 已删 → exit 2，无害但多一条误导日志）。
# ⚠️ 大批量删除两个坑都要防（PIT-144 压测实测，0.7.36 起）：
#    ① ntfs-3g 删除懒刷写：首轮 rm -rf 收尾 rmdir 常报 "Directory not empty"
#       （非真有并发写入者）→ sync + 重试；
#    ② ntfs3 幽灵索引项（本脚本现以 FUSE 为主，此处兜底）：readdir 看不到
#       条目但 rmdir 仍 ENOTEMPTY → 第 2 轮起重挂载重建缓存再删；仍失败
#       逐条抓 errno、最后抓 rmdir 原文取证。
rm_rf_retry(){
    _n=0
    while [ $_n -lt 4 ]; do
        rm -rf "$1" 2>/dev/null
        [ ! -e "$1" ] && return 0
        _n=$((_n + 1))
        if [ $_n -eq 2 ]; then
            sync
            umount "$M" 2>/tmp/zj-rmerr || say "WARN: umount for retry: $(head -1 /tmp/zj-rmerr 2>/dev/null)"
            if ! mnt_undo; then
                say "WARN: remount for rm retry failed"; return 1
            fi
            say "rm $(basename "$1") remounted for retry"
        fi
        say "rm $(basename "$1") retry $_n (sync+2s)"
        sync; sleep 2
    done
    for _e in "$1"/*; do
        [ -e "$_e" ] || continue
        rm -rf "$_e" 2>/tmp/zj-rmerr && continue
        say "  stuck: $(basename "$_e") err=$(head -1 /tmp/zj-rmerr 2>/dev/null)"
    done
    if rmdir "$1" 2>/tmp/zj-rmerr; then return 0; fi
    say "  rmdir $(basename "$1"): $(head -1 /tmp/zj-rmerr 2>/dev/null)"
    say "  ls -a count=$(ls -a "$1" 2>/dev/null | wc -l)"
    return 1
}
rm_rf_retry "$M/~new" || say "WARN: rm ~/.new failed after retries (leftover below)"
if [ -e "$M/~new" ]; then
    say "WARN: ~/.new still present after rm (L8 will fail) # the 2026-10-13 case"
    say "  leftover: $(ls "$M/~new" 2>/dev/null | head -8 | tr '\n' ' ')"
fi
rm_rf_retry "$M/~old" || say "WARN: rm ~/.old failed"
[ -e "$M/~old" ] && say "WARN: ~/.old still present after rm"
for d in "$M"/_zjresy*.log "$M"/restore-task.conf "$M"/restore-task.json; do
    [ -e "$d" ] || continue
    rm -f "$d" 2>/dev/null || say "WARN: rm contract $(basename "$d") failed"
done

# ── L8：成功后置校验（fail-loud，真看盘不自说自话）──
# 演练盘（QEMU）与真机都靠这一行判断"旧系统是不是真的回到了根"：
# 只有盘上确实有 /Windows 且 ~/.new ~/.old 都已消失才报 ok，否则非零退出。
_bad=""
[ -d "$M/Windows" ] || _bad="${_bad}Windows-missing "
[ -e "$M/~new" ]    && _bad="${_bad}~new-still-present "
[ -e "$M/~old" ]    && _bad="${_bad}~old-still-present "
if [ -n "$_bad" ]; then
    say "ERROR: post-check failed: $_bad (old tree may be incomplete)"
    sync
    umount "$M" 2>/dev/null
    exit 1
fi
say "post-check ok: old system at root, ~/.new and ~/.old gone"

sync
umount "$M" 2>/dev/null
say "return-old done (old tree at root; new tree discarded)"
exit 0
