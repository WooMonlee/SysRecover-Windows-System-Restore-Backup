# SysRecover（知鉴一键还原 · 单机版）

Windows 系统备份 / 一键还原工具。**CLI 优先**（`SysRecover.exe`），另有 GUI（`SysRecoverUI.exe`），
双 exe 共享同一套 `SysRecoverCore` 代码；**零运行时依赖**、Release x64、体积目标 < 10 MB。

> ⚠️ 本仓库是**独立产品（单机版）**。早期的 `WooMonlee/OnekeyRestore`（C# + VHDX 多点秒还原）
> 是**另一个产品**，两边的设计与实现分开推进、文档不混用。
>
> 👋 **第一次进来先看 [`docs/00-项目简介（给协作者）`](docs/00-项目简介（给协作者）.md)** ——
> 一页速览：这是什么、现在到哪一步、目录怎么读、待办有哪些。

## 它怎么工作

1. **备份**：在 Windows 里热备（VSS 快照）系统分区为 WIM/ESD。
2. **还原**：选镜像 + 目标分区 → 自动二选一：
   - **就地还原**（目标分区没被占用：在 PE 里、或还原到非系统盘）→ 格式化 + `libwim` 应用 +
     `bcdboot` 修引导，**不重启**；
   - **重启还原**（还原正在运行的系统盘）→ 暂存任务 → 重启进内置 Linux 救援层 →
     格式化 + 应用镜像 + 修复引导 → 自动重启回新系统。

## 环境支持（均已实测）

| 环境 | 状态 |
|---|---|
| BIOS / MBR | ✅ |
| UEFI / GPT（Secure Boot 关闭） | ✅ |
| UEFI / GPT + **Secure Boot 开启** | ✅ **零注册、零交互** |
| WinPE / 还原到非系统盘 | ✅ 就地还原（不重启） |
| Windows 7 / 10 / 11 | ✅（Win7 需 UCRT，见「已知事项」） |

## 构建

```bash
mingw32-make -f Makefile all        # CLI + GUI
mingw32-make -f Makefile package    # 再部署 bootfiles/皮肤 + 许可声明到 dist/
mingw32-make -f Makefile clean
```

- 工具链：**MinGW-w64 GCC 14.2**（路径写死在 `Makefile` 头部，其余全相对路径）
- 救援层组装：`tools/build-ubuntu-rescue.py`（**Ubuntu 签名内核 + 签名模块** + Alpine 用户态）
- 回归测试：`tools/vmtest/*.ps1`（见 `tools/vmtest/README.md`）

## 发布包（`dist/`，约 54 MB）

```
SysRecover.exe / SysRecoverUI.exe / libwim-15.dll
bootfiles/{grldr, grldr.mbr, vmlinuz-zjrestore, initramfs-zjrestore.cpio.gz, zjrestore-lite.sh}
bootfiles/sb/{shimx64.efi, grub-ubuntu.efi, grub.cfg}     # Secure Boot 链
skin/ resources/ version.json THIRD_PARTY_LICENSES.txt
```

## 引导设计（细节见 AGENTS.md §7）

| 环境 | 链 |
|---|---|
| BIOS/MBR | `MBR → bootmgr → BCD(bootsector \grldr.mbr) → \grldr → \menu.lst → 内核 + initramfs` |
| UEFI（SB 关） | `固件启动项 Boot####（NVRAM）直启内核`，命令行走 OptionalData |
| UEFI + SB 开 | `固件 → shimx64.efi（微软签名）→ grubx64.efi（Canonical 签名的 GRUB）→ Canonical 签名的 Ubuntu 内核 + 我们的 initramfs` |

> Secure Boot 那条链**全部使用已签名的现成组件**（不绕过任何机制），因此无需用户注册密钥、
> 无需关闭 Secure Boot；相关踩坑与对比见 `AGENTS.md` PIT-060/061/062/063/065/066。

## 许可

- 本产品自身代码**未静态链接任何 GPL 组件、未修改任何第三方源码**（`libwim-15.dll` 为
  LGPL 动态链接，其余为「单独分发」的聚合），因此不受 copyleft 的衍生作品条款约束。
  **是否开源为待决事项**，与合规无关。
- 第三方组件清单、用法与许可全文：`THIRD_PARTY_LICENSES.txt`（随包分发）。
- 构建期工具（MinGW-w64、osslsigncode、QEMU/OVMF、mtools）**不随产品分发**。

## 已知事项 / 待完善

- **Windows 7** 需要 UCRT：`libwim-15.dll` 与我们的 exe 都依赖 `api-ms-win-crt-*`（Win10 内置、
  Win7 没有）。修法：把 Windows SDK 的 `Redist\ucrt\DLLs\x64\`（`ucrtbase.dll` +
  `api-ms-win-crt-*.dll`，约 1.5MB）随包带上。
- **服务器 RAID 驱动**（`vmd`/`megaraid`/老 `mpt*`/`isci`）在 Ubuntu 的 `linux-modules-extra`
  里，需挑子集补进 initramfs（基础包已覆盖 NVMe/AHCI/virtio/USB）。
- `ZJ_ENABLE_MOK_PATH`（备选线：我们签名的 UKI + MOK 注册）默认**不编译、不随包**，
  `src/boot/uefi.cpp` 里改成 1 即可启用。
