# -*- coding: utf-8 -*-
"""build-ubuntu-rescue.py — 用 **Canonical 签名的 Ubuntu 内核 + 模块** 重建救援层。

用途（PIT-066 变体 D）：Secure Boot 开启时，`shim（微软签名）→ Canonical 签名的
GRUB → Canonical 签名的内核 + 我们的 initramfs`。整条链上**每个可执行文件都有
合法签名**（initrd 按 UEFI 规则不做校验），因此**不需要任何 MOK 注册**。

用户态仍复用 `build-alpine-initramfs.py` 的 Alpine 静态工具（busybox/musl/wimlib/
ntfs-3g —— 用户态与内核无关），只把「内核 + 模块」换成 Ubuntu 的。

产出（覆盖 bootfiles/ 里的同名文件，下游 `ZJ_SB_MODE=grub` 部署逻辑不用改）：
    bootfiles/vmlinuz-zjrestore             Canonical 签名的内核
    bootfiles/initramfs-zjrestore.cpio.gz   用户态 + Ubuntu 签名模块（全量）

用法：python tools/build-ubuntu-rescue.py
"""
import gzip
import importlib.util
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ZSTD = r'D:\Prog\ProgIDE\msys64\usr\bin\zstd.exe'

KVER = '6.8.0-31-generic'
DEBS = [
    ('kernel',
     'http://archive.ubuntu.com/ubuntu/pool/main/l/linux-signed/'
     'linux-image-6.8.0-31-generic_6.8.0-31.31_amd64.deb'),
    ('modules',
     'http://archive.ubuntu.com/ubuntu/pool/main/l/linux/'
     'linux-modules-6.8.0-31-generic_6.8.0-31.31_amd64.deb'),
    # -extra 里是**服务器/阵列卡**驱动（基础包不含），维修店的主战场。
    # 只挑需要的 24 个（约 1.2MB），不是整包（108MB）。
    ('extra',
     'http://archive.ubuntu.com/ubuntu/pool/main/l/linux/'
     'linux-modules-extra-6.8.0-31-generic_6.8.0-31.31_amd64.deb'),
]

# 从 -extra 里额外补入的模块（按文件名前缀匹配；依赖闭包会自动补齐）
EXTRA_MODULES = [
    'vmd',                                    # Intel VROC / RSTe（"PE 里找不到盘"第一大原因）
    'megaraid', 'megaraid_sas',               # 老 MegaRAID / SAS
    'isci', 'pm80xx', 'mvsas', 'smartpqi',    # Intel SCU / PMC / Marvell / Microchip
    'hpsa', 'arcmsr',                         # HP Smart Array / Areca
    'snic', 'libfc', 'libfcoe', 'bnx2fc', 'fnic', 'csiostor', 'be2iscsi',
    '3w-9xxx', 'advansys', 'atp870u', 'dmx3191d', 'esp_scsi', 'imm',
    'initio', 'sym53c8xx', 'wd719x',          # 老 SCSI HBA（老服务器/工控机）
]



def log(m):
    print(m, flush=True)


def load_alpine_builder():
    """加载 build-alpine-initramfs.py 复用它的用户态组装/打包函数与路径常量。"""
    p = os.path.join(HERE, 'vmtest', 'build-alpine-initramfs.py')
    spec = importlib.util.spec_from_file_location('zjinit', p)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # __main__ 保护 → 不会跑它的 main()
    return mod


def ar_members(deb):
    d = open(deb, 'rb').read()
    assert d[:8] == b'!<arch>\n', 'not an ar archive'
    off = 8
    while off + 60 <= len(d):
        hdr = d[off:off + 60]
        name = hdr[0:16].decode('ascii', 'replace').strip().rstrip('/')
        size = int(hdr[48:58].decode().strip())
        yield name, d[off + 60:off + 60 + size]
        off += 60 + size + (size % 2)


