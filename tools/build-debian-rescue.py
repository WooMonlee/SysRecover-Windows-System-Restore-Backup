# -*- coding: utf-8 -*-
"""build-debian-rescue.py — 用 **Debian 签名的内核 + 模块** 重建救援层（备选 B）。

为什么换 Debian（2026-09-23 实测对比，见 PLAN.md §11.1）：
  * Debian 的 `shim-signed 1.51+16.1-2` 是 **CA2011 + CA2023 双签** → 老固件与
    **2026 新出厂只信 CA2023 的固件**都能启动（这一条 Ubuntu 做不到：目前只有 CA2011）。
  * 关键存储/HBA 驱动 **31/31 齐全**（含 `vmd`(Intel VROC)、`ntfs3`），AlmaLinux 缺 5 项。
  * 内核反而**最小**（vmlinuz 11.6MB vs Ubuntu 14.2MB）。
链：`固件 → shim（微软双签）→ grubx64.efi（Debian 签名）→ vmlinuz（Debian 签名）+ 我们的 initramfs`。
initrd 按 UEFI 规则不校验，所以**我们的用户态不用换**。

与 build-ubuntu-rescue.py 的差别：
  1. Debian 只发 **一个包**（`linux-image-<abi>-amd64` = 内核 + **全部 4225 个模块**，89MB）
     没有 Ubuntu 那种 base/extra 之分 → 这里**按路径白名单裁剪**到存储/文件系统相关
     （否则 initramfs 会从 34MB 涨到 ~90MB），依赖闭包仍自动补齐。
     **2026-09-24（PIT-079）再加一层 EXCLUDE_PREFIXES**：白名单里 `kernel/fs/` 是整
     目录，会把网络/集群/嵌入式文件系统全拉进来；剔除后模块 779→约 490、省 ~15.7MB。
     另补 EXTRA_KEEP = USB HID（救援 shell 的 USB 键盘；PS/2 那套是内核内建的）。
  2. 模块是 **`.ko.xz`**，统一转成 **`.ko.gz`**（busybox modprobe 走 gzip —— 现在的
     Ubuntu 版就是这么跑的，已验证），用 Python 自带的 `lzma`/`gzip`，不需要外部工具。
  3. `modules.dep` 依然自己生成（Debian 的包不含它）。

产出（文件名不变，下游部署逻辑零改动）：
    bootfiles/vmlinuz-zjrestore              Debian 签名的内核
    bootfiles/initramfs-zjrestore.cpio.gz    用户态 + Debian 签名模块（存储子集）

用法：python tools/build-debian-rescue.py
"""
import gzip
import importlib.util
import io
import lzma
import os
import shutil
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))

ABI = '6.12.107+deb13-amd64'
DEB_URL = ('https://mirrors.tuna.tsinghua.edu.cn/debian/pool/main/l/'
           'linux-signed-amd64/linux-image-6.12.107+deb13-amd64_6.12.107-1_amd64.deb')

# 只收这些子系统（模块在包内的相对路径，形如 kernel/drivers/scsi/...）。
# 覆盖：SATA/AHCI、SCSI 与各家 HBA、NVMe、块设备、软 RAID/多路径、virtio、
# USB(存储+主控)、MMC/eMMC、光纤通道(Fusion)、CD-ROM，以及全部 fs/ 与加密/压缩库。
KEEP_PREFIXES = [
    'kernel/drivers/ata/', 'kernel/drivers/scsi/', 'kernel/drivers/nvme/',
    'kernel/drivers/block/', 'kernel/drivers/md/', 'kernel/drivers/raid/',
    'kernel/drivers/virtio/', 'kernel/drivers/usb/', 'kernel/drivers/mmc/',
    'kernel/drivers/fusion/', 'kernel/drivers/message/', 'kernel/drivers/cdrom/',
    'kernel/drivers/firmware/', 'kernel/drivers/acpi/',
    # vmd（Intel VROC/RSTe）在 drivers/pci/controller/ 下 —— 不在存储目录里，容易漏。
    'kernel/drivers/pci/',
    # ⚠️ 必须有：UEFI 下内核把 GOP 帧缓冲注册成 simple-framebuffer，内建 efifb 不绑定，
    #    只有 simpledrm 能接管 → 否则 UEFI 显示"黑屏像卡死"（PIT-061）。整个 tiny/ 很小。
    'kernel/drivers/gpu/drm/tiny/',
    'kernel/fs/', 'kernel/lib/', 'kernel/crypto/', 'kernel/arch/x86/',
]

