# -*- coding: utf-8 -*-
"""pbr-probe.py — 验证「mkntfs 快速格式化的分区 + 微软引导区」能否引导 Windows。

背景（PIT-053）：我们一度以为 Windows 的 NTFS 引导代码不认 mkntfs 的布局，
于是改成"保留原 NTFS 只清空文件"。后来证明那是误诊：真实原因是我们移植引导区
时漏写了扇区 1..8（引导代码会先读入 16 个扇区，再 `jmp 0x226` 跳到扇区 1 执行）
以及 `dd skip=84` 错位。

原理：做一个「mkntfs + 完整微软引导区 + 根目录里**没有** bootmgr」的分区，
用 GRUB4DOS chainload 它。如果引导代码能解析这个 mkntfs 卷，它必然报
    BOOTMGR is missing / Press Ctrl+Alt+Del to restart
否则黑屏/光标闪。

准备（两条命令，见 tools/vmtest/README.md）：
    ZJ_DRILL_WIM=<marker-only.wim> ZJ_DRILL_NOBOOTFIX=1 ./mk-drill.sh
    powershell -File run-drill.ps1 -Secs 220        # 让 /init 完成格式化+写引导区
然后：
    python tools/vmtest/pbr-probe.py

用法: python tools/vmtest/pbr-probe.py [--keep]
退出码: 0 = 探针通过（引导代码认得 mkntfs），1 = 失败
"""
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = r'D:\Prog\ProgIDE\msys64\mingw64\bin\qemu-system-x86_64.exe'
MCOPY = r'D:\Prog\ProgIDE\msys64\mingw64\bin\mcopy.exe'
IMG = os.path.join(HERE, 'base', 'drill.raw')
S1_OFF = 1048576            # sda1 (FAT32 + grldr) 起始字节
PORT = 5601
MENU = (b'timeout 10\ndefault 0\ntitle chainload NTFS PBR (probe)\n'
        b'rootnoverify (hd0,1)\nchainloader +1\n')
NEEDLE = b'BOOTMGR is missing'


def patch_menu():
    tmp = os.path.join(HERE, 'base', 'menu-chain.lst')
    with open(tmp, 'wb') as f:
        f.write(MENU)
    r = subprocess.run([MCOPY, '-o', '-i', '%s@@%d' % (IMG, S1_OFF), tmp, '::menu.lst'],
                       capture_output=True)
    if r.returncode != 0:
        print('mcopy failed:', r.stderr.decode('latin1'))
        return False
    return True


def vga_text(f, dump_path):
    """用 monitor 的 memsave 把 0xB8000 的 VGA 文本缓冲存成文件再解析。

    （不要用 `xp` —— monitor 会逐字符回显命令行，输出里全是转义序列，解析不可靠。）
    """
    if os.path.exists(dump_path):
        os.remove(dump_path)
    f.write(b'memsave 0xb8000 4000 ' + dump_path.encode() + b'\n')
    time.sleep(1.5)
    if not os.path.exists(dump_path):
        return b''
    d = open(dump_path, 'rb').read()
    # 每 2 字节 = 字符 + 属性
    return bytes(d[0::2])


def main():
    if not os.path.exists(IMG):
        print('missing %s — run mk-drill.sh first' % IMG)
        return 1
    if not patch_menu():
        return 1
    p = subprocess.Popen(
        [QEMU, '-m', '512', '-drive', 'file=base/drill.raw,format=raw,if=ide',
         '-boot', 'c', '-display', 'none', '-vga', 'std',
         '-monitor', 'tcp:127.0.0.1:%d,server,nowait' % PORT],
        cwd=HERE)
    time.sleep(16)
    text = b''
    try:
        s = socket.create_connection(('127.0.0.1', PORT), timeout=10)
        f = s.makefile('rwb', 0)
        time.sleep(0.5)
        text = vga_text(f, os.path.join(HERE, 'base', 'vga-text.bin'))
        f.write(b'quit\n')
        time.sleep(1)
        s.close()
    except Exception as e:
        print('monitor error:', e)
    try:
        p.wait(timeout=10)
    except Exception:
        p.kill()
    printable = text.replace(b'\x00', b' ').decode('latin1').strip()
    print('--- VGA text ---')
    print(printable[:600])
    print('----------------')
    if NEEDLE in text:
        print('PASS: Microsoft NTFS boot code parsed the mkntfs volume')
        return 0
    print('FAIL: "%s" not found on screen' % NEEDLE.decode())
    return 1


if __name__ == '__main__':
    sys.exit(main())
