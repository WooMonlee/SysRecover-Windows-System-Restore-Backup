# -*- coding: utf-8 -*-
"""build-alpine-initramfs.py — 用 Alpine 包组装 SysRecover 救援 initramfs。

为什么这样做：旧 BG-Rescue 底座的内核没有任何 SCSI HBA 驱动（mpt3sas/mptspi/
megaraid 等），在 VMware LSI SCSI / 服务器 RAID 卡上认不到硬盘（PIT-043）。
Alpine 的 linux-lts 带全套存储模块，且包是 tar.gz，可直接在 Windows 解包，
无需 Linux 环境。

产物：
  bootfiles/vmlinuz-zjrestore              (Alpine vmlinuz-lts)
  bootfiles/initramfs-zjrestore.cpio.gz    (busybox + 工具 + 模块 + /init + 还原脚本)

输入（tools/vmtest/dl/alpine/，已 gitignore）：
  linux-lts.apk, busybox-static.apk, ntfs-3g*.apk, fuse3-libs.apk, musl.apk,
  wimlib.apk, blkid/libblkid/libuuid/libeconf.apk,
  linux-firmware-{qlogic,isci,adaptec,advansys,microchip}.apk（仅存储 HBA 固件）

用法: python tools/vmtest/build-alpine-initramfs.py
"""
import gzip
import io
import os
import shutil
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BOOT = os.path.join(ROOT, 'bootfiles')
DL = os.path.join(HERE, 'dl', 'alpine')
STG = os.path.join(HERE, 'base', 'alpine-stage')
LTS = os.path.join(DL, 'lts')          # linux-lts.apk 解包缓存

NEWC = b'070701'

# relpath → 权限位（0o755/0o644…）。必须显式记录：Windows 的 os.stat 不跟踪
# 执行位（st_mode & 0o111 恒为 0），若依赖本地文件系统权限，打包后所有文件都
# 会变成 0644 → 内核执行 /init 报 EACCES(-13) → "No working init found"。
MODES = {}

# 需要打进 initramfs 的内核模块子树。
# 策略（2026-09-18 改）：这是**通用还原工具**，兼容性优先，所以 drivers/ 整树
# 全带（含 IDE/SATA/AHCI/SCSI/SAS/RAID/NVMe/UFS/MMC/USB/Thunderbolt…），
# fs/ 整树全带（NTFS/exFAT/FAT/ext4/xfs/btrfs…），只排除与"本机还原"绝对
# 无关且体积大的子系统（见 MODULE_EXCLUDES）。另配 modules.dep 依赖闭包兜底，
# 避免再出现"漏了跨目录依赖"（PIT-053: nvme 依赖 hwmon 被裁掉导致认不到盘）。
MODULE_SUBTREES = [
    'kernel/drivers',               # 全部驱动（排除项见下）
    'kernel/fs',                    # 全部文件系统
    'kernel/block',                 # t10-pi（sd_mod 依赖）
    'kernel/crypto',
    'kernel/arch/x86/crypto',
    'kernel/lib',
    'kernel/mm',
    'kernel/security',
    'kernel/kernel',
]
# 与还原无关、体积大的子树，整块排除（共约 36MB，仅此五项）
MODULE_EXCLUDES = [
    'kernel/drivers/media',         # 电视卡/摄像头/遥控（5.1MB）
    'kernel/drivers/net',           # 有线+无线网卡（18.2MB；救援层不使用网络）
    'kernel/drivers/gpu',           # DRM 显卡（9.6MB；文字控制台靠 EXTRA_MODULES 的 simpledrm）
    'kernel/drivers/infiniband',    # RDMA/InfiniBand（2.3MB）
    'kernel/drivers/staging',       # staging 实验驱动（0.4MB）
]
# 直接放在 modules 根下的索引文件（modprobe 需要）
MODULE_FILES = ['modules.dep', 'modules.alias', 'modules.builtin', 'modules.order']

