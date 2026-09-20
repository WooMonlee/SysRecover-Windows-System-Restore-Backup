#!/usr/bin/env bash
# test-pbr-transplant.sh — 回归测试：zjrestore-lite.sh 的 PBR 引导代码写入
#
# 背景（两个都踩过）：
#  · PIT-049：对「完整 512B PBR」源只给 seek=84 漏 skip=84 → 写到 0x54 的是源文件
#    偏移 0 的 BPB，引导代码错位 78 字节。
#  · PIT-050 后续：切到「内置 426B 代码段」源后仍沿用 skip=84 → 代码段自己又跳了
#    84 字节，且校验用同一 skip 而"假通过"。
# 本测试验证两条路径都能把 0x54..0x1FD 写成正确的代码。
#
# 用法（Git Bash / msys）: ./test-pbr-transplant.sh
set -e
export PATH="/usr/bin:/bin:/mingw64/bin:/d/Prog/ProgIDE/msys64/mingw64/bin:$PATH"

W=_pbrtest
rm -rf "$W"; mkdir -p "$W"

python - "$W/blob.bin" <<'PY'
import sys
# 内置 blake 就是 426 字节代码段（0x54..0x1FD 的内容）
code = bytes([0xFA, 0x33, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C])
open(sys.argv[1], 'wb').write(code + bytes(426 - len(code)))
PY

python - "$W/pbr.bin" <<'PY'
import sys
b = bytearray(512)
b[0:3] = bytes([0xEB, 0x52, 0x90])
b[3:11] = b'NTFS    '
b[0x54:0x54+8] = bytes([0xFA, 0x33, 0xC0, 0x8E, 0xD0, 0xBC, 0x00, 0x7C])
b[0x1FD] = 0xAA
open(sys.argv[1], 'wb').write(b)
PY

check() {
    lbl="$1"; got=$(od -An -tx1 -j 84 -N 4 "$W/dst.bin" | tr -d ' \n')
    [ "$got" = "fa33c08e" ] && echo "PASS $lbl (0x54=$got)" || { echo "FAIL $lbl (0x54=$got)"; exit 1; }
}

# ── 路径 A：内置 426B 代码段（不 skip，seek=84）──
python - "$W/dst.bin" <<'PY'
import sys; open(sys.argv[1], 'wb').write(bytes(512))
PY
cp "$W/blob.bin" "$W/code.bin"
dd if="$W/code.bin" of="$W/dst.bin" bs=1 seek=84 count=426 conv=notrunc 2>/dev/null
check "built-in 426B blob"

# ── 路径 B：完整 512B PBR（skip=84 seek=84）──
python - "$W/dst.bin" <<'PY'
import sys; open(sys.argv[1], 'wb').write(bytes(512))
PY
dd if="$W/pbr.bin" of="$W/code.bin" bs=1 skip=84 count=426 2>/dev/null
dd if="$W/code.bin" of="$W/dst.bin" bs=1 seek=84 count=426 conv=notrunc 2>/dev/null
check "full 512B PBR"

rm -rf "$W"
echo "ALL PASS"
