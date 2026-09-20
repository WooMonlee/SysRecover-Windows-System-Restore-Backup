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
| Windows 7 / 10 / 11 | ✅（Win7 装 VC++ 运行库后正常；**已实测 Win7 宿主还原 Win10 镜像并正常启动**） |
| **架构** | **仅 64 位（x64）** —— 32 位 Windows / 32 位 PE 上 exe **根本起不来**（系统报"不是有效的 Win32 应用程序"，程序内无法提示）；需要时另出 x86 版，见「已知事项」 |

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

## 使用案例（命令行）

> CLI 与 GUI 走**同一套 `app` 层代码**（`RunBackup` / `StageRestore`），行为一致。
> CLI 已编入 `requireAdministrator` 清单：在管理员命令行 / 计划任务（最高权限）/ PsExec `-s` /
> SCCM 下**全程静默不弹 UAC**（机房批量部署即用这条路）。

先看清现场（只读，随时可用）：

```cmd
SysRecover.exe list      :: 磁盘/分区/文件系统/盘符/ESP/系统标记
SysRecover.exe diag      :: 固件类型、Secure Boot 状态、启动项是否已装、wimlib 自检
```

### 案例 1 · 把当前系统备份成镜像（热备份）

```cmd
SysRecover.exe backup --dest D:\backup\win10-20260920.esd --source C:/ ^
                      --compress recovery --verify --name "Win10 出厂态"
```

- `--source C:/`：**盘符根 + 正斜杠** → 触发热备（VSS 快照 + 排除清单）
- `--compress`：`recovery`（.esd 最省）/ `maximum` / `fast`
- `--verify`：写完立即校验；`--name`：子镜像名
- 目标已存在需 `--yes` 覆盖，或用 `--append` 追加为同一 WIM 里的新子镜像
- 辅助：`images --image <镜像>` 列子镜像；`verify --image <镜像>` 单独校验

### 案例 2 · 还原一个万能镜像到 C 盘

```cmd
SysRecover.exe list                                       :: 先确认磁盘号/分区号
SysRecover.exe restore --image D:\backup\wannei-win10.esd ^
                       --disk 0 --part 3 --index 1 --yes
```

执行方式**自动二选一**：

| 情形 | 行为 |
|---|---|
| 目标是**正在运行的系统盘** | 暂存任务 → **重启**进内置救援层 → 格式化 + 应用 + 修引导 → 自动重启回新系统 |
| 目标**未被占用**（在 **PE** 里、或还原到**非系统盘**） | **就地还原**：格式化 + 应用 + `bcdboot` → **完成，不重启** |

- 安全四检查（目标是 ESP / BitLocker / 恢复分区 / 镜像在目标分区内）**任一命中即拒绝**（退出码 4）
- 不自动修引导：`--no-repair-boot`；指定子镜像：`--index N`
- 退出码：`0` 成功 / `2` 参数错 / `3` 需管理员 / `4` 危险目标被拒 / `5` 镜像校验失败 / `6` 取消

### 案例 3 · 给已有镜像做"无人参与的静默还原"入口

**思路**：把还原任务**暂存**下来（并装好常驻引导模块），之后由**开机菜单选择**或**单次启动**
触发，全自动完成还原，**不需要任何人点确认**。

```cmd
:: 1)（推荐）先用 GUI 的「安装启动还原」装一次常驻引导模块 —— 开机启动菜单里就会多出该入口
::    （UEFI：写固件启动项；BIOS：BCD 实模式启动扇区条目）

:: 2) 暂存一次静默还原：安检 → 镜像可用性校验 → 写契约 → 刷新引导层 → 设单次启动
SysRecover.exe restore --image D:\backup\wannei-win10.esd --disk 0 --part 3 --index 1 --yes

:: 3) 重启（这之后无需任何操作）
shutdown /r /t 0
```

- **单次语义**：那条"单次启动"用完即消，平时开机照常进 Windows ✓
- **常驻语义（UEFI）**：`安装启动还原` 写入的**固件启动项**挂在 `BootOrder` 末尾，
  开机启动菜单里随时能选到；而任务契约 `restore-task.conf` 留在**数据盘**（不被格式化），
  所以**之后再选它还会再还原一次** —— 这就是"菜单里常驻的一键还原" ✓
- ⚠️ **BIOS 下不常驻**：救援文件（`grldr`/`grldr.mbr`/`menu.lst`）随目标分区一起被格式化，
  所以那条菜单项**只对当次有效**；要常驻请用 UEFI（或把救援文件放到不被格式化的分区，当前未实现）
- GUI 勾上 **`静默模式`** 后**全程无任何对话框**（机房批量正为此设计）
- 等价做法：双击 GUI → 选镜像 → 选目标分区 → 勾「静默模式」→ 开始恢复系统

## 许可

- 本产品自身代码**未静态链接任何 GPL 组件、未修改任何第三方源码**（`libwim-15.dll` 为
  LGPL 动态链接，其余为「单独分发」的聚合），因此不受 copyleft 的衍生作品条款约束。
  **是否开源为待决事项**，与合规无关。
- 第三方组件清单、用法与许可全文：`THIRD_PARTY_LICENSES.txt`（随包分发）。
- 构建期工具（MinGW-w64、osslsigncode、QEMU/OVMF、mtools）**不随产品分发**。

## 已知事项 / 待完善

- **Windows 7** 需要 UCRT：`libwim-15.dll` 与我们的 exe 都依赖 `api-ms-win-crt-*`（Win10 内置、
  Win7 没有）。**2026-09-20 实测**：在那台 Win7 上装 **VC++ 2015-2022 x64 运行库**（或
  `Windows6.1-KB2999226-x64.msu`）后运行正常，**并已用它还原 Win10 镜像、正常启动** ✓。
  **机制已就绪**：把 UCRT 的 `ucrtbase.dll` + `api-ms-win-crt-*.dll`（约 1.5MB）拷进
  `third_party/ucrt/x64/`，`make package` 会自动复制到 `dist/` 与 exe 同目录（微软官方支持的
  本地部署方式）。取法与授权见 `third_party/ucrt/README.txt`。
- **服务器 RAID 驱动**（`vmd`/`megaraid`/老 `mpt*`/`isci`）在 Ubuntu 的 `linux-modules-extra`
  里，需挑子集补进 initramfs（基础包已覆盖 NVMe/AHCI/virtio/USB）。
- `ZJ_ENABLE_MOK_PATH`（备选线：我们签名的 UKI + MOK 注册）默认**不编译、不随包**，
  `src/boot/uefi.cpp` 里改成 1 即可启用。