# 不在 MODULE_SUBTREES 里、但救援层必须的单个模块（依赖闭包会自动带回依赖）：
#   simpledrm —— Alpine 内核带 `CONFIG_SYSFB_SIMPLEFB=y`：开机把 UEFI GOP 的
#   帧缓冲注册成 "simple-framebuffer" 平台设备，**因此内建 efifb 不绑定**；
#   能用它的只有 simpledrm。不加载 → 没有任何帧缓冲 → 没有 fbcon →
#   `console=tty0` 在 UEFI 下**一个字都不输出**（表现为"EFI stub: Loaded initrd
#   ..."之后黑屏、像卡死，实际内核在正常跑）。BIOS 路径不受影响（走 vgacon）。
EXTRA_MODULES = [
    'kernel/drivers/gpu/drm/tiny/simpledrm.ko.gz',
]

# apk → 解包（跳过元数据）
APKS = [
    'busybox-static-1.36.1-r31.apk',
    'ntfs-3g-2026.2.25-r0.apk',
    'ntfs-3g-libs-2026.2.25-r0.apk',
    'ntfs-3g-progs-2026.2.25-r0.apk',
    'fuse3-libs-3.16.2-r0.apk',
    'musl-1.2.5-r3.apk',
    'blkid-2.40.1-r1.apk',
    'libblkid-2.40.1-r1.apk',
    'libuuid-2.40.1-r1.apk',
    'libeconf-0.6.3-r0.apk',
    'wimlib-1.14.4-r0.apk',
    # 存储 HBA 固件（Alpine 把 linux-firmware 拆成 105 个子包，只取存储相关）。
    # 实测只有两个驱动要外部固件：qla2xxx（QLogic FC）与 isci（Intel C600 SCU）。
    # adaptec/advansys/microchip 一并带上（共 ~90KB，赌的是老服务器卡）。
    'linux-firmware-adaptec-20240811-r0.apk',
    'linux-firmware-advansys-20240811-r0.apk',
    'linux-firmware-isci-20240811-r0.apk',
    'linux-firmware-microchip-20240811-r0.apk',
    'linux-firmware-qlogic-20240811-r0.apk',
]


def log(msg):
    print(msg, flush=True)


def extract_apk(path, dest):
    """apk 是 tar.gz，条目无前导 '/'；保留权限，跳过 .PKGINFO/.SIGN。"""
    with tarfile.open(path, 'r:gz') as tf:
        for m in tf.getmembers():
            if m.name.startswith('.'):
                continue
            tgt = os.path.join(dest, m.name)
            if m.isdir():
                os.makedirs(tgt, exist_ok=True)
            elif m.issym():
                os.makedirs(os.path.dirname(tgt), exist_ok=True)
                if os.path.lexists(tgt):
                    os.remove(tgt)
                os.symlink(m.linkname, tgt)
            elif m.islnk():
                # apk 里多用硬链接（wimlib 的 wimlib-imagex/wimapply/… 都硬链到
                # wimappend）。cpio 无硬链接概念，改建成相对符号链接（省空间，
                # 且 argv[0] 仍是原名 → wimlib 多调用程序按名字分派行为不变）。
                os.makedirs(os.path.dirname(tgt), exist_ok=True)
                if os.path.lexists(tgt):
                    os.remove(tgt)
                rel = os.path.relpath(m.linkname,
                                      os.path.dirname(m.name) or '.')
                os.symlink(rel, tgt)
            elif m.isfile():
                os.makedirs(os.path.dirname(tgt), exist_ok=True)
                with tf.extractfile(m) as src, open(tgt, 'wb') as out:
                    shutil.copyfileobj(src, out)
                os.chmod(tgt, m.mode & 0o777)
                MODES[m.name] = m.mode & 0o777


def ensure_lts():
    vm = os.path.join(LTS, 'boot', 'vmlinuz-lts')
    if os.path.exists(vm):
        return
    log('extract linux-lts.apk (99MB, once) ...')
    os.makedirs(LTS, exist_ok=True)
    extract_apk(os.path.join(DL, 'linux-lts.apk'), LTS)


def newc(name, mode, data=b''):
    nm = name.encode() + b'\x00'
    ns, fs = len(nm), len(data)
    fields = [0, mode, 0, 0, 1, 0, fs, 0, 0, 0, 0, ns, 0]
    hdr = NEWC + b''.join(f'{f:08x}'.encode() for f in fields)
    assert len(hdr) == 110
    hn = 110 + ns
    pn = (4 - hn % 4) % 4
    pd = (4 - fs % 4) % 4
    return hdr + nm + b'\x00' * pn + data + b'\x00' * pd


