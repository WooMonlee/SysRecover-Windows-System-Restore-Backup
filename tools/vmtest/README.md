# SysRecover QEMU 测试台（tools/vmtest）

> 2026-09-09 搭建；2026-09-17 换 Alpine 底座；2026-09-18 加"还原演练"与
> "PBR 探针"两项自动测试。
> 环境：MSYS2（`D:\Prog\ProgIDE\msys64`）+ QEMU 11.1.1（mingw64）+ mtools + Python。

## 目前验证到哪一步

✅ GRUB4DOS 真实引导链：grldr.mbr → grldr → menu.lst → vmlinuz+initramfs
✅ 救援层（Alpine linux-lts 6.6.142 + 自写 `/init`）：控制台**有输出**（VGA/串口都行），
   存储驱动覆盖 SATA/AHCI/NVMe/UFS/MMC/各厂商 HBA/虚拟化（PIT-044~048）
✅ **还原演练**（`mk-drill.sh` + `run-drill.ps1`）：找日志 → 找镜像 → mkntfs 快格 →
   wimlib apply → bootfix → 写微软引导区 → reboot，全链绿
✅ **PBR 探针**（`pbr-probe.py`）：证明「mkntfs 分区 + 完整微软引导区」能引导 Windows
   （屏幕出现 `BOOTMGR is missing`），纠正 PIT-052 的误诊，见 PIT-053
✅ **UEFI/GPT 引导分支**（PIT-060/061）：固件启动项 `Boot####`（NVRAM）直接加载内核
   +`initrd=`，端到端已在真机/VM 验证（还原成功自动重启进新系统）
⬜ Windows 基像自动化（目前用手工 VMware VM 验证真实还原）

## 手工测试 VM（真实还原）

| 项 | 值 |
|---|---|
| **UEFI/GPT（主用）** | `D:\Software\System\_VMware\VMdisks\初心Win10x64`（`firmware="efi"`、NVMe、1 快照） |
| BIOS/MBR | `...\Win10New`（NVMe） |

> 虚拟 NVMe 上大镜像还原会每 ~15% 卡一下（`nvme ... timeout, completion polled`），
> 根因是**虚拟 NVMe 偶发完成中断丢失**，卡顿时长 = `nvme_core.io_timeout`（默认 30s）。
> 修复：把它降到 **1 秒**（`modprobe nvme-core io_timeout=1` + sysfs + cmdline），见
> AGENTS.md **PIT-055**。压小脏页阈值**无效**（已回退）。

## UEFI 自动回归（QEMU + OVMF）

| 脚本 | 验什么 |
|---|---|
| `uefi-smoke.ps1` | UEFI Shell 以 `initrd=` 启动内核 → 救援 `/init` 跑起来（`SR: modules loaded`） |
| **`uefi-ubuntu-smoke.ps1`** | ⭐ **当前 SB 链（PIT-066 变体 D）**：`shim → Canonical 签名的 GRUB → Canonical 签名的 Ubuntu 内核 + 我们的 initramfs` 能起来（含模块加载、磁盘枚举）。资产从 `bootfiles/sb/` 取 |
| `uefi-bootentry-smoke.ps1` | 固件从 `Boot####` **直接加载并启动**我们的内核（SB 关闭时走的路径） |
| `uefi-screen.ps1` | UEFI 下**屏幕有输出**（抓两次截图比对；坏了就是一片黑 = 用户以为死机，PIT-061） |
| 🅿️ `uefi-uki-smoke.ps1` | （备选线）我们签名的 **UKI**（systemd-stub）能把 initrd 交给内核。**需要先 `make package` 生成 UKI**；`ZJ_ENABLE_MOK_PATH=0` 时不再随包 |
| 🅿️ `uefi-shim-smoke.ps1` | （备选线）**shim 加载我们的 UKI**（同上，需 UKI 存在） |

```powershell
powershell -File uefi-ubuntu-smoke.ps1     # 当前主线
powershell -File uefi-smoke.ps1
powershell -File uefi-bootentry-smoke.ps1
powershell -File uefi-screen.ps1
# 备选线（需先构建 UKI）： powershell -File uefi-shim-smoke.ps1
```

> ⚠️ 这些脚本（以及 `tools/ui/*.ps1`）**必须保持纯 ASCII**：PowerShell 5.1 对无 BOM 的
> `.ps1` 按 ANSI 解析，中文注释会把解析器打挂（症状很怪，比如某个变量莫名其妙是空的）——
> 本仓库已经踩过两次，见 AGENTS.md §15。

## 还原演练（drill）