# 明确排除（对**白名单命中**与**依赖闭包**两处同时生效）。
# 依据 2026-09-24 调研（AGENTS.md PIT-079）：下列模块对"本地盘 Windows 还原"完全
# 用不到，删掉可省 ~15.7MB（.ko.gz）。已逐条校验"没有保留模块依赖被排除模块"，
# 且构建期有**断言**兜底（真冲突会直接报错，不会静默产出坏包）。
EXCLUDE_PREFIXES = [
    # 网络文件系统（镜像不通过网络盘）
    'kernel/fs/nfs/', 'kernel/fs/nfsd/', 'kernel/fs/nfs_common/', 'kernel/fs/smb/',
    'kernel/fs/ceph/', 'kernel/fs/afs/', 'kernel/fs/9p/', 'kernel/fs/cachefiles/',
    'kernel/fs/netfs/',
    # 集群/Unix 系文件系统
    'kernel/fs/ocfs2/', 'kernel/fs/gfs2/', 'kernel/fs/dlm/', 'kernel/fs/lockd/',
    'kernel/fs/jfs/', 'kernel/fs/reiserfs/', 'kernel/fs/nilfs2/', 'kernel/fs/ubifs/',
    'kernel/fs/jffs2/', 'kernel/fs/hfs/', 'kernel/fs/hfsplus/', 'kernel/fs/erofs/',
    'kernel/fs/squashfs/', 'kernel/fs/zonefs/', 'kernel/fs/orangefs/', 'kernel/fs/ecryptfs/',
    'kernel/fs/coda/', 'kernel/fs/ufs/', 'kernel/fs/sysv/', 'kernel/fs/minix/',
    'kernel/fs/befs/', 'kernel/fs/adfs/', 'kernel/fs/omfs/', 'kernel/fs/freevxfs/',
    'kernel/fs/qnx4/', 'kernel/fs/qnx6/', 'kernel/fs/bfs/', 'kernel/fs/efs/',
    'kernel/fs/hpfs/', 'kernel/fs/affs/', 'kernel/fs/romfs/', 'kernel/fs/vboxsf/',
    'kernel/fs/ntfs/',        # 老只读 ntfs 驱动（我们走 ntfs3 / ntfs-3g）
    'kernel/fs/fuse/',        # fuse 本身内建（CONFIG_FUSE_FS=y），这里的 cuse 不需要
    # 用不到的通用 fs
    'kernel/fs/overlayfs/', 'kernel/fs/autofs/', 'kernel/fs/efivarfs/',
    'kernel/fs/pstore/', 'kernel/fs/binfmt_misc',
    # 虚拟化/媒体/声音/IB/测试
    'kernel/arch/x86/kvm/', 'kernel/drivers/media/', 'kernel/sound/',
    'kernel/drivers/infiniband/', 'kernel/drivers/target/', 'kernel/drivers/parport/',
    'kernel/lib/test_', 'kernel/lib/notifier-error-inject',
    'kernel/lib/pm-notifier-error-inject', 'kernel/lib/memory-notifier-error-inject',
    'kernel/lib/test_static_key', 'kernel/crypto/tcrypt',
    # 网络存储（NVMe-oF / iSCSI / FCoE offload）—— 本地盘还原用不到，
    # 但它们会经闭包拉进 drivers/net、infiniband、target、libfc 一大串
    'kernel/drivers/net/',
    'kernel/drivers/nvme/host/nvme-rdma', 'kernel/drivers/nvme/host/nvme-tcp',
    'kernel/drivers/nvme/target/nvmet-rdma', 'kernel/drivers/nvme/target/nvmet-tcp',
    'kernel/drivers/scsi/be2iscsi/', 'kernel/drivers/scsi/bnx2fc/',
    'kernel/drivers/scsi/bnx2i/', 'kernel/drivers/scsi/qla4xxx/',
    'kernel/drivers/scsi/cxgbi/', 'kernel/drivers/scsi/fcoe/',
    'kernel/drivers/scsi/fnic/', 'kernel/drivers/scsi/qedf/',
    'kernel/drivers/scsi/qedi/', 'kernel/drivers/scsi/libfc/',
    'kernel/drivers/scsi/iscsi_boot_sysfs', 'kernel/drivers/scsi/iscsi_tcp',
    'kernel/drivers/scsi/libiscsi', 'kernel/drivers/scsi/scsi_transport_iscsi',
    'kernel/drivers/scsi/qla2xxx/tcm_qla2xxx',
    'kernel/drivers/firmware/iscsi_ibft',
    # USB 设备端(gadget)/Type-C/串口/摄像头等（还原用不到）
    'kernel/drivers/usb/typec/', 'kernel/drivers/usb/gadget/',
    'kernel/drivers/usb/dwc3/', 'kernel/drivers/usb/usbip/',
    'kernel/drivers/usb/misc/', 'kernel/drivers/usb/class/',
    'kernel/drivers/usb/serial/', 'kernel/drivers/usb/atm/',
    'kernel/drivers/usb/image/',
    # 与磁盘无关的块/md
    'kernel/drivers/block/null_blk', 'kernel/drivers/md/md-cluster',
    # DRM/KMS：Debian 的 efifb/simplefb/FRAMEBUFFER_CONSOLE **内建**
    # （CONFIG_FB_EFI/FB_SIMPLE=y），控制台不需要 KMS。（PIT-061 是 Alpine 内核
    # 的特有问题——它 SYSFB_SIMPLEFB=y 会顶掉 efifb，Debian 不存在。）
    'kernel/drivers/gpu/drm/tiny/',
]