def pack_cpio(stage, out_path):
    entries = []
    for dp, dns, fns in os.walk(stage):
        for d in dns:
            p = os.path.join(dp, d)
            rel = os.path.relpath(p, stage).replace(os.sep, '/')
            entries.append((rel, 0o040755, b''))
        for f in fns:
            p = os.path.join(dp, f)
            rel = os.path.relpath(p, stage).replace(os.sep, '/')
            if os.path.islink(p):
                entries.append((rel, 0o120777,
                                os.readlink(p).encode()))
            else:
                with open(p, 'rb') as fh:
                    blob = fh.read()
                mode = 0o100000 | MODES.get(rel, 0o644)
                entries.append((rel, mode, blob))
    entries.sort(key=lambda x: (x[0].count('/'), x[0]))
    buf = io.BytesIO()
    for name, mode, blob in entries:
        buf.write(newc(name, mode, blob))
    buf.write(newc('TRAILER!!!', 0, b''))
    raw = buf.getvalue()
    with gzip.open(out_path, 'wb', compresslevel=9) as gz:
        gz.write(raw)
    return len(entries), len(raw)


def main():
    ensure_lts()

    if os.path.isdir(STG):
        shutil.rmtree(STG)
    os.makedirs(STG)

    # 1) 用户态工具
    for apk in APKS:
        p = os.path.join(DL, apk)
        if not os.path.exists(p):
            log(f'!! missing {apk}')
            sys.exit(1)
        extract_apk(p, STG)
        log(f'extracted {apk}')

    # busybox-static 的实体叫 bin/busybox.static → 规范成 /bin/busybox
    bb_src = os.path.join(STG, 'bin', 'busybox.static')
    if os.path.exists(bb_src):
        os.replace(bb_src, os.path.join(STG, 'bin', 'busybox'))
        MODES['bin/busybox'] = 0o755

    # 预置 /bin/sh → busybox：内核执行 /init 的 #!/bin/sh 时必须已存在，
    # 否则 execve 失败 → "No working init found"（其余 applet 由 /init 里
    # busybox --install -s 运行时生成）
    for a in ('sh',):
        lnk = os.path.join(STG, 'bin', a)
        if os.path.lexists(lnk):
            os.remove(lnk)
        os.symlink('busybox', lnk)

    # 2) 内核模块子集
    mod_root = os.path.join(LTS, 'lib', 'modules')
    kver = os.listdir(mod_root)[0]
    src_k = os.path.join(mod_root, kver, 'kernel')
    dst_k = os.path.join(STG, 'lib', 'modules', kver, 'kernel')
    nmod = 0
    copied = []          # 已收集模块（相对 /lib/modules/<kver> 的路径）
    ex_set = [e.replace('kernel/', '') for e in MODULE_EXCLUDES]

    def _excluded(fullp):
        return any(fullp == e or fullp.startswith(e + '/') for e in ex_set)

    for sub in MODULE_SUBTREES:
        s = os.path.join(src_k, sub.replace('kernel/', ''))
        d = os.path.join(dst_k, sub.replace('kernel/', ''))
        if not os.path.isdir(s):
            log(f'  (skip missing module dir {sub})')
            continue
        for dp, dns, fns in os.walk(s):
            fullp = os.path.relpath(dp, src_k).replace(os.sep, '/')
            if _excluded(fullp):
                dns[:] = []          # 不再深入被排除的子树
                continue
            rel = os.path.relpath(dp, s)
            tgt = os.path.join(d, rel) if rel != '.' else d
            os.makedirs(tgt, exist_ok=True)
            for f in fns:
                shutil.copy2(os.path.join(dp, f), os.path.join(tgt, f))
                # 必须是正斜杠相对路径，才能与 modules.dep 的键匹配
                copied.append(os.path.relpath(os.path.join(tgt, f),
                                              os.path.join(STG, 'lib', 'modules', kver)
                                              ).replace(os.sep, '/'))
                nmod += 1

    # ── 额外单模块（simpledrm 等，见 EXTRA_MODULES 注释）──
    for rel in EXTRA_MODULES:
        src = os.path.join(mod_root, kver, rel)
        if not os.path.isfile(src):
            log(f'  (skip missing extra module {rel})')
            continue
        tgt = os.path.join(STG, 'lib', 'modules', kver, rel)
        os.makedirs(os.path.dirname(tgt), exist_ok=True)
        shutil.copy2(src, tgt)
        copied.append(rel)
        nmod += 1

    # ── 依赖闭包：按 modules.dep 补齐被子树裁剪漏掉的依赖 ──
    # 例：nvme.ko.gz 依赖 hwmon.ko.gz（不在任何子树里），缺了会让
    # `modprobe nvme` 直接报 "can't load module hwmon: No such file" → 认不到 NVMe 盘。
    depmap = {}
    dep_path = os.path.join(mod_root, kver, 'modules.dep')
    if os.path.exists(dep_path):
        with open(dep_path, 'r', encoding='utf-8', errors='replace') as fh:
            for line in fh:
                if ':' not in line:
                    continue
                left, right = line.split(':', 1)
                depmap[left.strip()] = right.split()
    queue, seen, extra = list(copied), set(copied), []
    while queue:
        for dep in depmap.get(queue.pop(), []):
            if dep in seen:
                continue
            seen.add(dep)
            extra.append(dep)
            queue.append(dep)
    nadd = 0
    for dep in extra:
        src = os.path.join(mod_root, kver, dep)
        if not os.path.isfile(src):
            continue
        tgt = os.path.join(STG, 'lib', 'modules', kver, dep)
        os.makedirs(os.path.dirname(tgt), exist_ok=True)
        shutil.copy2(src, tgt)
        nadd += 1
    log(f'  dep closure: +{nadd} modules ({len(extra)} required)')

    for f in MODULE_FILES:
        s = os.path.join(mod_root, kver, f)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(STG, 'lib', 'modules', kver, f))
    log(f'modules: {nmod} files, kver={kver}')

    # 3) 我们的脚本
    for d in ('proc', 'sys', 'dev', 'tmp', 'mnt'):
        os.makedirs(os.path.join(STG, d), exist_ok=True)
    shutil.copy2(os.path.join(BOOT, 'alpine', 'init'),
                 os.path.join(STG, 'init'))
    os.chmod(os.path.join(STG, 'init'), 0o755)
    MODES['init'] = 0o755
    shutil.copy2(os.path.join(BOOT, 'zjrestore-lite.sh'),
                 os.path.join(STG, 'zjrestore-lite.sh'))
    os.chmod(os.path.join(STG, 'zjrestore-lite.sh'), 0o755)
    MODES['zjrestore-lite.sh'] = 0o755
    # 微软 NTFS 引导代码（426 字节，0x54..0x1FD）：随包内置，还原时写回目标 PBR。
    # 不能依赖"格式化前备份的 PBR"——若上次还原已把 PBR 换成 mkntfs 自带的代码，
    # 备份就会是那份非引导代码，导致自我复制、开机黑屏光标闪（PIT-050）。
    nb = os.path.join(BOOT, 'ntfs-boot-code.bin')
    if os.path.exists(nb):
        shutil.copy2(nb, os.path.join(STG, 'ntfs-boot-code.bin'))
        MODES['ntfs-boot-code.bin'] = 0o644
    # 引导代码的「后续 8 个扇区」：Windows 的 NTFS 引导代码不是 512 字节，而是跨
    # 9 个扇区（第一扇区代码 0x54..0x1FD + 扇区 1..8）。缺了它，代码里那句
    # `jmp 0x226` 会跳进空白 → 死循环光标闪（PIT-052）。
    nc = os.path.join(BOOT, 'ntfs-boot-cont.bin')
    if os.path.exists(nc):
        shutil.copy2(nc, os.path.join(STG, 'ntfs-boot-cont.bin'))
        MODES['ntfs-boot-cont.bin'] = 0o644

    # 4) 内核
    shutil.copy2(os.path.join(LTS, 'boot', 'vmlinuz-lts'),
                 os.path.join(BOOT, 'vmlinuz-zjrestore'))
    vm_size = os.path.getsize(os.path.join(BOOT, 'vmlinuz-zjrestore'))

    # 5) 打包
    out = os.path.join(BOOT, 'initramfs-zjrestore.cpio.gz')
    n, rawlen = pack_cpio(STG, out)
    log(f'initramfs: {n} entries, {rawlen} bytes raw, '
        f'{os.path.getsize(out)} bytes gz')
    log(f'vmlinuz  : {vm_size} bytes')


if __name__ == '__main__':
    main()