```bash
cd /d/Prog/_Project/SysRecover/tools/vmtest
./mk-drill.sh                       # 重建 base/drill.raw（2GB，3 分区）
powershell -File run-drill.ps1 -Secs 220
# 成功标志：日志末尾 "reboot: Restarting system"（-no-reboot → QEMU 自行退出）
```

`base/drill.raw` 布局（扇区）：sda1 2048..206847（FAT32 引导链）、
sda2 206848..1606847（**还原靶子**，故意先用 FAT32 格式化 → 走 mkntfs 路径）、
sda3 1606848..（FAT32，放 `images/test.wim` + `restore-task.conf`）。

可选环境变量：

| 变量 | 作用 |
|---|---|
| `ZJ_DRILL_WIM` | 换用别的 WIM（如 `drill-images/nobm.wim`：只有一个标记文件、**没有 bootmgr**） |
| `ZJ_DRILL_NOBOOTFIX=1` | 完全不注入 `bootfix/`（配合上面的 WIM 做 PBR 探针） |

> 坑：`restore-task.conf` 的 `target_offset` 是**字节**（`target_part_offset` 同理）。
> 早期 mk-drill 注入的是扇区号，导致 `mkntfs --partition-start` 写成 404，
> 引导代码按错误绝对偏移读盘 → 演练根本验证不了引导。已修。

### PBR 探针（回归测试：mkntfs 能不能引导 Windows）

原理：造一个「mkntfs + 完整微软引导区 + 根目录没有 bootmgr」的分区，
用 GRUB4DOS `chainloader` 它。引导代码若能解析这个卷，必然报
`BOOTMGR is missing / Press Ctrl+Alt+Del to restart`。

```bash
# 准备（无 bootmgr 的靶子）
ZJ_DRILL_WIM=drill-images/nobm.wim ZJ_DRILL_NOBOOTFIX=1 ./mk-drill.sh
powershell -File run-drill.ps1 -Secs 220
# 探针（自己写 chainload 菜单、启动、读 VGA 文本、断言）
python pbr-probe.py        # PASS / FAIL，退出码 0/1
```

`nobm.wim` 用项目 CLI 造：`SysRecover.exe backup --source <只含标记文件的目录>/ --dest ...`

## 截屏 / 读屏（GUI、BIOS、引导代码阶段）

内核无输出的老问题已随 Alpine 底座消失，但 BIOS/GRUB/PBR 这些阶段仍然只能靠屏幕。
两种手段（都走 `-monitor tcp:127.0.0.1:<port>,server,nowait`）：

```python
# 1) 抓图（PPM → 自己转 PNG；本机没有 PIL）
f.write(b'screendump base/shot.ppm\n')
# 2) 读文本：把 VGA 文本缓冲（0xB8000, 80x25x2B）直接落盘再解析
f.write(b'memsave 0xb8000 4000 base/vga-text.bin\n')
```

> **不要用 `xp /4000cb 0xb8000`**：monitor 会逐字符回显命令行，输出里混杂大量
> 转义序列，解析不可靠（踩过）。`memsave` 干净可靠。
> 也不要指望 PIL：本机 Python 没有，转 PNG 用手写 zlib+struct（见 `pbr-probe.py`）。

## 目录

```
base/drill.raw        2GB 还原演练盘（mk-drill.sh 生成；gitignore）
base/testdisk.raw     512MB 引导链测试盘（mk-testdisk.sh）
base/bootfix/         演练用的假 bootfix（bootmgr/BCD/fonts）
drill-images/         演练镜像（test.wim 正常流程；nobm.wim 探针用）
logs/                 每次 QEMU 启动的 -nographic 全量输出
mk-drill.sh           重建演练盘
mk-testdisk.sh        重建引导链测试盘
run-drill.ps1         跑一次演练（PowerShell：能可靠 kill 原生 qemu）
pbr-probe.py          PBR 探针回归测试
uefi-smoke.ps1                UEFI(OVMF) 冒烟：内核+initrd 能起救援
uefi-bootentry-smoke.ps1      UEFI：固件从 Boot#### 直接启动内核
uefi-screen.ps1               UEFI：屏幕有输出（黑屏回归）
build-alpine-initramfs.py  从 Alpine apk 组装 vmlinuz+initramfs（纯 Windows）
dl/                   下载的 Alpine 包（gitignore）
```

## 历史坑位

见 `AGENTS.md` PIT-013~014（BG-Rescue 旧底座，已作废）、PIT-020/021（分区表顺序、
cpio newc 头字段）、PIT-044~048（Alpine 底座）、PIT-049~053（NTFS 引导区）、
PIT-055（NVMe 卡顿）、PIT-056/057（注册表事务日志、半截镜像）、PIT-058/059（部署布局）、
PIT-060/061（UEFI 固件启动项、UEFI 黑屏）。
