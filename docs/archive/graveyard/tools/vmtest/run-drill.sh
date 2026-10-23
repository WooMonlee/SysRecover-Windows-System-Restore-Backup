#!/usr/bin/env bash
# run-drill.sh — 还原演练一次跑（TCG + -cpu max + -no-reboot）
# 用法: ./run-drill.sh [秒数，默认 280]
set -e
cd "$(dirname "$0")"
SECS=${1:-280}
TS=$(date +%Y%m%d-%H%M%S)
LOG=logs/drill-run-$TS.log
mkdir -p logs

QEMU=/d/Prog/ProgIDE/msys64/mingw64/bin/qemu-system-x86_64.exe
set +e
"$QEMU" -accel tcg,thread=multi -cpu max -m 1024 \
    -drive file=base/drill.raw,format=raw,if=ide \
    -boot c -no-reboot -nographic > "$LOG" 2>&1 &
BPID=$!

# Git Bash 的 kill 杀不掉 Windows 原生 qemu，用 taskkill；timeout 也支持
# 但 QEMU 若成功会自己退出；我们用 wait/timeout 组合
set +e
bash -c "sleep $SECS; taskkill //PID $BPID //F 2>/dev/null || true" &
KILLER=$!

wait $BPID 2>/dev/null
EXIT=$?
kill $KILLER 2>/dev/null || true
wait $KILLER 2>/dev/null

echo "== QEMU exit=$EXIT (0=自退/成功或-no-reboot触发，124=超时杀死)"
echo "== log: $LOG"
echo "Boot-from-disk count: $(grep -c 'Booting from Hard Disk' "$LOG" || true)"
echo "GRUB menu count:      $(grep -c 'GRUB4DOS 0.4.6a' "$LOG" || true)"