# 精确补收：不在 KEEP_PREFIXES 里、但救援 shell 需要 —— **USB 键盘/鼠标**。
# PS/2 那套（atkbd/i8042/libps2/serio/input-core）在 Debian 内核里是内建的，
# 只有 USB HID 需要模块（hid → usbhid/hid-generic，闭包自动带 usbcore）。
EXTRA_KEEP = [
    'kernel/drivers/hid/hid.ko.xz',
    'kernel/drivers/hid/hid-generic.ko.xz',
    'kernel/drivers/hid/usbhid/usbhid.ko.xz',
]


def is_keep(rel):
    return rel in EXTRA_KEEP or any(rel.startswith(p) for p in KEEP_PREFIXES)


def is_excluded(rel):
    return any(rel.startswith(p) for p in EXCLUDE_PREFIXES)


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
    """解 .deb（ar → data.tar.{xz,zst,gz}）。Debian 用 .xz，tarfile 原生支持。"""
    for name, body in ar_members(deb):
        if not name.startswith('data.tar'):
            continue
        tf = tarfile.open(fileobj=io.BytesIO(body))
        tf.extractall(dest)


def ensure_debian(dl):
    """下载并解开内核包，返回 (vmlinuz 路径, 模块根目录)。"""
    out = os.path.join(dl, 'debian/' + ABI)
    deb = os.path.join(dl, 'debian-image.deb')
    if not os.path.exists(os.path.join(out, '.done')):
        if not os.path.exists(deb):
            log('downloading %s ...' % DEB_URL.rsplit('/', 1)[-1])
            urllib.request.urlretrieve(DEB_URL, deb)
        log('extracting %s (%.1f MB) ...' % (os.path.basename(deb),
                                            os.path.getsize(deb) / 1048576.0))
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out, exist_ok=True)
        extract_deb(deb, out)
        open(os.path.join(out, '.done'), 'w').close()
    mod_root = os.path.join(out, 'lib', 'modules', ABI)
    if not os.path.isdir(mod_root):
        # Debian 是 /usr 合并布局：模块实际在 usr/lib/modules/<abi>/
        mod_root = os.path.join(out, 'usr', 'lib', 'modules', ABI)
    vmlinuz = None
    boot = os.path.join(out, 'boot')
    if os.path.isdir(boot):
        for f in sorted(os.listdir(boot)):
            if f.startswith('vmlinuz'):
                vmlinuz = os.path.join(boot, f)
    if not vmlinuz or not os.path.isdir(mod_root):
        raise RuntimeError('内核包结构不符预期: %s / %s' % (vmlinuz, mod_root))
    return vmlinuz, mod_root


def depends_of(raw):
    """读模块内嵌的 modinfo 字段 depends=（逗号分隔的模块名）。"""
    i = raw.find(b'depends=')
    if i < 0:
        return []
    j = raw.find(b'\x00', i)
    s = raw[i + 8:j if j > 0 else len(raw)].decode('ascii', 'replace')
    return [d for d in s.split(',') if d]