def extract_deb(deb, dest):
    for name, body in ar_members(deb):
        if not name.startswith('data.tar'):
            continue
        if name.endswith('.zst'):
            zp = deb + '.tar.zst'
            tp = deb + '.tar'
            open(zp, 'wb').write(body)
            subprocess.run([ZSTD, '-d', '-f', zp, '-o', tp], check=True)
            tf = tarfile.open(tp)
        else:
            tf = tarfile.open(fileobj=__import__('io').BytesIO(body))
        tf.extractall(dest)


def ensure_ubuntu(ubu):
    for tag, url in DEBS:
        deb = os.path.join(ubu, tag + '.deb')
        out = os.path.join(ubu, tag)
        if not os.path.exists(os.path.join(out, '.done')):
            if not os.path.exists(deb):
                log('downloading %s ...' % url.rsplit('/', 1)[-1])
                urllib.request.urlretrieve(url, deb)
            log('extracting %s ...' % os.path.basename(deb))
            shutil.rmtree(out, ignore_errors=True)
            os.makedirs(out, exist_ok=True)
            extract_deb(deb, out)
            open(os.path.join(out, '.done'), 'w').close()
    return (os.path.join(ubu, 'kernel', 'boot', 'vmlinuz-' + KVER),
            os.path.join(ubu, 'modules', 'lib', 'modules', KVER),
            os.path.join(ubu, 'extra', 'lib', 'modules', KVER))


def depends_of(raw):
    """读模块内嵌的 modinfo 字段 depends=（逗号分隔的模块名）。"""
    i = raw.find(b'depends=')
    if i < 0:
        return []
    j = raw.find(b'\x00', i)
    s = raw[i + 8:j if j > 0 else len(raw)].decode('ascii', 'replace')
    return [d for d in s.split(',') if d]


def stage_ubuntu_modules(zj, src_root, extra_root):
    """把 Ubuntu 模块装进 initramfs：基础包全量 + `-extra` 里的服务器/阵列卡子集。
    `.ko.zst` → `.ko.gz`（busybox modprobe 支持 gzip，已验证），并生成 modules.dep
    （Ubuntu 的模块包不含它，安装时才 depmod）。"""
    dst = os.path.join(zj.STG, 'lib', 'modules', KVER)
    os.makedirs(dst, exist_ok=True)
    rels, deps, name2rel = [], {}, {}

    def write_module(path, rel):
        outrel = rel[:-len('.zst')] + '.gz'
        raw = subprocess.run([ZSTD, '-d', '-c', path],
                             capture_output=True, check=True).stdout
        deps[outrel] = depends_of(raw)
        name2rel[os.path.basename(rel)[:-len('.ko.zst')]] = outrel
        tgt = os.path.join(dst, outrel)
        os.makedirs(os.path.dirname(tgt), exist_ok=True)
        with gzip.open(tgt, 'wb', compresslevel=9) as g:
            g.write(raw)
        zj.MODES[outrel] = 0o644
        rels.append(outrel)
        return outrel

    # 1) 基础包：全量（999 个）
    for dp, _dn, fns in os.walk(src_root):
        for f in fns:
            if f.endswith('.ko.zst'):
                p = os.path.join(dp, f)
                write_module(p, os.path.relpath(p, src_root)
                             .replace(os.sep, '/'))
    nbase = len(rels)

    # 2) -extra：只挑 EXTRA_MODULES（按 depends 递归补它们的额外依赖）
    extra_files = {}
    for dp, _dn, fns in os.walk(extra_root):
        for f in fns:
            if f.endswith('.ko.zst'):
                extra_files[f[:-len('.ko.zst')]] = os.path.join(dp, f)
    queue, seen, nadd = list(EXTRA_MODULES), set(), 0
    while queue:
        name = queue.pop()
        if name in seen or name in name2rel:
            continue
        seen.add(name)
        p = extra_files.get(name)
        if not p:
            log('  (extra: %s not found)' % name)
            continue
        outrel = write_module(p, os.path.relpath(p, extra_root)
                              .replace(os.sep, '/'))
        nadd += 1
        queue.extend(deps.get(outrel, []))
    log('ubuntu modules: base %d + extra %d' % (nbase, nadd))

    # modules.dep: "<模块路径>: <依赖模块路径...>"（依赖里是内建的会被丢掉）
    # ⚠️ 必须用**二进制**写：Python 在 Windows 上文本模式会把 \n 变成 \r\n，
    #    而 busybox 的 modprobe 解析出来会带上 \r → "module X not found in
    #    modules.dep"（实测踩过，错误信息里的路径尾巴有个点就是这个 \r）。
    with open(os.path.join(dst, 'modules.dep'), 'wb') as fh:
        for rel in sorted(rels):
            paths = ' '.join(name2rel[d] for d in deps.get(rel, [])
                             if d in name2rel)
            line = '%s:%s\n' % (rel, (' ' + paths) if paths else '')
            fh.write(line.encode('ascii'))
    zj.MODES['lib/modules/%s/modules.dep' % KVER] = 0o644
    for extra in ('modules.builtin', 'modules.order'):
        s = os.path.join(src_root, extra)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, extra))
            zj.MODES['lib/modules/%s/%s' % (KVER, extra)] = 0o644


