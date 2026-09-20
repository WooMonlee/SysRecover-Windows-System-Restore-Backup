# -*- coding: utf-8 -*-
"""build-uki.py — 打 UKI（Unified Kernel Image）并用我们的 MOK 密钥签名。

为什么需要这个：Secure Boot 开着时，固件只加载**微软签名**的镜像。我们的内核
没签名 → 必须走「微软签名的 shim → 我们签名的 UKI」这条链：
  固件 → shimx64.efi（微软签名，随包）→ grubx64.efi(= 本脚本产出的 UKI)
       → systemd-stub 读自己的 .cmdline/.initrd/.linux 节 → 启动我们的内核

内核自己**不支持** PE `.initrd`/`.cmdline` 节（源码只认 `initrd=` 命令行或
LINUX_EFI_INITRD_MEDIA_GUID 设备路径），所以必须借 systemd-stub 来做这件事。

用法：
  python tools/build-uki.py [--out dist/bootfiles/sb/zjrestore-uki.efi]

依赖：
  * bootfiles/vmlinuz-zjrestore, bootfiles/initramfs-zjrestore.cpio.gz
  * bootfiles/sb/linuxx64.efi.stub           （systemd-stub，LGPL-2.1+）
  * keys/zj-mok.key + keys/zj-mok.crt        （我们的 MOK 私钥/证书；不入库）
  * mingw objcopy（构建工具）+ osslsigncode（缺则自动下载到 tools/sb/dl/）
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT = os.path.join(ROOT, 'bootfiles')
SB = os.path.join(BOOT, 'sb')
KEYS = os.path.join(ROOT, 'keys')
DL = os.path.join(ROOT, 'tools', 'sb', 'dl')
OBJCOPY = r'D:\Prog\ProgIDE\mingw64\bin\objcopy.exe'
# osslsigncode 依赖 mingw64 的 OpenSSL/zlib DLL（部分在 usr\bin），两处都加进 PATH
TOOL_PATH = r'D:\Prog\ProgIDE\msys64\mingw64\bin;D:\Prog\ProgIDE\msys64\usr\bin'

# UKI 各节相对 ImageBase 的偏移（systemd 文档给的布局；必须互不重叠）。
# objcopy 要求节的 VMA >= 镜像的 ImageBase —— 而预编译的 linuxx64.efi.stub
# 的 ImageBase 很高（实测 0x14df90000），硬写 0x20000 会报
# "section below image base"，所以运行时读出来再加。
SEC_OFF = {'.osrel': 0x20000, '.cmdline': 0x30000, '.linux': 0x2000000,
           '.initrd': 0x3000000}

CMDLINE = 'console=tty0 nvme_core.io_timeout=1 zjre=1'
OSREL = ('ID=sysrecover\nNAME="SysRecover Rescue"\n'
         'PRETTY_NAME="SysRecover Rescue"\nVERSION_ID=0.1.0\n')


def log(msg):
    print(msg, flush=True)


def run(cmd, **kw):
    log('  $ ' + ' '.join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


OPENSSL = r'D:\Prog\ProgIDE\msys64\usr\bin\openssl.exe'


def ensure_keys():
    """MOK 私钥/证书：没有就生成（私钥不进仓库，必须自行备份 —— 丢了以后
    老客户机器上已注册的 MOK 就对不上新签名了）。"""
    key = os.path.join(KEYS, 'zj-mok.key')
    crt = os.path.join(KEYS, 'zj-mok.crt')
    cer = os.path.join(KEYS, 'zj-mok.cer')
    if not (os.path.exists(key) and os.path.exists(crt)):
        os.makedirs(KEYS, exist_ok=True)
        log('generating MOK key (keys/zj-mok.key + .crt) ...')
        # 必须带 Code Signing EKU：UEFI/shim 校验 EFI 镜像签名时会检查这个扩展，
        # 用 openssl 默认的裸证书签出来的签名通不过（实测踩过）。
        run([OPENSSL, 'req', '-new', '-x509', '-newkey', 'rsa:2048',
             '-keyout', key, '-out', crt, '-days', '3650', '-nodes',
             '-addext', 'extendedKeyUsage=codeSigning',
             '-addext', 'keyUsage=digitalSignature',
             '-subj', '/CN=SysRecover Rescue MOK/O=SysRecover'])
    if not os.path.exists(cer):
        run([OPENSSL, 'x509', '-in', crt, '-outform', 'DER', '-out', cer])
    return key, crt, cer


def image_base(pe):
    """读 PE 的 ImageBase（objcopy 的 --change-section-vma 是绝对 VMA）。"""
    out = subprocess.run([OBJCOPY.replace('objcopy', 'objdump'), '-p', pe],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if 'ImageBase' in line:
            return int(line.split()[-1], 16)
    raise RuntimeError('cannot read ImageBase from ' + pe)


def ensure_osslsigncode():
    """osslsigncode 不进仓库：缺就从 msys2 镜像下一个包、解出 exe 用。
    它需要 mingw64 的 OpenSSL DLL（开发机已装 openssl.exe 就有）。"""
    exe = os.path.join(DL, 'bin', 'osslsigncode.exe')
    if os.path.exists(exe):
        return exe
    os.makedirs(DL, exist_ok=True)
    log('downloading osslsigncode from msys2 mirror ...')
    listing = urllib.request.urlopen(
        'https://repo.msys2.org/mingw/mingw64/', timeout=60).read().decode(
        'utf-8', 'replace')
    names = [m for m in listing.split('"')
             if m.startswith('mingw-w64-x86_64-osslsigncode-')
             and m.endswith('.pkg.tar.zst')]
    if not names:
        raise RuntimeError('osslsigncode package not found on mirror')
    url = 'https://repo.msys2.org/mingw/mingw64/' + sorted(names)[-1]
    pkg = os.path.join(DL, 'osslsigncode.pkg.tar.zst')
    urllib.request.urlretrieve(url, pkg)
    # .pkg.tar.zst 就是 tar+zstd：用 Windows 自带 tar（bsdtar 认 zstd）
    # 注意：Windows 的 tar 是 bsdtar，不认 GNU 的 --force-local；用 cwd + 相对路径。
    pkgname = os.path.basename(pkg)
    tmpdir = os.path.join(DL, 'x')
    shutil.rmtree(tmpdir, ignore_errors=True)
    os.makedirs(tmpdir, exist_ok=True)
    run(['tar', '-xf', pkgname, '-C', 'x'], cwd=DL)
    for dp, _dn, fn in os.walk(tmpdir):
        if 'osslsigncode.exe' in fn:
            src = os.path.join(dp, 'osslsigncode.exe')
            os.makedirs(os.path.dirname(exe), exist_ok=True)
            shutil.copy2(src, exe)
            return exe
    raise RuntimeError('osslsigncode.exe not found in package')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(
        ROOT, 'dist', 'bootfiles', 'sb', 'zjrestore-uki.efi'))
    args = ap.parse_args()

    kernel = os.path.join(BOOT, 'vmlinuz-zjrestore')
    initrd = os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz')
    stub = os.path.join(SB, 'linuxx64.efi.stub')
    key, crt, cer = ensure_keys()
    for p in (kernel, initrd, stub, key, crt):
        if not os.path.exists(p):
            log('MISSING: ' + p)
            return 1

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='zj-uki-')
    try:
        p_cmd = os.path.join(tmp, 'cmdline')
        p_osrel = os.path.join(tmp, 'osrel')
        open(p_cmd, 'w', encoding='ascii').write(CMDLINE + '\n')
        open(p_osrel, 'w', encoding='ascii').write(OSREL)

        unsigned = os.path.join(tmp, 'uki.efi')
        ib = image_base(stub)
        log('objcopy: building UKI (%s + %s + initramfs), ImageBase=0x%x ...' %
            (os.path.basename(stub), os.path.basename(kernel), ib))
        cmd = [OBJCOPY]
        for sec, path in (('.osrel', p_osrel), ('.cmdline', p_cmd),
                          ('.linux', kernel), ('.initrd', initrd)):
            cmd += ['--add-section', '%s=%s' % (sec, path),
                    '--change-section-vma', '%s=0x%x' % (sec, ib + SEC_OFF[sec])]
        cmd += [stub, unsigned]
        run(cmd)
        log('  unsigned UKI: %.1f MB' % (os.path.getsize(unsigned) / 1048576))

        exe = ensure_osslsigncode()
        env = dict(os.environ)
        env['PATH'] = TOOL_PATH + os.pathsep + env.get('PATH', '')
        log('osslsigncode: signing with keys/zj-mok ...')
        # 先删旧产物：osslsigncode 对已存在的输出文件有时会直接失败（实测 -1）
        if os.path.exists(args.out):
            os.remove(args.out)
        run([exe, 'sign', '-certs', crt, '-key', key, '-h', 'sha256',
             '-in', unsigned, '-out', args.out], env=env)
        log('  signed UKI : %s (%.1f MB)' %
            (args.out, os.path.getsize(args.out) / 1048576))

        log('verify (against our own cert as CA):')
        run([exe, 'verify', '-in', args.out, '-CAfile', crt], env=env)
        # 公钥证书随包发到 ESP（用户注册 MOK 时选它）
        shutil.copy2(cer, os.path.join(os.path.dirname(args.out),
                                       'zj-mok.cer'))
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    sys.exit(main())