def stage_debian_modules(zj, mod_root):
    """把 Debian 模块装进 initramfs：**按路径白名单裁剪** + 依赖闭包；
    `.ko.xz` → `.ko.gz`（busybox modprobe 走 gzip）；自生成 modules.dep。"""
    dst = os.path.join(zj.STG, 'lib', 'modules', ABI)
    os.makedirs(dst, exist_ok=True)

    # 1) 先建索引：模块名 → (源路径, 包内相对路径)，供闭包查找用（含被裁剪掉的）
    all_files = {}
    for dp, _dn, fns in os.walk(mod_root):
        for f in fns:
            if f.endswith('.ko.xz'):
                rel = os.path.relpath(os.path.join(dp, f), mod_root).replace(os.sep, '/')
                all_files[f[:-len('.ko.xz')]] = rel
    log('debian: 包内模块总数 %d' % len(all_files))

    rels, deps, name2rel = [], {}, {}

    def write_module(rel):
        outrel = rel[:-len('.xz')] + '.gz'
        src = os.path.join(mod_root, rel.replace('/', os.sep))
        raw = lzma.open(src, 'rb').read()
        deps[outrel] = depends_of(raw)
        name2rel[os.path.basename(rel)[:-len('.ko.xz')]] = outrel
        tgt = os.path.join(dst, outrel)
        os.makedirs(os.path.dirname(tgt), exist_ok=True)
        with gzip.open(tgt, 'wb', compresslevel=9) as g:
            g.write(raw)
        zj.MODES[outrel] = 0o644
        rels.append(outrel)

    # 2) 白名单命中者（+ EXTRA_KEEP），排除清单在此直接过滤
    queue, nkeep = [], 0
    for name, rel in sorted(all_files.items()):
        if is_keep(rel) and not is_excluded(rel):
            write_module(rel)
            nkeep += 1
        else:
            queue.append(name)
    log('debian: 白名单命中 %d，其余 %d 个按需（依赖闭包）补' % (nkeep, len(queue)))

    # 3) 依赖闭包：白名单外的模块，只要是已收模块的依赖就补进来
    #    （先把已收模块的依赖收集起来，再一层层解析）
    pending = set()
    for rel in list(rels):
        pending.update(deps.get(rel, []))
    added = 0
    blocked = []
    while pending:
        name = pending.pop()
        if name in name2rel:
            continue
        rel = all_files.get(name)
        if not rel:
            continue  # 内建模块 / 不在包里
        if is_excluded(rel):
            blocked.append(name)   # 被保留模块需要，却被排除 → 构建期直接报错
            continue
        write_module(rel)
        added += 1
        pending.update(deps.get(name2rel[name], []))
    log('debian: + 依赖闭包 %d 个，共 %d 个模块' % (added, len(rels)))

    # 4) 断言（fail-fast，避免静默产出缺依赖的坏包）：
    #    a) 不能有"被排除但被保留模块需要"的模块；
    #    b) 每个保留模块的依赖必须都在包内（不在 all_files 里 = 内建，允许）。
    if blocked:
        raise RuntimeError('EXCLUDE 与依赖冲突，被保留模块需要: %s'
                           % ', '.join(sorted(set(blocked))))
    missing = []
    for rel in rels:
        for d in deps.get(rel, []):
            if d not in name2rel and d in all_files:
                missing.append('%s <- %s' % (d, rel))
    if missing:
        raise RuntimeError('依赖缺失（构建错误）: %s'
                           % ', '.join(sorted(set(missing))[:10]))

    # modules.dep（必须**二进制**写：见 build-ubuntu-rescue.py 里的注释，\r 会让
    # busybox modprobe 报 "not found in modules.dep"）
    with open(os.path.join(dst, 'modules.dep'), 'wb') as fh:
        for rel in sorted(rels):
            paths = ' '.join(name2rel[d] for d in deps.get(rel, [])
                             if d in name2rel)
            fh.write(('%s:%s\n' % (rel, (' ' + paths) if paths else '')).encode('ascii'))
    zj.MODES['lib/modules/%s/modules.dep' % ABI] = 0o644
    for extra in ('modules.builtin', 'modules.order'):
        s = os.path.join(mod_root, extra)
        if os.path.exists(s):
            shutil.copy2(s, os.path.join(dst, extra))
            zj.MODES['lib/modules/%s/%s' % (ABI, extra)] = 0o644
    return len(rels)


def main():
    zj = load_alpine_builder()
    dl = os.path.join(HERE, 'vmtest', 'dl')
    os.makedirs(dl, exist_ok=True)
    kernel, mod_root = ensure_debian(dl)

    if os.path.isdir(zj.STG):
        shutil.rmtree(zj.STG)
    os.makedirs(zj.STG)

    # 1) 用户态（与 Alpine/Ubuntu 版完全一致）
    for apk in zj.APKS:
        p = os.path.join(zj.DL, apk)
        if not os.path.exists(p):
            log('!! missing %s（先跑一次 build-alpine-initramfs.py 让它下载，或手动放进 dl/）' % apk)
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

    # 2) Debian 签名模块（存储子集 + 依赖闭包）
    nmod = stage_debian_modules(zj, mod_root)

    # 3) 我们的脚本与引导代码块
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

    # 4) 内核（Debian 签名）
    shutil.copy2(kernel, os.path.join(zj.BOOT, 'vmlinuz-zjrestore'))

    # 5) 打包
    out = os.path.join(zj.BOOT, 'initramfs-zjrestore.cpio.gz')
    n, rawlen = zj.pack_cpio(zj.STG, out)
    log('initramfs: %d entries, %d bytes raw, %d bytes gz' %
        (n, rawlen, os.path.getsize(out)))
    log('modules  : %d 个（%s）' % (nmod, ABI))
    log('vmlinuz  : %d bytes (debian-signed %s)' %
        (os.path.getsize(os.path.join(zj.BOOT, 'vmlinuz-zjrestore')), ABI))
    return 0


if __name__ == '__main__':
    sys.exit(main())