def main():
    zj = load_alpine_builder()
    ubu = os.path.join(zj.DL, 'ubuntu')
    os.makedirs(ubu, exist_ok=True)
    kernel, mod_root, extra_root = ensure_ubuntu(ubu)

    if os.path.isdir(zj.STG):
        shutil.rmtree(zj.STG)
    os.makedirs(zj.STG)

    # 1) 用户态（与 Alpine 版完全一致）
    for apk in zj.APKS:
        p = os.path.join(zj.DL, apk)
        if not os.path.exists(p):
            log('!! missing %s' % apk)
            return 1
        zj.extract_apk(p, zj.STG)
    bb = os.path.join(zj.STG, 'bin', 'busybox.static')
    if os.path.exists(bb):
        os.replace(bb, os.path.join(zj.STG, 'bin', 'busybox'))
        zj.MODES['bin/busybox'] = 0o755
    sh = os.path.join(zj.STG, 'bin', 'sh')
    if os.path.lexists(sh):
        os.remove(sh)
    os.symlink('busybox', sh)
    log('userland: %d apks' % len(zj.APKS))

    # 2) Ubuntu 签名模块（全量）
    stage_ubuntu_modules(zj, mod_root, extra_root)

    # 3) 我们的脚本与引导代码块（与 Alpine 版一致）
    for d in ('proc', 'sys', 'dev', 'tmp', 'mnt'):
        os.makedirs(os.path.join(zj.STG, d), exist_ok=True)
    for name, mode in (('init', 0o755), ('zjrestore-lite.sh', 0o755),
                       ('ntfs-boot-code.bin', 0o644),
                       ('ntfs-boot-cont.bin', 0o644)):
        s = os.path.join(zj.BOOT, name if name != 'init'
                         else os.path.join('alpine', 'init'))
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(zj.STG, name))
            zj.MODES[name] = mode

    # 4) 内核（Canonical 签名）
    shutil.copy2(kernel, os.path.join(zj.BOOT, 'vmlinuz-zjrestore'))

    # 5) 打包
    out = os.path.join(zj.BOOT, 'initramfs-zjrestore.cpio.gz')
    n, rawlen = zj.pack_cpio(zj.STG, out)
    log('initramfs: %d entries, %d bytes raw, %d bytes gz' %
        (n, rawlen, os.path.getsize(out)))
    log('vmlinuz  : %d bytes (canonical-signed Ubuntu %s)' %
        (os.path.getsize(os.path.join(zj.BOOT, 'vmlinuz-zjrestore')), KVER))
    return 0


if __name__ == '__main__':
    sys.exit(main())
