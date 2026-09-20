#!/usr/bin/env bash
# run-boot-test.sh — 单次 Linux 引导链测试（TCG + -cpu max，必须有 -cpu max！PIT-013）
# 用法: ./run-boot-test.sh [秒数，默认 90]
set -e
cd "$(dirname "$0")"
SECS=${1:-90}
TS=$(date +%Y%m%d-%H%M%S)
LOG=logs/boot-$TS.log
mkdir -p logs

QEMU=/d/Prog/ProgIDE/msys64/mingw64/bin/qemu-system-x86_64.exe
"$QEMU" -accel tcg,thread=multi -cpu max -m 1024 \
    -drive file=base/testdisk.raw,format=raw,if=ide \
    -boot c -nographic > "$LOG" 2>&1 &
BPID=$!

# Git Bash 的 kill 杀不掉 Windows 原生 qemu，用 taskkill
sleep "$SECS"
taskkill //PID $BPID //F 2>/dev/null || kill -9 $BPID 2>/dev/null || true
sleep 1

echo "== log: $LOG"
echo "Boot-from-disk count: $(grep -c 'Booting from Hard Disk' "$LOG" || true)"
echo "GRUB menu count:      $(grep -c "GRUB4DOS 0.4.6a" "$LOG" || true)"
echo
echo "出现 ≥2 次 = 内核崩溃重启循环（查 -cpu max）；1 次且持续运行 = 存活（控制台不可见属已知）"
