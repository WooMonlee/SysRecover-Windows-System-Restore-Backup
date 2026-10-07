# AGENTS.md - SysRecover（一键还原，C++ 重写）开发指南

> 🆕 **第一次接手 / 不知道从哪看起？先读 [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md)**
> —— 现状（已验证 vs 待验证）、下一步优先级、文档地图、以及"AI 环境丢了怎么续上"。
>
> 🆕 **换机器 / 新环境？先看 [`docs/10-新环境交接说明.md`](docs/10-新环境交接说明.md)**
> —— 里面是：**复制哪些文件夹**、**必须保持一致的绝对路径**（工具链/QEMU 写死在 Makefile 与 tools 里）、
> 新环境开工三步（编译 → `diag` 自检 → QEMU 回归），以及**已踩过的坑**。
> 本文件继续作为**操作手册**（§0 红线 / §7 引导 SOP / §13 坑位册）。

> 面向 AI Agent 和开发者。地位：操作手册（怎么做）。路线图见 PLAN.md，Linux 引导契约见 docs/boot-contract.md（如缺则以本文件 §5 为准）。
> 文档版本：v2.1（C++ 新项目 + Linux 层定稿）/ 最后更新：2026-09-15 / 维护者：DreamGrain
> 旧 C# 原型（`D:\Prog\_Project\SysRestore\src/`）已冻结，仅作设计蓝本，不移植代码。

---

## 0. 开工前必查（5 条，进任务前必读）

1. PE 红线：读分区/磁盘信息**禁用 WMI**（PE 不可用），只用 Win32 API；设引导（BCD）只在正常 Windows 执行。
2. BCD 红线：严格按 §7 成熟方案（`\grldr.mbr` + `/application bootsector` + GUID 回显判断），严禁自行发明。
3. License 红线：只动态链接 `libwim`（LGPLv3）；**严禁抄 `wimlib-imagex`（GPLv3）源码、grub4dos 源码、Dism++ 主程序**（闭源）。
4. 构建红线：钉版工具链 + Release x64 + 零依赖验证 + 体积门禁（§3）。
5. 契约红线：改 `restore-task.conf` / `progress.json` / `_zjresy*.log` 字段必须双端（Windows + Linux）同步发版。
6. 国际化红线：**新字符串必须 `Tr()`（日志/契约/救援层/品牌名除外），译文只能改 `tools/i18n-en.py` 再 `--gen-lang` 生成，改完必跑 `make check`**（`tools/check-i18n.py` 会拦漏翻、死键、printf 占位符错位、`.lang` 不同步）—— 详见 §18。

---

## 1. 项目快照

- **产品**：九转还原（单机版）/ SysRecover，原生 C++，零运行时依赖，目标体积 **< 10 MB**。
- **用户**：电脑维修人员、系统维护人员、IT 管理员。
- **运行矩阵**：Windows 7 / Windows 10 / Windows 11 / WinPE，其中**主用 Windows 10**。
- **架构**：Windows C++ 前端（备份/任务暂存/引导配置） + Linux initramfs 恢复层（实际执行还原）。
- **策略**：**CLI 优先**（Phase 0–4 唯一交付物 `SysRecover.exe`，console 子系统），GUI 后做（Phase 5 才加 `SysRecoverUI.exe`，windows 子系统，双 exe 共享 `SysRecoverCore.lib`）。

### 技术栈决策（不再讨论）

| 问题 | 方案 | 原因 | 已验证 |
|---|---|---|---|
| 语言/GUI | C++ / Win32 / Duilib 系（推荐 `nim_duilib` MIT） | 一键还原主流是 C++/Delphi；WinPE 默认无 .NET；C# 自包含 148MB vs Dism++ 3.5MB | ✅ 2026-09-05 |
| WIM | 直接链接 `libwim-15.dll`（LGPLv3） | 根治"调 exe + 解析文本"的编码/进度/退出码坑 | ✅ 方案确定 |
| 磁盘枚举 | `DeviceIoControl` + `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` | PE 可用；旧 C# 的 P/Invoke 签名可直接翻译 | ✅ 原型验证 |
| 引导 | BCD（bcdedit）+ GRUB4DOS（grldr.mbr） | 2026-09-05 已验证打通，见 §7 | ✅ 2026-09-05 |
| 恢复执行 | Linux（**Alpine linux-lts 6.6 底座**：自带全套存储模块 + `bootfiles/alpine/init` 自写 PID1 + `zjrestore-lite.sh`） | 体积小、速度快、**硬件覆盖面广**（SATA/NVMe/mpt3sas/mptspi/megaraid/virtio/USB）；**禁用 WinPE/WinRE**；2026-09-17 QEMU 全链验证通过 | ✅ 2026-09-17 |

同类调研：Dism++ = C++（`.vcxproj`，CBS API）；易数一键还原 = DiskGenius 内核（Delphi）；**Dism++ 主程序闭源**（官方 issue #548 明确不开源程序本身），不可抄袭。

---

## 2. 硬禁令清单（进 PR 必勾）

- [ ] 重启类还原/备份**必须走 Linux**，禁用 WinPE/WinRE。
- [ ] 读磁盘/分区信息**禁用 WMI**（PE 不可用），只用 §6 白名单 API。
- [ ] BCD 必须用 §7 的 5 条成熟命令，`path` 必须是 `\grldr.mbr`，必须带 `/application bootsector`。
- [ ] `libwim` 只动态链接，**不抄** `wimlib-imagex` 源码；GRUB4DOS 只分发 `grldr/grldr.mbr` 二进制，不并入源码。
- [ ] GUI 只用 BSD/MIT 库（Duilib / SOUI / nim_duilib），不引入 GPL 界面库。
- [ ] 不抄 Dism++ 主程序、不抄来路不明的博客代码。
- [ ] **每次修改都 `python tools/version.py --bump`**（★ **改完立即，不等提交**；纯文档/纯重构除外）——
      保证**每次交付/编译的版本号都不同、可区分**；**改完代码、编译打包前必须执行**，并核对
      `src/common/version.h` 与 `dist/version.json` 已同步（详见 `PLAN.md`「版本号规则」）。

---

## 3. 工具链与构建发布（钉版；唯一允许的绝对路径是 MinGW 本体）

| 项 | 要求 |
|---|---|
| 编译器 | **MinGW-w64 GCC 14.2.0**（`D:\Prog\ProgIDE\mingw64\bin\g++.exe`，x86_64-w64-mingw32，posix-seh），`_WIN32_WINNT=0x0601`（Win7 兼容） |
| 构建 | `mingw32-make` + 手写 `Makefile`（无 CMake；make 来自 `D:\Prog\ProgIDE\mingw64\bin\mingw32-make.exe`），禁止写死其他绝对路径 |
| 配置 | **Release + x64**（另需 x86 构建时再加）；`-O2`；静态链接优先 **`-static`**（PE 零依赖，MinGW 无 `/MT` 概念）；子系统按 exe 区分（CLI=`-mconsole`，GUI=`-mwindows`）；提权清单用 windres 编入 **GUI 与 CLI 两个 exe**（`src/gui/SysRecoverUI.rc` / `src/cli/SysRecover.rc` + 同名 `.manifest` → `build/*_rc.o`，见 PIT-018） |
| libwim 引入 | 预编译 `wimlib.h + libwim.lib + libwim-15.dll` 放 `third_party/wimlib/`；运行时 DLL 与 EXE 同目录分发；**严禁静态链接 libwim，严禁抄 `wimlib-imagex.c`** |
| Duilib 引入 | **已换库：经典 `Duilib`（MIT/BSD）**，`third_party/duilib-master/`（35 cpp，静态库 `build/libduilib.a`）。原因：`nim_duilib` 运行时强制 Skia（`GlobalManager` 无 GDI 回退），Skia 体积违背 <10MB 目标，弃用（源码留存 `third_party/nim_duilib-main/` 不再编译）；MinGW 移植补丁见 PIT-012；XML 皮肤随包 `dist/skin/`，禁止依赖外部散文件 |
| 构建命令 | **`mingw32-make -f Makefile package`** = ★**双架构发布包**（启动器 + `dist/x86` + `dist/x64` + 共享资源；需两套工具链）/ `... all`（仅 x64 → `dist/x64`）/ `... ARCH=x86 all`（仅 x86 → `dist/x86`）/ `... check`（单元测试，零依赖 `tests/`）/ `... clean`（Makefile 写死两套 MinGW 路径，其余用相对路径）|
| 救援层构建 | `python tools/build-debian-rescue.py`（**当前**：Debian 签名内核 + 存储子集模块 + Alpine 用户态；纯 Windows/Python，无需 Linux 环境）。用户态组装复用 `tools/vmtest/build-alpine-initramfs.py`（从 `tools/vmtest/dl/alpine/*.apk`），见 PIT-044/045/046、PLAN §11 |
| 输出物 | `SysRecover.exe` + `libwim-15.dll` + `boot/{vmlinuz,initramfs,restore.sh,grldr,grldr.mbr,menu.lst模板}` + `version.json` + SHA256 |
| 门禁 | 体积检查（见 PLAN.md §1 体积目标 < 10 MB）+ `objdump -p` / `x86_64-w64-mingw32-objdump` 或 Dependencies 零依赖检查 + `diag` 自检通过 |

版本号：SemVer `主.次.修订` —— **唯一来源 `src/common/version.h`**（`SYSRECOVER_VERSION`），
`version` 命令、`_zjresy*.log` 的 `software_version`、GUI 标题栏副标题、`dist/version.json` 全部由它派生。
**规则见 `PLAN.md`「版本号规则」**：主/次版本**由用户指定**（`python tools/version.py --set X.Y.Z`），
**修订号每次修改都 +1** —— ★ **改完立即跑 `python tools/version.py --bump`，不等提交**，
保证每次交付/编译的版本号都不同、可区分；**AI 改完代码、编译打包前必须执行**。发布时与 git tag `vX.Y.Z` 对齐。

> **架构（2026-09-24 方案 D）：Windows 侧位数跟随系统**（32 位系统跑 x86、64 位系统跑 x64，主要为备份压缩速度）。
> 发布形态 = **根目录 x86 整套（入口）+ `x64/` x64 整套 + 根目录共享资源**；**无独立启动器** ——
> 32 位程序在 64 位系统上由 `src/common/selfarch.cpp::ReexecX64IfNeeded()` **自举成 `x64\同名`** 再跑
> （在 `main()`/`WinMain()` 最开头调用；父进程已提权，`CreateProcess` 子进程不会再弹 UAC，PIT-082）。
> `ExeDir()` 在 exe 位于 `x64/` 子目录时自动上移一级（`bootfiles/`、`skin/` 放根目录）。
> **Linux 救援层固定 x86_64**（与宿主位数无关）。背景：2026-09-20 用户实测 32 位 Win7 上 x64 exe 直接起不来
> （系统层面拒绝、程序内无法提示）。四样前置：① i686 工具链 ✅（winlibs **i686 UCRT** GCC 14.2.0，
> `D:\Prog\ProgIDE\mingw32`）；② 官方 32 位 `libwim-15.dll` ✅（`third_party/wimlib/x86/`，1.14.5）；
> ③ Makefile `ARCH=x86` ✅；④ **x86 版 UCRT** ✅（`third_party/ucrt/x86/`，与 `x64/` 各 16 个文件）。
> 代价：32 位系统上备份压缩慢（`fast`≈0~10%，`recovery`≈20~35%），**还原 0%**。详见 `docs/08` §0。
> **⚠️ WOW64 坑（必记）**：32 位 exe 在 64 位 Windows 上，`GetSystemDirectoryW` 的**字面量仍是 `System32`**，
> 但 32 位进程访问它会被重定向到 `SysWOW64` —— 而 `bcdedit`/`bcdboot`/`manage-bde` **只在原生 System32**
> （实测 SysWOW64 里没有）→ 写 BCD / 修引导会"找不到文件"。必须用 `%windir%\Sysnative` 取原生 System32
> （`src/common/process.cpp::SysToolPath` 已处理；`diag` 的 `proc=/tools:` 行可自检）。
> **救援层与宿主位数无关**（Linux 侧照旧）；但 **UEFI 引导资产是 x64**（`shimx64.efi` + 64 位内核），
> 32 位 UEFI（IA32）需另找 `shimia32.efi` + 32 位内核 —— 极少见，32 位机器基本都走 **BIOS + GRUB4DOS** ✓
> （Win7 x86 **不支持 UEFI**，必然走 BIOS/MBR，正好是我们最成熟的路径）。

---

## 4. 项目结构（CLI 优先）

```
Makefile / README / AGENTS.md / PLAN.md
third_party/wimlib/      # 只放 wimlib.h + libwim-15.dll（MinGW 直连 DLL，无需 .lib），不放源码
src/common/              # Result<T>, Logger, ProcessRunner(CreateProcessW), SingleInstance, PathUtf16, IniConf
src/disk/                # DiskEnumerator, VolumeMapper, PartitionStyle（禁 WMI）
src/wim/                 # WimEngine（libwim RAII 封装，不向外泄漏 wimlib.h）
src/boot/                # BcdEditor(bcdedit 封装), Grub4dosDeployer, RestoreTaskWriter
src/app/                 # BackupOrchestrator, RestoreOrchestrator, SafetyChecks
src/cli/                 # main.cpp + list/backup/restore/verify/images/diag/version
src/gui/                 # Phase 5 才启用（Duilib 窗口 + 工作线程）
bootfiles/               # grldr/grldr.mbr/menu.lst 模板/vmlinuz+initramfs 引用（只读资产）
tools/pe-diag/  dist/  docs/
```

依赖 DAG：`common` 被所有人依赖，不依赖任何人；`boot` 依赖 `disk+common`，不依赖 `wim`；`app` 编排 `disk+wim+boot`；`cli/gui` 只调 `app`。

三条禁令：禁 WMI 读盘；禁抄 `wimlib-imagex` 源码；禁 GUI 直调 `wim`（必须经 `app`）。

---

## 5. 跨层契约（冻结，改字段需双端发版，`contract_version=1`）

Windows C++ 层 ↔ `restore-task.conf` ↔ Linux `restore.sh`。C++ 侧只许调用、不许单方面改字段；改 Linux 侧必须同步升版本号。

| 文件 | 位置 | 用途 |
|---|---|---|
| `restore-task.conf` | 软件目录 | 还原任务参数（key=value：action/pt_type/image_part_guid/image_rel_path/image_path/image_index/esp_index/target_guid/target_offset/target_size/target_disk_serial/repair_boot/partition_count/**contract_version**；`esp_index`=镜像里 ESP 子镜像的 index，0/缺省=没有——增量键，不升 contract_version；`ea_index`=EA 修复子镜像（ZJEA）的 index，同上——增量键） |
| `restore-task.json` | 软件目录 | 同上 JSON 形态 |
| `progress.json` | logs 目录 | 实时进度（Phase/Percent/Status/Detail/UpdatedAt/Pid，供 AI/外部工具读） |
| `_zjresy*.log` | **目标分区根**（C:） | 主发现契约（由 `WriteRestoreLog` 写）：action=restore / log_time / software_version / software_path / target_disk_name / target_disk_serial / target_disk_size / target_part_offset / target_part_size / target_fs / target_vol_label / image_path / image_index / esp_index / ea_index / repair_boot / pt_type / **contract_version**。**救援层启动即核对**：`get_task contract_version`（缺省视为 1）≠ `zjrestore-lite.sh` 里的 `ZJ_CONTRACT` 常量 → **报错退出、不碰目标分区**（G1 防版本错配） |
| `menu.lst` | 恢复分区根（D:\） | GRUB4DOS 菜单（`kernel` + `initrd`，不是 GRUB2 的 `linux`） |
| `grub.cfg` | GPT/UEFI 用 | GRUB2 菜单（UEFI 分支） |

> **Phase 5 新增字段**（`restore-task.conf` + `_zjresy*.log`）：`target_disk_name`（如 `INTEL SSDSCKGF240A5H REF`）、`target_disk_size`（字节）、`target_vol_label`（如 `Windows`）、`target_fs`（如 `NTFS`）、`software_path`（`SysRecover.exe` 完整路径）、`software_version`（语义化版本号）。

---

## 6. 磁盘枚举规范（PE 兼容核心）

允许 API 白名单：`CreateFile(\\.\PhysicalDriveN)`、`DeviceIoControl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX)`、`GetVolumeNameForVolumeMountPoint`、`GetDriveType`、`SetupDi*`（如需）。**禁用 WMI**（`Win32_DiskDrive` 等一律不用）。

返回 POD：`Disk{index,serial,size,style MBR/GPT}` + `Partition{guid,offset,size,letter,fs,isESP/isSystem}`。

铁律：全工程 `std::wstring` + `CreateProcessW`；wim 源路径强制 `X:/` 尾斜杠（wimlib Linux 路径风格）；长路径用 `\\?\` 前缀；GUID 比较用 ASCII（不受代码页影响）。

---

## 7. BCD + GRUB4DOS SOP（成熟方案，严禁发明）

**原理**：Windows `bootmgr` 只能加载 osloader 或 bootsector，**无法直接引导 Linux 内核**，必须经 GRUB4DOS 中转。

**链路**：`BIOS → MBR → bootmgr → [BCD「实模式启动扇区」→ D:\grldr.mbr] → \grldr → \menu.lst → kernel vmlinuz + initrd initramfs → restore.sh`

**目标分区准备与引导补全（PIT-051 / PIT-053）**：`zjrestore-lite.sh` 默认 **mkntfs 快速格式化**目标分区（默认模式 `ZJ_TARGET_MODE=format`；2026-09-18 已实测可引导，见 PIT-053），然后 apply 主镜像，再保证目标分区**能启动 Windows**：
① 写**完整**的微软 NTFS 引导区——**两个 blob 缺一不可**：`bootfiles/ntfs-boot-code.bin`（426B，扇区 0 的 0x54..0x1FD）+ `bootfiles/ntfs-boot-cont.bin`（4096B，扇区 1..8）；并保证 `mkntfs --partition-start` = 分区真实起始 LBA（= 契约 `target_part_offset`/512，写入 BPB 0x1C 隐藏扇区数），否则引导代码会按错误绝对偏移读盘；
② 把 `bootfix/`（Windows 暂存阶段用 `bcdedit /export` 生成的**可移植 BCD** + `\bootmgr`）`cp` 进目标分区，补上万能镜像常缺的 `C:\Boot\BCD`（**不能用 wimlib apply**：卷模式遇已存在文件报 rc=46）。
磁盘 MBR 不动（我们只格式化分区）。`ZJ_TARGET_MODE=keep` 保留旧行为（保留原 NTFS 只清空），仅作对比/兜底。

**BCD 五命令（原样复制）**：

```cmd
bcdedit /create {GUID} /d "一键还原恢复环境" /application bootsector
bcdedit /set {GUID} device partition=D:
bcdedit /set {GUID} path \grldr.mbr
bcdedit /displayorder {GUID} /addlast
bcdedit /bootsequence {GUID}
```

> `grldr` / `grldr.mbr` / `menu.lst` 都必须在数据盘**根目录**（GRUB4DOS 硬限制，PIT-058），
> 三个都标隐藏+系统；内核/initramfs 与日志进 `D:\ZJRESTORE\`。

`InstallAsync` 六步（PIT-059：部署到**目标分区**，不再动数据盘）：定位目标分区 → 建 `<目标>\ZJRESTORE\` → 复制 vmlinuz/initramfs/restore.sh 到 `ZJRESTORE\boot|scripts`（bootfix 也生成到 `ZJRESTORE\bootfix`）→ 部署 `<目标>\grldr.mbr` + `<目标>\grldr`（根目录）→ 生成 `<目标>\menu.lst`（根目录）→ 建 BCD 条目（`device partition=<目标盘>:`）→ `/bootsequence`。

`NeedsInstall` 四项缺一不可：`ZJRESTORE 目录` + `<系统盘>\grldr.mbr` + `<系统盘>\menu.lst` + `BCD 条目存在`。

**诊断日志与清理（PIT-058/059/104）**：日志写**软件目录** `<exeDir>\logs\`（conf 的 `software_dir=` 键；软件在目标盘/只读介质上时回退 `<数据盘>\ZJRESTORE`），**成功也保留**供日后排查。程序在光盘/U盘上时**启动即询问是否整包复制到本地固定分区再运行**（CLI 自动、不询问；目的地=Windows 分区之外的第一顺序可写固定分区；见 PIT-104），搬迁后日志一律在**新程序目录**的 `logs\` 下 —— "所有日志在一个文件夹"。
**还原成功后**：目标分区被格式化 → 目标盘上的救援文件 + 引导期日志 `zjrestore-boot.log` **自然消失**；Linux 侧只清旧版本遗留在数据盘根目录的引导文件（不碰软件目录日志/镜像）。**失败时**：引导期日志留在目标根（= 引导阶段没走完的信号），软件目录的日志用于排错。

**条目存在性判定（唯一正确做法）**：`bcdedit /enum {GUID}`，看输出是否**回显该 GUID**（存在约 200 字节，不存在约 15 字节"没有匹配的对象"；**两者退出码都是 0**）。GUID 是 ASCII，不受编码影响。

**PowerShell 提醒**：标识符必须加引号（`bcdedit /enum '{GUID}'`），因 PS 把 `{}` 当脚本块；C++ 的 `CreateProcessW` 直接传参，无此问题。

**安全**：`/bootsequence` 是单次启动，失败重启自动回 Windows；改 BCD 前先 `bcdedit /export` 备份。

### UEFI/GPT 分支（方案：内核 EFI stub + 固件启动项 Boot#### 直启，见 PIT-060）

- 链路：`UEFI → Boot####（NVRAM 固件启动项，FilePathList 直指内核）→ \EFI\ZJRESTORE\vmlinuz-zjrestore.efi (OptionalData=initrd=...) → 救援 init → zjrestore-lite.sh → 重启回 Windows`。**不经 bootmgr**（bootmgr 的 bootapp 只收 subsystem=16，加载我们的 subsystem=10 内核会 `0xc000007b`），**不用 GRUB2**。
- Windows 侧：`IsUefiFirmware()` 判固件；`FindEspPartition()` + `mountvol X: /s` 挂 ESP；`InstallUefiBootEntry()`（`src/boot/uefi.cpp`）把内核+initramfs 放 `<ESP>\EFI\ZJRESTORE\` 并写 `Boot####`（短格式 HardDrive 节点 + FilePath 节点；OptionalData = UTF-16LE 命令行）+ 挂 `BootOrder` 末尾；`bcdboot <目标>:\Windows /s <ESP>: /f UEFI` 修 ESP 引导；`SetUefiBootNext()` 单次启动。
- Linux 侧：`pt_type=gpt` → **跳过 PBR/引导区修复**（ESP 不动）；**成功保留** ESP 上的 `\EFI\ZJRESTORE\`（常驻恢复模块，仅 Windows 侧「删除启动还原」才清）。
- 安全门禁：GPT 目标**仅 UEFI 固件放行**（`safety.cpp`），BIOS+GPT 仍拒绝。
- **Secure Boot 分支**（2026-09-23 起换 **Debian** 链，见 PLAN §11.1）：`IsSecureBootEnabled()` 为真时改走 `固件 → shimx64.efi（**微软双签：CA2011+CA2023**）→ grubx64.efi（**Debian 签名**的 GRUB）→ 我们的内核（Debian 签名）+ initramfs`。选 Debian 的原因：① shim 双签才能在**只信 CA2023 的 2026 新固件**上启动（Ubuntu 目前只有 CA2011 单签）；② 关键存储/HBA 驱动 31/31（Alma 缺 5 项）；③ 内核更小。**不再需要 MOK 注册**（整条链都是发行版签名），也**不用** mmx64/fbx64（那是旧 MOK 备选线的资产）。资产在 `bootfiles/sb/{shimx64.efi,grubx64.efi,grub.cfg}`；回归测试 `tools/vmtest/uefi-ubuntu-smoke.ps1`。
- 回归：`tools/vmtest/uefi-smoke.ps1`（QEMU + OVMF，PASS）、`tools/vmtest/uefi-bootentry-smoke.ps1`（固件从 Boot#### 直启内核，PASS）、`tools/vmtest/uefi-screen.ps1`（屏幕有输出，PASS）。ESP/BitLocker 检查见 §11。

---

## 8. libwim 规范（唯一权威 https://wimlib.net/apidoc/，不抄博客）

Windows 下 `tchar = wchar_t（UTF-16LE）`，cdecl，动态链接 `libwim-15.dll`（LGPLv3），禁抄 `wimlib-imagex`（GPLv3）。

| 函数 | 一句话用途 |
|---|---|
| `wimlib_global_init / wimlib_global_cleanup` | 进程级初始化/清理（各一次） |
| `wimlib_create_new_wim` → `wimlib_write` → `wimlib_free` | 新建→写盘→释放 |
| `wimlib_open_wim` → `wimlib_overwrite` / `wimlib_free` | 打开追加场景（`overwrite` 仅用于 open） |
| `wimlib_add_image / wimlib_add_image_multisource` | 捕获（加 `--snapshot` 等价标志做 VSS 热备） |
| `wimlib_extract_image` | 应用（还原，`WIMLIB_EXTRACT_FLAG_NTFS`） |
| `wimlib_verify_wim` | 校验 |
| `wimlib_get_image_count / wimlib_get_image_info / wimlib_iterate_images` | 列镜像 |
| `wimlib_register_progress_function` | 进度+取消（回调回 `ABORT` 即取消；回调在工作线程，禁直接碰 UI/文件，见 §9） |
| `wimlib_get_error_string / wimlib_set_print_errors` | 排错 |

警告：`WIMStruct` 非线程安全；用 RAII `WimHandle` 类管理 `open/write/free`；进度回调只 `PostMessage`/原子写 `.tmp+rename`。

---

## 9. CLI / GUI 规范

CLI：`SysRecover.exe <cmd> [opts]`，无 `--no-gui`（默认就是 CLI），`--yes` 跳过确认，`--json` 供脚本。

| 命令 | 用途 |
|---|---|
| `list [--json]` | 列磁盘/分区/GPT-MBR/盘符 |
| `backup --dest <wim> [--source C:/] [--compress fast\|maximum\|recovery] [--append] [--verify] [--yes]` | 热备（源路径注意尾斜杠） |
| `restore --image <f> --disk N --part M [--index 1] [--no-repair-boot] [--yes]` | 先做 ESP/BitLocker/恢复分区/镜像在目标分区四检查 |
| `verify / images / diag / version` | 校验 / 列镜像 / 自检 / 版本 |
| `extract --file <镜像> [--index N] --path <镜像内路径> [--path ...] --dest <目录>` | **从镜像里取单个/一组文件**（支持通配符，如 `\Users\*\Desktop\*.txt`）——"还原前先看看、只捞一个文件出来" |
| `diag --zip [--out <zip>]` | **导出诊断包**（diag 文本 + `logs/` + 契约文件 + `version.json`）→ 用户直接把这个 zip 发回来即可排错。ZIP 由 `src/common/zip.{h,cpp}` 自写（store 模式、零依赖）|
| GUI 自动模式（Phase 5） | `--auto-backup --source C: --dest D:\x.wim` / `--auto-restore --image <f> --disk N --part M`（对齐老 `MainWindow.xaml.cs`，CLI 先行，GUI 后移植） |

退出码：`0 成功，1 通用失败，2 参数错误，3 需管理员，4 危险目标被拒，5 镜像校验失败，6 取消`。

控制台进度：一行刷新 `[hh:mm:ss] 百分比/GB/速度/ETA`。

GUI（Phase 5）：工作线程跑 wimlib，严禁在回调线程直接 `SetText`；必须 `::PostMessage(WM_WIM_PROGRESS)` 到 UI 线程（100ms 节流）；XML 皮肤进 RC 资源；DPI 用 Per-Monitor V2；PE 字体回退链 `YaHei→SimSun→System`；提权（**已落地**：GUI 带 `requireAdministrator` 清单，双击即由加载器弹一次 UAC，无需右键"以管理员身份运行"；**CLI 也必须提权**——机房批量部署靠"父进程已是管理员/SYSTEM"（管理员命令行、计划任务最高权限、PsExec `-s`、SCCM）实现**全程静默不弹窗**：这类上下文里进程本就是高完整性、清单不会触发 UAC；反之若留 asInvoker，写 BCD / 分区时会因权限不足失败）+ **GUI 单实例互斥** `Global\SysRecoverUI_SingleInstance`（命中即弹三选对话框：`使用之前的程序`[默认] / `使用现在新程序` / `退出`，见 `src/gui/instance_dlg.cpp` + `skin/instance.xml`；旧实例正在备份/还原时灰掉"使用现在新程序"，用 `WM_SR_QUERY_BUSY` 跨进程查询，查询失败一律当"忙"以免误关正在写盘的进程）；**副作用：非管理员会话下 `tools/ui/` 截图脚本启动 GUI 会弹 UAC 阻塞自动化**（见 §15）。

**还原确认与「静默模式」（2026-09-16 用户规格）**：`Silent` 勾选框默认**不勾选**。还原时——
- **未勾选**：只弹**一个**选择框（`CConfirmDlg` + `skin/confirm.xml`，非 `MessageBox`）——文案与默认项**跟随真实行为**（PIT-072）：
  · 需要重启（还原正在运行的系统盘）→ 按钮 `退出` / `退出并重启`（默认项，回车=重启，ESC=退出），
    正文注明"将暂存任务，随后自动重启执行"；选「退出并重启」→ 暂存任务 → 成功即自动 `shutdown /r /t 5` 重启。
  · **不需要重启**（PE 里 / 还原到非系统盘 = 就地还原）→ 按钮 `取消` / `开始还原`（默认项），
    正文注明"目标分区当前未被占用，将立即就地还原，**不需要重启**"。
  判断用 ops 层的 `CanRestoreInPlace()`（与 `StageRestore` 内部同一个函数，不会漂移）。
  **不再有**独立的"暂存成功"提示框，也**不带** `shutdown /c` 关机通知文本。
- **勾选**：完全无提示框，直接暂存 → 自动重启（供机房批量/无人值守）。
- **任务执行中点关闭/`Ctrl+F4`（2026-09-20 用户规格，替代原「只弹 MessageBox 拒绝」）**：`WM_CLOSE`（新实例 `PostMessage`/Alt+F4/任务栏）与 `CloseBtn` 两处都走 `AskBusyClose()` —— 弹二选框，**默认「继续等待」**（回车/ESC 都落在安全项上，防误取消）/ 可选**「终止并退出」** → `CancelAndExit()`：`m_cancel=true` → 保持消息泵地等 worker 收手（正常 <1s，wimlib 在下一次进度回调即 ABORT）→ `join()` → **删掉未写完的临时文件 `<目标>.tmp`**（`CleanupIncompleteOutput`；产物是原子写的——`wim.cpp::Capture` 先写 tmp、成功才 `MoveFileEx` 改名，所以取消**不会**损坏已有的同名镜像）→ 关窗；若 10s 仍未停则兜底 `TerminateProcess` + 把半成品登记为「下次开机删除」。⚠️ `Notify` 里关闭按钮必须**先于** `if (m_busy) return;` 处理（那里曾把关闭点击一起吞掉，表现为"点叉叉完全没反应"——见 PIT-071）。错误提示框在静默模式下仍保留（fail-safe）。


---

## 10. 日志与错误

级别：INFO/WARN/ERROR。**日志根**（PIT-104/105）：一般 = **程序目录**；**软件装在系统盘时 = `<数据盘>\ZJRESTORE\`**（还原/重装系统盘后日志仍留存）。Windows 日志、救援回写、BCD 备份、`history.jsonl`、`crash\` dump、`diag --zip` 全部落在这一个 `logs\` 下（`LogBaseDir()`，`src/common/relocate.cpp`；`diag` 会打印 `log_dir=`）。**启动还会自动生成 `diag.txt`/`list.txt` 并收集部署痕迹到 `logs\collected\`（PIT-107）—— 用户排错只需发 logs 文件夹。**原子写入（`.tmp+rename`）。还原/备份日志同时满足：日志根 `logs\SysRecover-YYYYMMDD.log` + 目标分区 `_zjresy*.log`。

---

## 11. 安全红线（CLI/GUI 共用检查表）

四元素校验（GUID/序列号/偏移/大小）fail-closed；**还原四检查**：ESP 分区 / BitLocker / 恢复分区 / **镜像文件在目标分区内**（命中任一即拒绝，需用户先移走镜像）；改 BCD 前 `/export` 备份；危险操作（格式化目标分区）必须二次确认（`--yes` 才能跳过）；管理员检查。

---

## 12. 测试矩阵

`Win7/10/11/PE × smoke/diag+list+verify / stage（暂存不重启）/ full（真重启进 Linux）`。BCD 用例只在正常 OS 跑，不在 PE 跑。`tools/pe-diag` 全绿为合入门禁。

---

## 13. 坑位登记册（新增坑必须用此模板，编号 PIT-xxx）

> **底座已更换（2026-09-17）**：救援层由 BG-Rescue 9.0.0 换成 **Alpine linux-lts 6.6**。
> 因此 **PIT-022 ~ PIT-043 中与 BG-Rescue 强相关的条目已作废**（bgrescue.rc 钩子、musl 1.1.21
> ABI、S99zjrestore/rcS 注入、`/init` 覆写、15 秒 USB 等待、blkid PART_ENTRY_OFFSET、无
> HBA 驱动等），保留仅作历史参考。新底座相关坑见 **PIT-044 ~ PIT-048**。

- PIT-001 BCD `path` 写成 `\grldr`：必须 `\grldr.mbr`（grldr.mbr 才是 bootmgr 可加载的启动扇区）。✅ 2026-09-05
- PIT-002 漏 `/application bootsector`：会被当 osloader，加载 Linux 内核必失败。✅ 2026-09-05
- PIT-003 全文匹配 GUID 误判：`{bootmgr}` 的 `bootsequence` 值里也有该 GUID。正确：`bcdedit /enum {GUID}` 看输出是否回显 GUID。✅ 2026-09-05
- PIT-004 PowerShell 花括号：PS 里标识符必须加引号，C++ `CreateProcessW` 无需处理。✅ 2026-09-05
- PIT-005 .NET 8 GBK 乱码（C# 遗留，C++ 警示）：`Encoding.GetEncoding(936)` 需注册 CodePages provider，否则中文变 `?`。C++ 侧对策：存在性判定只用 ASCII GUID，不解析中文。✅ 2026-09-05
- PIT-006 menu.lst 语法：GRUB4DOS 用 `kernel`+`initrd`，不是 GRUB2 的 `linux`。✅ 2026-09-05
- PIT-011 nim_duilib MinGW 直编三要素（已验证 329/329）：①排除 `CEFControl/RenderSkia/WebView2` 目录 + `**/{contrib,scripts,examples,test}` + `duilib.cpp`（纯 CEF 引用）+ `Image_LOTTIE.cpp`/`ImageDecoder_SVG.cpp`（需 Skia）；②加 `-D_stdcall=__stdcall`（RichEdit 头用的 MSVC 单下划线写法，**不改第三方源码**）；③加 `-I duilib/third_party/libwebp`（libwebp 内部头用相对路径）。✅ 2026-09-06
- PIT-010 `CreateProcessW` 裸文件名找不到系统工具：`L"bcdedit.exe"` 直接传会 `fail 2`（搜索顺序坑），必须 `GetSystemDirectoryW` 拼全路径（见 `SysToolPath()`；`manage-bde.exe` 同理）。✅ 2026-09-05
- PIT-008 `WIMLIB_*_FLAG_NTFS` 是**裸卷模式**，不是"启用 NTFS 语义"：`ADD_FLAG_NTFS` 要求源为**未挂载 NTFS 卷**，`EXTRACT_FLAG_NTFS` 要求目标为裸卷（且需 libntfs-3g，否则 `WIMLIB_ERR_UNSUPPORTED=68`）。**目录捕获/应用一律 flags=0**（NTFS 的 ACL/流在普通模式下本来就会处理；热备加 `WIMLIB_ADD_FLAG_SNAPSHOT` 即可）。✅ 2026-09-05
- PIT-009 热备必须传排除配置：`config=NULL` 时易失文件（pagefile/日志/Temp/云同步目录）在扫描与读取之间被改写 → `rc=88 WIMLIB_ERR_CONCURRENT_MODIFICATION_DETECTED`。做法：`src/wim/exclude.cpp` 生成默认清单 + 源内云目录动态检测（对齐旧 `ExclusionService`），`backup` 必传。✅ 2026-09-05
- PIT-007 MinGW `wprintf` 的 `%s` 是窄字符串：MinGW 链接 msvcrt.dll，其 `wprintf` 把 `%s` 当 `char*`（读到宽字符的 0x00 即停，输出为空）。宽字符串**必须用 `%ls`**。✅ 2026-09-05
- PIT-012 经典 Duilib MinGW 直编七要点（已验证 35/35 + GUI 链接）：①`StdAfx.h` 的 min/max 函数宏移到所有系统头之后，且定义前先 `#undef min/max`（libstdc++ `c++config.h` 会 undef，`minwindef.h` 会 define，状态不确定）；②**任何含 Duilib 头的编译单元，全部 C++ 标准头（尤其 `<functional>/<algorithm>/<thread>/<chrono>`）必须先于 `StdAfx.h` include**，否则 `bits/algorithmfwd.h` 的 `std::min/max` 模板与宏冲突（症状：`macro "max" passed 3 arguments` 或 StdAfx.h `#define max(a,b)` 行报 `expected unqualified-id before '(' token`——报错行是宏定义处，实为后续使用处冲突）；③加 `-fpermissive`（CDelegateStatic 函数指针→void* 等历史写法）；④`__super` 为 MSVC 专有 → 显式基类名（UICombo.cpp）；⑤`goto err` 跨初始化作用域 GCC 硬错误 → `return FALSE` 语义等价（UIRichEdit.cpp 4 处）；⑥`return false` 到指针类型（HFONT/桶数组）→ `return NULL`（UIManager.cpp/Utils.cpp）；GDI+ `DrawImage` 重载歧义 → 显式 `(INT)` 强转（UIGifAnim.cpp）；⑦排除 `UIFlash.cpp/UIWebBrowser.cpp`（需 ATL）并注释 `UIDlgBuilder.cpp` 的 `CWebBrowserUI` 分支；`Utils/stb_image.c` 必须加入构建（UIRender.cpp 引用 `stbi_load_from_memory`）。✅ 2026-09-07
- PIT-013 QEMU 下引导本项目内核**必须 `-cpu max`**：`vmlinuz-zjrestore`（5.15.12）使用了 qemu64 默认 CPU 不具备的指令集，启动即静默三重故障 → 重启循环，VGA/串口**零输出**（症状极易误判为"内核/镜像损坏"）。判活方法：`-nographic` 日志中 "Booting from Hard Disk" 出现 ≥2 次 = 崩溃循环；1 次且持续运行 = 存活。✅ 2026-09-09（QEMU 11.1.1 TCG）
- PIT-014 该 rescue 内核**无任何控制台输出**：`console=ttyS0` / `console=tty0` / `earlyprintk` 全部静默（内核疑似裁剪了 8250/VT 控制台驱动）。QEMU 里观察 Linux 阶段只能靠侧信道：① GRUB 阶段 `-nographic` VGA 镜像日志；② 磁盘写副作用（restore.sh 的持久化日志，`mdir` 查虚拟盘）；③ CPU 占用行为。**重建内核时务必开 `CONFIG_SERIAL_8250_CONSOLE=y`**（对齐 PLAN.md §4"进度供串口/日志采集"）。✅ 2026-09-09

- PIT-015 Duilib 的 `caption` 属性第 3/4 个值是**距右/下边缘的偏移量**，不是右/下坐标：写成 `caption="0,0,870,36"`（照搬窗口宽度 870）会让 `WindowImplBase::OnNcHitTest` 的命中条件 `pt.x < rcClient.right - rcCaption.right` 退化成 `pt.x < 0` 恒不成立 → 永不返回 `HTCAPTION`，**整个窗口拖不动**（症状：控件能点、界面能正常交互，就是没法拖动，且无任何报错，极易误判为"Duilib 不支持无边框拖动"）。正确写法 `caption="0,0,0,36"`。验收：`SendMessage(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenX, screenY))` —— 顶栏空白/标题处应返回 `2 HTCAPTION`，标题栏上的最小化/最大化/关闭按钮与模式 Tab 应返回 `1 HTCLIENT`（保证仍可点击）。成因：`skin/main.xml` 由设计稿转正式皮肤时把 `0,0,0,36` 误抄成 `0,0,870,36`（`resources/themes/default/main/main.xml` 里一直是对的）。✅ 2026-09-13

- PIT-016 经典 Duilib 皮肤七坑（把老 WPF 界面 1:1 搬到 Duilib 时实测，落地代码 `src/gui/ui_skin.cpp` + `skin/main.xml`）：①控件级圆角**只有** `borderround="cx,cy"`，且 `cx` 是**椭圆直径**（要 7px 圆角写 `14,14`）—— `roundcorner` **只认 `<Window>`**，写在控件上被静默忽略；②`pos="l,t,r,b"` 后两个是**右下角坐标**（rc 语义 `[l,t,r,b)`，最后一像素是 r-1/b-1），不是宽高；③`inset` 是**给子元素的内边距**，不是外边距，无 `height` 的子控件会被父布局拉伸填满；④**自绘控件必须用「新标签名」** —— 内建名（`Button`/`Edit`/`Option`/`Combo`/`Label`/`Progress`…）在 `UIDlgBuilder.cpp:292-352` 被拦截直接 `new` 库内类，`WindowImplBase::CreateControl()` 回调**永不触发**，自绘类拿不到创建机会（本项目用 `SkinButton/SkinEdit/SkinLabel/TabOption/GlyphCheck/RoundProgress/PartItem/TextItem/TitleLabel/WinBtn`）；⑤`font="N"` 必须先 `<Font id="N" name=... size=.../>` 声明，否则静默回退默认字体（自绘控件走 `fontsize`/`bold` 属性，不经这套）；⑥**GDI+ 抗锯齿会把 1px 描边摊成 2px 半透明**（实测 `#c9d6f2` 被画成 `#e4eaf8`，正是 50% 混白），1px 描边必须走 GDI `CreatePen(nStyle|PS_INSIDEFRAME, 1, ...)` + `HOLLOW_BRUSH` + `::RoundRect`（与 `CRenderEngine::DrawRoundRect`（UIRender.cpp:1270）同法，线全部压在 rc 内），`SmoothingModeAntiAlias` 只留给 width>1.5px 的粗线；⑦文字抗锯齿用 `CLEARTYPE_QUALITY`：老图是 WPF 无边框透明窗（灰度渲染），但 GDI 灰度 `ANTIALIASED_QUALITY` 的墨迹覆盖率只有它的 ~82%（表头 460px vs 558px），观感"发虚、不清晰"，换 ClearType 后达 96%（1294px vs 1343px），且字符 bbox 宽度与老图完全一致（主按钮 96 vs 95px、分区行 216 vs 216px）。✅ 2026-09-13

- PIT-017 经典 Duilib 同组 `Option` 的 `SELECTCHANGED` **在「被取消选中」时也会发**，且与「被选中」通知的**顺序不保证**（实测：先发新选中项，再发旧项的取消）。若按 `pSender->GetName()` 直接切界面，旧项的取消通知会把刚切好的界面**又刷回上一个模式** —— 症状是「Tab 视觉上已经切换、正文内容却没变，且无任何报错」，极易误判为 `SetText/SetVisible` 没生效。正确做法：只认 `static_cast<COptionUI*>(msg.pSender)->IsSelected() == true` 的那一次通知（`CMainForm::Notify`）。验收：向窗口 `PostMessage(WM_LBUTTONDOWN/WM_LBUTTONUP)` 依次点两个 Tab，第二次点完正文必须与 Tab 一致（`tools/ui/` 截图环可复现）。✅ 2026-09-13

- PIT-018 MinGW 下给 exe 嵌提权清单（GUI 启动即弹 UAC）六要点：①**GCC 14 起 MinGW-w64 不再自动链接 `default-manifest.o`** —— 本项目 GUI 原先 `RT_MANIFEST` 为空（`objdump -h` 连 `.rsrc` 节都没有），清单必须自己编，别以为"默认那份还在"；②清单只能走 `.rc`：`1 24 "SysRecoverUI.manifest"`（`24`=RT_MANIFEST，**id 必须写 1** =CREATEPROCESS_MANIFEST_RESOURCE_ID，写成别的值加载器根本不读、UAC 不弹），`windres -I src/gui x.rc -o build/x_rc.o` 再把 `.o` 和其它源文件一起交给 g++ 链接（`windres.exe` 就在 MinGW `bin/`，无需额外装）；③**清单里只写 `requestedExecutionLevel`，不要顺手补 `Common-Controls v6` / `dpiAware`** —— 本 GUI 全是 Duilib 自绘控件，受控件版本影响的只有 `SkinEdit` 内部那个原生 EDIT 子窗口，而加 `dpiAware` 会把界面从当前的 DPI 虚拟化切成真实 DPI 渲染，等于平白引入一处高 DPI 行为变更（本次需求与 DPI 无关）；即"与原 exe 相比只多提权这一处差异"；④清单方式**不改变工作目录**（同一个进程，由加载器解析），且 `CPaintManagerUI::GetInstancePath()` 取的是 exe 路径而非 CWD，故 `skin\main.xml` 定位不受影响 —— 这正是不选 `ShellExecuteW(L"runas")` 运行时自提升的主因（后者要重启进程、CWD 会变成 System32、还得自己防提权循环）；⑤Makefile 规则里**别仿写 `if not exist ... mkdir ...`**：本仓库 make 实际用的 shell 是 sh，该 cmd 写法会以 `syntax error: unexpected end of file` 失败（既有那几条从未暴露，只因目录一直存在、规则没被执行过）—— 新增输出物放到必然存在的 `build/` 根即可绕开；⑥验收：`tr -c '[:print:]\n' '\n' < dist/SysRecoverUI.exe | grep requireAdministrator` 应命中，旧产物应无（CLI 同理查 `dist/SysRecover.exe`）。✅ 2026-09-13

- PIT-019 经典 Duilib `WindowImplBase` 处理 `WM_CLOSE` 时**只销毁窗口、不结束消息循环** → 进程变「无窗口僵尸」，并**长期占着单实例 Mutex**。症状三连：①`PostMessage(WM_CLOSE)` 后 `IsWindow(hwnd)` 立刻变 `False`（窗口确实没了）；②进程却一直活着（任务栏/Alt+Tab 都看不见它）；③下次启动被互斥量拦下，而 `FindWindow(类名)` 又找不到窗口 → 单实例分支落空，或走 `WaitForSingleObject(hp, 5000)` **空等满 5 秒**。真机表现：「选『使用现在新程序』后对话框先关掉、等一会儿又弹一个框」——那 5 秒空等 + 兜底提示框就是这么来的。根因：`WindowImplBase::HandleMessage` 的 `WM_CLOSE` 分支只做 `m_PaintManager` 清理与 `DestroyWindow`，**从不 `PostQuitMessage`**，于是 `CPaintManagerUI::MessageLoop()` 的 `GetMessage` 永远等不到 `WM_QUIT`。修复：在自家窗体 `HandleMessage` 里 `if (msg == WM_DESTROY) ::PostQuitMessage(0);`。**两条注意**：①局部消息循环用的对话框（`CInstanceDlg::Ask()`）**不要**跟着 PostQuitMessage —— 它靠 `m_alive` 标志退出，若在此投递 `WM_QUIT`，该消息会残留到线程队列，把随后主窗口的 `MessageLoop()` 立刻打回、程序一闪即退；②验收：`PostMessage(WM_CLOSE)` 后 500ms 内进程必须消失（未修复时实测 6s 后仍在）。✅ 2026-09-13

- PIT-020 QEMU 测试台 `mk-drill.sh`/`mk-testdisk.sh` 分区表顺序坑：FAT 分区表条目从偏移 446 开始（MBR 64 字节主分区表），但脚本先用 `dd` 写 34 扇区的分区表（含 GPT 头 + 备份头），再追加写入 `grldr.mbr`（8192 字节 = 16 扇区，从偏移 0 开始）。由于 `grldr.mbr` 覆盖了前 16 扇区，**分区表条目被清零**，QEMU 识别不到任何分区 → `Cannot find GRLDR in all devices`。**修复**：先写 `grldr.mbr` 到扇区 0-15，再写分区表（扇区 16-33），这样分区表不被覆盖。✅ 2026-09-15

- PIT-021 `inject-drill.py` newc cpio 头字段错位：原始版本把 `filesize`（8 字节十六进制）写到了 `mtime` 的位置（第 5 个字段），导致所有注入文件的 size 被内核解析为 0 → 注入的 wimlib-imagex/libwim 等全是空文件。正确 newc 头字段顺序（11 个字段）：`dev/ino/mode/nlink/uid/gid/rdev/mtime/filesize`（`filesize` 是第 9 个，偏移 48-55）。修复后正确解析，所有文件大小与 mtools 拷贝一致。✅ 2026-09-15

- PIT-022 BG-Rescue 原生钩子机制（官方，不修改源码）：BG-Rescue 9.0.0 的 `/init`（`/etc/init.d/S90bgrescue.rc`）会自动扫描所有块设备分区，找**卷标以 `BG-Rescue` 开头**的分区，然后 source 该分区根的 `bgrescue.rc`（或 `rc.d/` 子目录）。这是 BG-Rescue 的官方扩展点，不修改其源码即可注入自定义逻辑。恢复分区必须改卷标为 `BG-Rescue...` 才能被识别。✅ 2026-09-15

- PIT-023 musl time64 ABI 不兼容：BG-Rescue 9.0.0 的 musl 是 **1.1.21**（2019，无 `__stat_time64` 等符号）；Alpine 3.18 的 wimlib/fuse3 二进制是 musl 1.2.4（2023，time64）。直接加载会报 `Error relocating libwim.so.15: __fstat_time64: symbol not found`。**解法**：随包自带 Alpine 3.18 的 `ld-musl-i386.so.1`（636KB，loader+libc），用包装脚本显式调用新 loader 运行 wimlib，绕开系统老 musl。✅ 2026-09-15

- PIT-024 `wimlib-imagex apply` **没有 `--ntfs` 选项**：`--ntfs` 是 Windows C API（`WIMLIB_EXTRACT_FLAG_NTFS`）的标志，Linux 版 `wimlib-imagex` 命令行工具对块设备（如 `/dev/sda2`）**自动以 NTFS 卷模式应用**，不需要也不接受 `--ntfs` 参数。传了会报 `apply: unrecognized option: ntfs`（退出码 255）。修复：去掉 `--ntfs`。✅ 2026-09-15

- PIT-025 `restore.sh` 持久化日志/目标分区卸载时序三 bug：①持久化日志写完立刻卸载（`umount` 在 `echo` 之后紧跟着），只剩 1 行记录；②目标分区查完日志后**未卸载**就调 `mkntfs` → `Resource busy`；③busybox ash **无 `${PIPESTATUS[0]}`**（bash 特性），直接报错退出。修复：持久化日志写完调 `sync`（不再立刻卸载，保证完整写入）；目标分区在 `mkntfs` 前卸载；`PIPESTATUS` 改为临时文件 + `$?`。✅ 2026-09-15
- PIT-026 `exec /bin/sh -c '...'` in /init hangs：BG-Rescue 的 busybox ash 里 `exec /bin/sh -c 'complex script'` 会因 stdin 阻塞挂死。必须 `</dev/null` 重定向 stdin。✅ 2026-09-16
- PIT-027 复杂 /init 脚本（含循环/条件/mount）在 QEMU 里挂死：在 busybox ash 的 /init 上下文中，复杂脚本（循环+条件+mount 组合）可能因解析或资源问题挂死。根因未完全定位，规避方案：用 S99zjrestore（/etc/init.d/ 独立脚本）替代 /init 修改。✅ 2026-09-16
- PIT-028 `inject-drill.py` 替换 /init 条目产生损坏 cpio：替换 /init 入口点的 cpio 注入方式会产生不可解析的 cpio（offset 7400 parse error）。规避：不替换 /init，改用 /etc/init.d/ + rcS 机制。✅ 2026-09-16
- PIT-029 Python 读写整个 2GB 磁盘镜像（`bytearray(open(...).read())`）损坏 FAT32：Python 一次性读取整个磁盘镜像到内存会破坏 FAT32 文件系统。必须用 `dd` 或小缓冲区写入做分区表修补。✅ 2026-09-16
- PIT-030 wimlib-imagex wrapper 需要 ZJTOOLS 路径在 exec 时有效：wrapper 脚本用 `$ZJTOOLS/ld-musl-i386.so.1`，如果 mount 被卸载后 exec，wrapper 找不到 musl loader。解法：S99zjrestore 在 exec 前将 zjtools 库拷贝到 /tmp（已落地）。✅ 2026-09-16
- PIT-031 mtools `mcopy -i IMG@@OFFSET` 需要 `-o` 覆盖标志：对已有文件的 FAT32 分区做 mcopy 时不带 `-o` 会静默失败。✅ 2026-09-16
- PIT-032 `mk-initramfs.sh` 用 wrapper 脚本覆盖了 wimlib-imagex ELF 二进制：`base/zjtools-wimlib-imagex`（494字节 shell 脚本）被拷贝为 `/usr/bin/wimlib-imagex`，覆盖了 Alpine 的 62760字节 ELF 二进制。S99zjrestore 拷贝该"假二进制"到 `.real`，musl loader 尝试执行 shell 脚本 → `Not a valid dynamic program`。**修复**：ELF 二进制放 `/usr/bin/wimlib-imagex.real`，wrapper 脚本放 `/usr/bin/wimlib-imagex`，S99zjrestore 拷贝 `.real` 文件。✅ 2026-09-16
- PIT-033 busybox blkid 不支持 `-s PART_ENTRY_OFFSET`：BG-Rescue 的 busybox blkid 可能不识别 `-s PART_ENTRY_OFFSET -o value` 选项，导致分区 offset 匹配失败。Fallback：从设备名推断分区号，用 `dd`+`od` 直接读 MBR 分区表获取 LBA 起始扇区。✅ 2026-09-16
- PIT-034 UEFI 下 grldr.mbr 无法加载（0xc000007b）：`grldr.mbr` 是16位实模式 bootsector，Windows boot manager 在 UEFI 模式下无法执行它（错误 `0xc000007b`——文件存在但格式无效）。症状：BCD 条目创建成功（菜单出现），但选择后报错"所需文件丢失或包含错误"。**临时规避**：safety check 检测 GPT GUID 并拒绝还原，提示"当前版本仅支持 MBR/BIOS 引导，UEFI 支持开发中（Phase 3）"。**路线**：先把 MBR/BIOS 做通，UEFI 引导（GRUB2 + `grub.cfg` + `bootx64.efi`）在 Phase 3 实现，不放弃。✅ 2026-09-16
- PIT-035 契约文件跨分区导致 Linux 找不到任务：`_zjresy*.log` 写**目标分区根**（主契约），`restore-task.conf` 写**软件目录**（副契约），二者常在不同分区（日志在 C:、软件在 D:\dist）。原 `S99zjrestore` 只在「日志所在分区根」找 conf → 找不到 → `zjrestore-lite.sh` 判"未找到任务文件"退出 → 停在 `rescue:/#`（症状：还原环境起来后无任何动作）。**修复**：①Windows 侧额外把 conf 写到目标分区根（与日志同处）；②S99zjrestore 独立搜索各分区（含子目录 `-maxdepth 4`）找 conf；③zjrestore-lite.sh 的 `get_task` 在 conf 缺失时回退到日志字段（日志已含全部所需键）。✅ 2026-09-16
- PIT-036 GRUB4DOS 菜单中文乱码：GRUB4DOS 显示中文需额外加载字体文件（`fontfile` + 中文码表），本项目未随包分发字体，menu.lst 写中文（UTF-8 或 GBK）一律显示为乱码。**规避**：menu.lst 一律用 ASCII 文本（`SysRecover Restore Environment`）。若要中文需另做字体打包。✅ 2026-09-16
- PIT-037 `NeedsInstall` 短路导致修复不生效：原还原流程写 `if (NeedsInstall(...) && !InstallBootLayer(...))`——四项检查全过就**跳过安装**，于是新版本修正过的 `menu.lst`/`initramfs`/脚本**不会随还原刷新**（症状：改了菜单/脚本，VM 上表现仍是旧的）。**修复**：还原前**无条件** `InstallBootLayer`（幂等，BCD 条目已存在则跳过创建），确保引导文件随本版本刷新。✅ 2026-09-16
- PIT-038 BG-Rescue 启动阶段有 ~8 次交互式选择，每次 `read -t 15` 阻塞 15 秒：来自 BG-Rescue 原生 init 脚本——`/etc/init.d/S20keymap`（选键盘布局）、`S25font`（选字体）、`S30modules`（是否加载全部模块）、`S40network`（是否启用网络）、`S60lvm2`（是否启用 LVM），以及 `/etc/inittab` 的 `tty2/3/4::askfirst`（3 次"Please press Enter to activate this console"）。这些脚本都读 `/etc/boot.config` 的 `BGKEYMAP/BGFONT/...` 变量（设 `"0"` 即跳过），但该文件由 `/init` 在启动时无条件用空值覆盖，且交互变量不跨脚本传递（rcS 每个脚本都在子 shell/子进程里跑），**无法用配置消除**。**解法**：在 initramfs 里注入同名覆盖脚本（`bootfiles/zj-init/`，构建时 `inject-drill.py` 的 drop 参数先删原文件再加覆盖版），脚本只保留必要逻辑（如 S30modules 保留基础模块加载、去掉"加载全部"询问），并注入精简 `inittab`（只留 `tty1::respawn`）。✅ 2026-09-16
- PIT-039 Duilib `CSkinLabelUI`/`TextIn` 是**单行绘制**（`TextOut`），文本里的 `\n` 不会换行（会画成缺字符方框）。多行提示必须拆成多个 `SkinLabel`，逐行 `SetText`（见 `CConfirmDlg::InitWindow` 按 `'\n'` 拆分到 `DlgMsg1/2/3`）。✅ 2026-09-16
- PIT-040 `zjrestore-lite.sh` 用 offset 匹配目标分区永远失败：`blkid -s PART_ENTRY_OFFSET` 返回 **512 字节扇区数**，而契约 `target_part_offset`（`WriteRestoreLog`/`WriteRestoreTask`）写的是**字节数**，两者恒不相等 → 判"未找到目标分区"退出 → 停在 `rescue:/#`。**正解**：恢复日志 `_zjresy*.log` 本来就写在**目标分区根**，所以「找到日志的分区就是目标分区」——`S99zjrestore` 把该设备作为 `$1` 传给脚本（对齐生产 `restore.sh` 的定位方式），不再做 offset 比较；脚本保留 offset 回退但改为 `_sec * 512`。✅ 2026-09-16
- PIT-041 BG-Rescue `/init` 固定 15 秒 USB 枚举等待：`/init` 里 `DELAY=15` + `echo "Waiting for $BGUSBDELAY seconds..."` + `sleep $BGUSBDELAY`，而 `BGUSBDELAY` 只能由「卷标含 BG-Rescue」分区上的 `bgrescue.cfg` 覆盖（本项目不改卷标，故无效）。**解法**：注入修改版 `/init`（`bootfiles/zj-init/init`，`DELAY=0` 并删掉该 echo+sleep），构建时用 `inject-drill.py` 的 drop 参数先删原 `/init`。注意 PIT-027/028 的警告源于当年**有 bug 的注入器**（PIT-021 头字段错位），现注入器已修好，替换 `/init` 安全；替换后务必 `parse-initramfs.py cat init` 复核内容再上机。✅ 2026-09-16
- PIT-042 诊断 Linux 还原阶段"无动作"：rescue 内核**不向串口/VGA 输出内核日志**（PIT-014），但 **shell 里的 stdout 是可见的** —— 故 `S99zjrestore`/`zjrestore-lite.sh` 的 `say()` 必须同时 `echo` 到控制台（不只写 `/tmp/zjrestore.log`），失败时用户在 `rescue:/#` 能直接看到卡在哪一步；`/tmp` 是 RAM，重启即失，需要长期留痕时再写目标/镜像分区。✅ 2026-09-16
- PIT-043 rescue 内核**存储驱动覆盖有限**，VMware LSI SCSI 硬盘认不到：内核 `5.15.12-64bit (bg@rescue)` 内建 **ahci / ata_piix(IDE) / nvme / usb-storage / sd**（够用真实 PC），但**没有 mptspi/mptsas（VMware LSI Logic SCSI）、virtio_blk、megaraid**。症状：`/proc/partitions` 除表头外为空、`/sys/class/block` 只有 loop0-7，`list_parts` 空 → 找不到 `_zjresy*.log` → 落 shell（**不是挂载问题**，是设备根本没出现）。内核 `CONFIG_MODULE_UNLOAD=y`（**支持模块**）但 initramfs **不含任何 .ko**（`usr/lib/modules` 是空目录，`/etc/modules` 仅注释）——BG-Rescue 的模块在其 ISO 的 squashfs 里，我们只用 initramfs 故拿不到。vermagic = `5.15.12-64bit SMP mod_unload `（BG-Rescue 自编，只有它自己的模块能加载）。**规避**：VM 磁盘控制器改用 **SATA(AHCI) 或 NVMe**（内核都支持）；**正解**：换 Alpine 底座（PIT-044）。✅ 2026-09-16

- PIT-044 **救援底座换 Alpine linux-lts**（2026-09-17，根治 PIT-043）：BG-Rescue 内核的 `.config` 可从 vmlinuz 里提取（`IKCFG_ST`…`IKCFG_ED` 之间是 gzip 的 `.config`），证实 54 个 SCSI HBA 驱动**全部 `is not set`**、`=m` 模块数 **0** → 所谓"支持 SCSI Disk"只指通用 `sd` 层，**没有 HBA 驱动**，VMware LSI/服务器 RAID 一律认不到盘。Alpine 的 `linux-lts` 把全套 HBA 编成模块：`mpt3sas`/`mptspi`/`mptsas`/`megaraid_sas`/`aacraid`/`hpsa`/`arcmsr`/`vmw_pvscsi`/`virtio_blk`/`virtio_scsi`/`usb-storage`/`uas` + `ntfs3`/`fuse`/`vfat`/`exfat`。**组装方式**：Alpine 包是 **tar.gz（apk）**，可在 **Windows 上直接解包**，无需 Linux 环境 → `tools/vmtest/build-alpine-initramfs.py` 从 apk 组装（busybox-static + ntfs-3g/mkntfs + wimlib + util-linux blkid + musl + 裁剪后的 443 个模块），内核用 `boot/vmlinuz-lts`。附带收益：Alpine 内核内建 `SERIAL_8250_CONSOLE`/`FRAMEBUFFER_CONSOLE`，**PIT-014 的"控制台零输出"问题也随之消失**。✅ 2026-09-17
- PIT-045 Windows 上打包 initramfs **必须显式带权限位**：Windows 不跟踪 Unix 执行位（`os.stat().st_mode & 0o111` 恒为 0），若用本地文件权限推 mode，cpio 里所有文件都会是 0644 → 内核执行 `/init` 报 `Failed to execute /init (error -13)`（EACCES）→ `Kernel panic: No working init found`。解法：解 apk 时用 tar 成员自带的 `m.mode` 记录到 `MODES{路径:权限}`，打包时按此写 cpio；自行生成的脚本显式标 0755。**另**：`/init` 的 `#!/bin/sh` 要求 `/bin/sh` 在 exec 前就存在 → 镜像里预置 `/bin/sh -> busybox` 符号链接（其余 applet 由 `/init` 里 `busybox --install -s /bin` 运行时生成）。✅ 2026-09-17
- PIT-046 apk 内大量文件是**硬链接**：Alpine 的 wimlib 包中 `wimlib-imagex`/`wimapply`/`wimcapture`… 全是硬链接到 `wimappend`（tar `type '1'`，`m.isfile()` 为 False）。解包函数只处理普通文件/符号链接会**静默漏掉它们**（症状：`wimlib-imagex: not found`、只有 `wimappend` 落地）。解法：`m.islnk()` 时建**相对符号链接**（`os.path.relpath(m.linkname, dirname(m.name))`）——cpio 无硬链接概念，符号链接既能省空间，`argv[0]` 仍是原名，wimlib 多调用程序按名字分派行为不变。✅ 2026-09-17
- PIT-047 `busybox --install -s /bin` 会**遮蔽真工具**：busybox 自带简化 `blkid`，装到 `/bin/blkid` 后排在 PATH 的 `/bin` 里，抢在 util-linux 的 `/sbin/blkid` 前面 → `-s TYPE -o value` 返回空 → 文件系统类型判定失败、挂载走错分支（症状：vfat 分区报 `mount ... failed: No such device`）。解法：`--install` 之后 `rm -f /bin/blkid`。**另**：initramfs 根里默认**没有 `/proc` `/sys` 目录**，必须 `mkdir -p` 后再 `mount`，否则 `mount -t proc proc /proc` 失败（`/sys/class/block` 空 → 一个分区都枚举不到）。✅ 2026-09-17
- PIT-048 引导修复不再依赖 `ms-sys`（Alpine 仓库无此包）：我们只格式化**分区**、不动磁盘 MBR 引导代码，所以关键是恢复分区 PBR。做法：`mkntfs` **之前** `dd` 备份原 PBR 512 字节，**之后**把微软引导代码段（偏移 `0x54..0x1FD`，426 字节）写回——等价 `ms-sys --ntfs` 的 boot code 部分，且 BPB 仍是 mkntfs 的正确值。**移植前先验 OEM 名**（偏移 0x03 8 字节是否为 `NTFS    `），否则会把 FAT 引导代码写到 NTFS 上。`ms-sys` 存在时仍优先走它（兼容旧底座）。✅ 2026-09-17
- PIT-049 PBR 移植 `dd` **必须同时给 `skip=84` 和 `seek=84`**：只写 `seek=84 count=426` 会从源文件的**偏移 0** 读，把原 PBR 开头的 BPB 写到新 PBR 的 0x54，引导代码整体**错位 78 字节**——症状是开机黑屏、左上角光标闪烁（BIOS 已交出控制权，PBR 执行到垃圾指令）。校验：把 `dd` 后的 PBR 与未动过的同型 NTFS 分区 PBR 对比，0x54 处应是 `fa 33 c0 8e d0 bc 00 7c`（而不是 `eb 52 90 4e 54 46 53`）。已加回归测试 `tools/vmtest/test-pbr-transplant.sh`。✅ 2026-09-17（用户实测踩到）
- PIT-050 引导代码**不能以「格式化前备份的原 PBR」为源**（会自我复制污染）：`mkntfs` 会给新分区写一份 **ntfs-3g 自带的引导代码**（特征：有 `BOOTMGR is compressed` 但**没有** `BOOTMGR is missing`），它**不能引导 Windows**。一旦某次还原后分区停留在这个状态，下一次还原的"原 PBR 备份"就是这份非引导代码 → 每次都写回它 → 永远黑屏光标闪（实测：两次还原后 sda1 PBR 的 0x54 区与真 Windows 差 140 字节）。**修复**：随包内置 `bootfiles/ntfs-boot-code.bin`（426B，偏移 0x54..0x1FD，取自同机未动过的真 Windows 分区 PBR），还原时优先写它；只有备份 PBR 通过「OEM=`NTFS    ` + 含 `BOOTMGR is missing`」双重校验时才用备份。**诊断技巧**：`mkntfs -f` 后根目录是空的（只有 `$MFT`/`$Bitmap` 等 `$` 系统元文件），所以「根目录里出现 `Windows`/`Users`/`bootmgr`」= apply 确实写了；配合 `$Bitmap` 的已用簇数可定性。✅ 2026-09-17（用户实测踩到）
- PIT-051 **万能镜像的 C: 常不含 `\Boot`（BCD）**，apply 完也起不来：Win10 官方安装会把引导文件放 C:\（BIOS）或 ESP（UEFI），而"万能/GHO/ESD 系统镜像"作者常常只打包系统分区内容，引导文件靠部署工具生成。我们只 apply 镜像 → `C:\bootmgr` 有、`C:\Boot\BCD` 没有 → 无法启动（PE 里 `bcdboot` 一跑就好）。**修复（BIOS 路径）**：**Windows 暂存阶段**（本机还在跑 Windows）用 `bcdedit /store <copy>\BCD` 把 `{bootmgr}`/`{default}` 的 `device`/`osdevice` 改成**可移植的 `boot`**、`{default} path \Windows\system32\winload.exe`，连同 `C:\bootmgr` 一起复制到 `D:\ZJRESTORE\bootfix\`，再用 libwim 打成 `bootfix.wim`；**Linux 侧**在 apply 完主镜像后**再 apply 一次 `bootfix.wim`**（`/init` 扫各分区找它），把 `\bootmgr`+`\Boot\BCD` 写进目标分区。原因：Linux 侧没有 bcdboot，而 BCD 是注册表 hive，不好凭空生成。UEFI 路径（写 ESP `\EFI\Microsoft\Boot\BCD` + `bootx64.efi`）留待 Phase 3。**踩坑**：`C:\Boot\BCD` 在运行中的 Windows 里被当作注册表 hive 挂着（`HKLM\BCD00000000`），文件**独占锁定**，`CopyFile` 必失败（`copy FAIL C:\Boot\BCD`）；必须改用 `bcdedit /export <目标文件>` 导出（官方方式，导出的就是可用作 `\Boot\BCD` 的 store），再 `bcdedit /store` 改成 `device boot`。**同理**：内置引导代码 `ntfs-boot-code.bin` 只是 426B 代码段，写回时**不能**再 `skip=84`（那是给完整 512B PBR 用的）——曾因此把代码段又跳了 84 字节，而校验用同一 skip 造成"假通过"（实测 PBR 0x54 变成 `e13b060b`）。回归测试 `tools/vmtest/test-pbr-transplant.sh` 覆盖两条路径。**再踩坑**：Linux 侧**不能用 wimlib apply 写补全包** —— wimlib 以「NTFS 卷」模式 apply 时**不覆盖已存在文件**，而镜像里通常已有 `\bootmgr` → `Can't create "/bootmgr" in NTFS volume: File exists` → rc=46（`WIMLIB_ERR_NTFS_3G`），`\Boot` 只建出空壳。改为：Windows 侧产出 `ZJRESTORE\bootfix\` **目录**（不打 WIM），Linux 侧 mount 目标分区（`ntfs-3g -o force`）后 **`cp -rf` 覆盖写入**。启动菜单等待也顺手设 `{bootmgr} timeout 0`。✅ 2026-09-18（QEMU 演练验证：`install bootfix files` → `bootfix installed; target root: ... Boot ... bootmgr`；`PBR boot code written+verified (0x54 = fa33c08e)`）
- PIT-052 ⚠️**本节"不认 mkntfs"的结论已被 PIT-053 推翻**（保留作历史）：**Windows 的 BIOS 引导代码不是 512 字节**（开机黑屏光标闪的根因之一）：引导代码跨 **9 个扇区**——第一扇区 0x54..0x1FD 的代码段 + **扇区 1..8 的后续代码**；代码开头把后面 16 个扇区读进内存，再 `jmp 0x226` 跳到扇区 1 继续。`mkntfs` 只写第一扇区（自带一份非 Windows 代码），仅移植第一扇区的 426 字节 → 扇区 1..8 全 0 → `jmp 0x226` 跳进空白 → 死循环（**无任何报错**，只有光标闪）。补齐扇区 1..8 后**仍然黑屏**：阳性对照（QEMU 用真 Windows MBR 启动活动分区）证明同一份代码在 **Windows 自己格式化的 NTFS**（同机数据盘 sda2）上能正常跑（找到根目录、报 `BOOTMGR is missing`），在 **mkntfs 造的分区**上却在解析 `$MFT`/根目录时静默跳 BIOS boot-failure 入口 → **引导代码不认 mkntfs 的布局**。**修复**：**不格式化目标分区**，保留其原有 NTFS（连它的 Windows 引导区一起保留），用 `ntfs-3g` 挂载后 `rm -rf` 清空文件，再 `wimlib apply` + `cp` bootfix；目标非 NTFS 才退回 mkntfs。另内置 `bootfiles/ntfs-boot-cont.bin`（扇区 1..8）用于修复被上次失败还原写坏的引导区。**排查手法**（可复用）：`qemu-img create -f qcow2 -b <vmdk> ovl.qcow2` + `qemu-io -c "write -s file.bin <off> <len>"` 做免破坏试验，`-monitor tcp:...` + `screendump` 抓 VGA 屏。✅ 2026-09-18

- PIT-053 **mkntfs 快速格式化完全可用，PIT-052 是误诊**（2026-09-18 QEMU 实证）：用一个「mkntfs 分区 + 完整微软引导区（扇区 0 代码 426B + 扇区 1..8 共 4096B）+ 根目录**故意不放 bootmgr**」的靶子，用 GRUB4DOS `chainloader` 直接引导它，屏幕正确显示 `BOOTMGR is missing / Press Ctrl+Alt+Del to restart` —— 证明微软引导代码**能**解析 mkntfs 卷并找到根目录。所以目标分区**默认 mkntfs 快格**（`ZJ_TARGET_MODE=format`），不再需要"保留原 NTFS 只清空"那套绕路（它还会带来：残留文件→wimlib rc=46、FUSE 清空 20s 卡顿）。PIT-052 当初失败的真实原因是我们自己移植引导区时的三个 bug 叠加：①**漏写扇区 1..8**（只在 `REPAIR_BOOT` 的"保留 NTFS"分支写，`mkntfs` 分支从来没写）；②`dd skip=84` 错位（PIT-049，当时未发现）；③`hidden sectors` 写错（`mkntfs --partition-start` 不对时引导代码按错误绝对偏移读盘）。另：Alpine 的 `mkntfs` 自带的是**占位代码**（扇区里是 `This is not a bootable disk...`），必须整体换成微软引导区。**回归测试**：`tools/vmtest/pbr-probe.py`（造探针 → chainload → `memsave 0xb8000` 读 VGA 文本 → 断言 `BOOTMGR is missing`，退出码 0/1）。⚠️ 读屏**不要**用 monitor 的 `xp`（逐字符回显 + 转义序列，解析不可靠），用 `memsave <addr> <len> <file>`。✅ 2026-09-18

- PIT-054 `bcdedit /export` 会把**暂存阶段加的 GRUB4DOS 恢复条目**和 `{bootmgr} bootsequence` 一起导出进 bootfix 的 BCD，导致还原后的目标机出现**两个启动菜单**（一个是真 Windows，另一个是恢复环境的中文描述 → 显示成一串方框），而且 `bootsequence` 残留会把重启**再带回 GRUB4DOS/Linux**（症状：还原完成重启 → 选"Windows"却进 `#` shell，见用户 1111.png）。`PrepareBootFixFiles` 原来只删了 `{memdiag}`。**修复**：导出后对目标 store 再执行 `deletevalue {bootmgr} bootsequence` + `displayorder {恢复GUID} /remove` + `delete {恢复GUID} /f`，只留真正的 Windows 条目（`{memdiag}` 删除也补 `/f`）。✅ 2026-09-19（待用户回归确认）

- PIT-055 Alpine 救援层在 **VMware/QEMU 虚拟 NVMe** 上还原大镜像时，进度每 N% 卡满 **30s**，dmesg 报 `nvme nvme0: I/O N QID M timeout, completion polled`（实测 4982MiB 镜像）。**真正的根因**是虚拟 NVMe **偶发"完成中断丢失"**：设备其实**已把完成项写进 CQ**，但驱动没收到中断 → 要等满 `io_timeout`（默认 **30s**）才在超时处理里轮询 CQ 并发现完成项（内核原文："the device did indeed post a completion queue entry … but the driver believes it was never notified via interrupt"）。所以卡顿时长 = io_timeout，**与脏页/回刷无关**——压小 `dirty_ratio` 无效（实测只是把首次卡顿从 14% 提前到 2%，没减时长，已回退）。**修复**：把 `nvme_core.io_timeout`（单位=**整数秒**；`#define NVME_IO_TIMEOUT (nvme_io_timeout * HZ)`，故**最小只能到 1s，设不了 500ms**）从 30 降到 **1** —— 三处同时上：`modprobe nvme-core io_timeout=1`（见 `bootfiles/alpine/init`）、写 `/sys/module/nvme_core/parameters/io_timeout`（见 `init` 与 `zjrestore-lite.sh`）、内核 cmdline `nvme_core.io_timeout=1`（见 `src/boot/grub.cpp`）。轮询几乎立刻就在 CQ 找到完成项，卡顿 30s→1s；单个 I/O 正常远小于 1s，不会误判（真超时仍走内核既有 abort/reset，与超时值无关）。⚠️ 方向别搞反：**调大** io_timeout 只会让卡顿更久。⚠️ 卡顿是**被内核扣住的那个 I/O 请求**阻塞了写线程，所以"先跑到 100% 再检查"不可行（内核不会放行该请求）；总浪费 = 丢中断次数 × io_timeout，唯一杠杆就是超时值。🚧 待实测（属模拟器竞态，真实 NVMe 少见）。**测试 VM**：`D:\Software\System\_VMware\VMdisks\Win10New`（NVMe，**只有快照1**，旧的 LSI SCSI VM 与快照3 已不存在）。

- PIT-056 **用自己的热备份镜像还原后黑屏起不来 —— 元凶是排除了注册表事务日志**（2026-09-19 定位）：第三方镜像还原正常，唯独我们自己 backup 出来的镜像还原后黑屏。逐层排查：①镜像完整（用 libwim 列过 `\bootmgr`/`\Boot\BCD`/`\Windows\System32\winload.exe`/`ntoskrnl.exe`/`config\SYSTEM` 都在）；②还原流程正常（Linux 日志 `apply rc=0`、`bootfix installed`、`boot region OK (0x54=fa33c08e)`、`RESTORE DONE`）；③读 MBR 确认 **p1 就是活动 NTFS 分区**、还原后 `\Boot\BCD` = 20480B（bootfix 导出的可移植 BCD，非镜像自带的 24576B）→ 引导链完整。**真正原因**：热备（VSS）时 `\Windows\System32\config\SYSTEM` 等 hive 常处于**"脏"状态**（实测 base block 里 `primary_seq=130 != secondary_seq=129`），Windows 开机必须靠 `.LOG1/.LOG2` 把 hive 恢复一致；而 `exclude.cpp` 的默认清单**排除了 `\Windows\System32\Config\*.log*`、`*.regtrans-ms`、`*.TM.blf`、`\Users\*\NTUSER.DAT*.log*`** → 日志随包丢了 → hive 无法恢复 → **开机黑屏**。微软默认 `WimScript.ini` **不排除**这些日志。**修复**：从默认排除清单里删掉这 6 条，随包捕获日志（只有几 MB）。✅ 2026-09-19（**用户实测确认**：修复后重新热备份 + 还原，系统正常进入；排查手法：`wimlib_extract_paths` 取 hive → 读 REGF base block 的 `primary_seq/secondary_seq` 判脏；`bcdedit /store` 读还原后的 BCD）

- PIT-057 "还原完黑屏起不来"可能是**镜像本身是"写入未完成"的半截文件**（2026-09-19，承 PIT-056）：用户用刚备份的 `backup-20260919-124942.esd` 还原，症状是 GUI "开始恢复系统"按钮**是灰的**（切一下模式又能点），点了之后重启卡在 `#`。VM 磁盘取证（QEMU `-snapshot` 挂 VMDK + 临时"检查器" init 把 `zjrestore-debug.log` 打到串口）看到 Linux 侧 `wimlib-imagex apply` 原文：`[WARNING] The WIM_HDR_FLAG_WRITE_IN_PROGRESS flag is set ... a process may have crashed while writing the WIM` → `rc=84 WIMLIB_ERR_WIM_IS_INCOMPLETE`（= "written by a program that was terminated before finishing"）。即**上次备份中途退出/中断**，WIM 头 "写入中" 标志未清、文件是半截。而**暂存阶段从不校验镜像** → 坏镜像被暂存进任务 → 重启后 Linux 先把目标分区 mkntfs 了，apply 才发现装不上 → 既没系统又没引导 → 黑屏。**修复**：①新增 `WimEngine::Probe()`（`wimlib_open_wim(WIMLIB_OPEN_FLAG_CHECK_INTEGRITY)` + 检查 `wim_info.write_in_progress` / `image_count`），`StageRestore` 在**做任何事之前**先校验，坏镜像直接拒绝（退出码 5 + "请重新备份"）；②备份成功后也 Probe 一次自己的产物；③GUI 新增 `m_imageOk`（镜像可读才置真），读失败时主按钮保持灰 —— 修掉"切下模式就能点坏镜像"的怪象。**取证手法**（可复用）：把检查器 init 写进 `bootfiles/alpine/init` 临时构建 initramfs，`qemu-system-x86_64 -kernel vmlinuz-zjrestore -initrd <insp> -append console=ttyS0 -drive file=<vmdk>,format=vmdk,if=virtio -snapshot -nographic`（`-snapshot` 保证不写真实盘），检查器里 mount 只读后把目标/数据分区的日志 `while read` 逐行 echo 到串口。✅ 2026-09-19（Probe 已用"人为置 `WIM_HDR_FLAG_WRITE_IN_PROGRESS`"的镜像验证：暂存被拒、退出码 5）

- PIT-058 **GRUB4DOS 的 `grldr` 必须在分区根目录，做不到"根目录零文件"**（2026-09-19）：官方 README 原文 `The bootstrap code of GRLDR.MBR only finds GRLDR file in the root dir of a partition.`；且 `grldr` 只在 `/menu.lst`、`/grub/menu.lst`、`/boot/grub/menu.lst` 找配置（`grldr.mbr` 虽可由 BCD `path` 指向子目录，但用户最终决定不折腾，直接放根目录）。**最终布局**：根目录放 `grldr` + `grldr.mbr` + `menu.lst` 三个隐藏+系统文件，其余全部进 `D:\ZJRESTORE\`（`boot\`/`scripts\`/`logs\`）——即"一个文件夹 + 根目录三个隐藏小文件"。**诊断日志统一写 `D:\ZJRESTORE\logs\`**（`ZJRDIR`/`diag_dir`，不散在 C:/D: 根目录；出故障时让用户打包这个子文件夹发回）。**还原成功后 `cleanup_after_success` 把上面全部删掉（含 `logs\`），失败时全保留**（便于排错/重试）。⚠️ 想根目录零文件只能改 `grldr` 内置菜单或自写启动扇区，都不划算。✅ 2026-09-19（用户实测：热备份+还原正常）

> **备选方案（已验证可用，暂不采用）**：`grldr.mbr` 也可放进 `D:\ZJRESTORE\`，BCD 写 `path \ZJRESTORE\grldr.mbr`（**用户已在 VM 实测通过**），这样数据盘根目录只留 `grldr` + `menu.lst` 两个隐藏文件。当前为最大兼容性仍按上面"根目录三文件"布局；如日后需要更少根目录文件，可直接切回该方案（改 `DeployGrldr`/`BcdCreateBootsector` 两处即可）。

- PIT-059 **布局优化：救援改放目标盘（C:），日志放软件目录**（2026-09-19，用户要求）——不再往数据盘（D:/E:）根目录塞任何东西。①**救援文件**（`grldr`/`grldr.mbr`/`menu.lst` + `ZJRESTORE\{boot,scripts,bootfix}`）部署到**目标分区**（= 本次要被格式化的那块盘，通常 C:）：GRUB4DOS 在格式化前已把内核/initramfs 读进内存、bootfix 也被 `/init` 在 mkntfs 前拷进 `/tmp`，所以格式化目标不影响还原；BCD 条目 `device partition=<目标盘>:` + `path \grldr.mbr`。②**诊断日志写软件目录**（`<exeDir>\logs\`，`restore-task.conf` 新增 `software_dir=`，Linux 侧 `find_soft_dir` 定位）：**成功也保留**，用户直接在软件目录里打包发回；软件在目标盘/只读介质上时回退到 `<数据盘>\ZJRESTORE`（GUI 先弹确认）。③**引导期日志**：`/init` 在跑还原脚本前把引导阶段日志写一份到**目标分区根** `zjrestore-boot.log`；脚本一启动就删掉它（此后日志进软件目录）。**它若还在 = 引导阶段没走完**，让用户把该文件发回即可排错。④成功还原会把目标分区格式化 → 目标盘上的救援文件/引导日志**自然消失**；`cleanup_after_success` 只清旧版本遗留在数据盘根目录的引导文件（**不碰软件目录日志/镜像**）。✅ 2026-09-19（**用户实测确认**：VM 中热备份 + 还原均正常；目标盘部署生效、D:/E: 根目录干净、日志落软件目录）

- PIT-060 **UEFI/GPT 分支：内核 EFI stub + 往 NVRAM 写「固件启动项」（Boot####），既不用 GRUB2 也不用 loader**（2026-09-19）：`bootfiles/vmlinuz-zjrestore` 是带 EFI stub 的 PE（`MZ` + PE + subsystem=10=EFI_APPLICATION），**UEFI 固件可直接加载它**，`initrd=` 走内核命令行。QEMU(OVMF) 实测全链（`tools/vmtest/uefi-smoke.ps1`，PASS：`EFI stub: Loaded initrd from command line option` → Alpine 救援 `/init` → `SR: modules loaded`）。
  - ❌ **弯路：BCD `/application bootapp` 不行**。bootmgr 的 bootapp 只接受 subsystem=16 的 bootmgr 私有程序，加载我们的 subsystem=10 内核 → **`0xc000007b STATUS_INVALID_IMAGE_FORMAT`**（VM 实测：启动菜单里选条目 → 卡在固件 `Attempting to start up from: Windows Boot Manager...`，还原从未开始、目标分区完好）。另：`device` 也必须写 `boot`（`partition=X:` 的盘符在启动环境里解析不了）。
  - ✅ **正解（成熟法，同 Rufus / `efibootmgr` / `bcfg`）**：用 `GetFirmwareEnvironmentVariableW` / `SetFirmwareEnvironmentVariableW`（kernel32；写前必须 `AdjustTokenPrivileges` 启用 **`SeSystemEnvironmentPrivilege`**，管理员默认持有但未启用）往 NVRAM 写 `Boot####` + 插 `BootOrder`（命名空间 `EFI_GLOBAL_VARIABLE = {8BE4DF61-93CA-11D2-AA0D-00E098032B8C}`），**由固件直接加载内核**。
    - `EFI_LOAD_OPTION` 布局：`u32 Attributes(=1 即 ACTIVE)` + `u16 FilePathListLength` + `Description(UTF-16, 含 NUL)` + `FilePathList` + `OptionalData`。`FilePathList` 用**短格式**（固件自行展开，不需要 PCI 前缀）= `HardDrive(42B: part#, 起始 LBA, 扇区数, GPT PartitionId, mbrType=2, sigType=2)` + `FilePath(4+2*(len+1))` + `End(7f ff 04)`；GPT 信息从 `\\.\X:` 的 `IOCTL_DISK_GET_PARTITION_INFO_EX` 取（扇区大小取 `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`）。
    - **`OptionalData` 就是该镜像的 `LoadOptions`**：内核 stub 的 `efi_convert_cmdline()` 按 **`efi_char16_t*`（UTF-16LE）** 读取 → 命令行**必须 UTF-16LE**（内容：`initrd=\EFI\ZJRESTORE\initramfs-zjrestore.cpio.gz console=tty0 nvme_core.io_timeout=1 zjre=1`，`zjre=1` 是判归属的标记）。
    - QEMU 实证（`tools/vmtest/uefi-bootentry-smoke.ps1`）：固件日志 `BdsDxe: loading Boot0003 "ZJTEST" from .../vmlinuz-zjrestore.efi` → **固件确实从 `Boot####` 直接加载启动了我们的内核**。
    - 代码：`src/boot/uefi.cpp`（`InstallUefiBootEntry` / `RemoveUefiBootEntry` / `SetUefiBootNext` / `UefiBootEntryExists`）；写变量优先 Win8+ 的 `SetFirmwareEnvironmentVariableExW`（显式属性 `0x7` = NON_VOLATILE|BOOTSERVICE|RUNTIME，保证**重启后仍在**），Win7 回退普通版。
    - **常驻 + 单次**：条目挂在 **`BootOrder` 末尾**（默认仍进 Windows；开机启动菜单里能选到 → **Windows 蓝屏/引导损坏也能进**，这是用户明确要求）；还原暂存用 **`BootNext`** 做**单次**启动（不依赖顺序）。GUI 按钮 = `安装启动还原` / `删除启动还原`。
    - 安装时顺手清旧版残留：`bcdedit /delete {GUID} /f` + `deletevalue {bootmgr} bootsequence`（否则 Windows 启动菜单里会留一个必然报 `0xc000007b` 的条目）。
  - **ESP 挂载**：`mountvol X: /s`（用完 `/d`）。**还原后修 UEFI 引导**：`bcdboot <目标>:\Windows /s <ESP>: /f UEFI`。**Linux 侧**：`pt_type=gpt` 时跳过 PBR/boot-region 修复；**成功也不删** `<ESP>\EFI\ZJRESTORE\`（那是常驻模块，只有 Windows 侧「删除启动还原」才清）。
  - **体积代价**：内核 10.5MB + initramfs 38.7MB ≈ **51MB 常驻 ESP**（ESP 一般 ≥100MB，Windows 引导文件约 40MB，尚可；ESP 偏小需裁剪 initramfs）。**安全门禁**：GPT 目标仅 UEFI 固件放行（`IsUefiFirmware()`），BIOS+GPT 仍拒绝。
  - 🚧 **待真机/VM 回归**（QEMU 只验到"固件→Boot####→内核"；`bcfg boot -opt` 语法未攻下，故"OptionalData→命令行"是靠 UEFI 规范 + 内核源码 + Rufus/efibootmgr 先例，尚未实测）。✅ 2026-09-19（QEMU OVMF 验证固件直启内核）

- PIT-062 **Secure Boot 支持：走「微软签名的 shim → 我们签名的 UKI」链**（2026-09-19）：Secure Boot 开着时固件**只加载微软签名的镜像**，我们的内核未签名必被拒。调研同行做法：**易数一键还原** = 直接部署固件启动项 + **送微软 UEFI CA 2023 签名**；**傲梅一键还原** = 官方让用户进 BIOS 关 Secure Boot；**联想 OKR** = 同理需关。微软签名路线对个人**不可行**（EV 证书 ~$359/年可由 SSL.com 的 sole-proprietor 层级拿到、个人可申请，但 2025-10 起强制**年度第三方安全审计**，数千美元）。故我们选**免费且个人可用**的 shim+MOK（Ventoy/rEFInd 同款）。
  - **前提实测（关键）**：Alpine 内核 `CONFIG_LOCK_DOWN_KERNEL_FORCE_NONE=y`、**无** `LOCK_DOWN_IN_EFI_SECURE_BOOT`、`MODULE_SIG_FORCE` 未设 → **Secure Boot 开着也能加载未签名内核模块** ✓（否则救援层 1800 个 Alpine 模块全废，这条路直接走不通）。
  - **内核不吃 PE `.initrd` 节**：`efi_load_initrd()` 只认 `initrd=` 命令行或 `LINUX_EFI_INITRD_MEDIA_GUID` 设备路径 → 必须用 **systemd-stub 打 UKI**（它读自己的 `.cmdline/.initrd/.linux` 节并注册成设备路径）。
  - **链路**：`固件 → \EFI\ZJRESTORE\shimx64.efi（微软签名）→ grubx64.efi（= 我们签名的 UKI）→ systemd-stub → 我们的内核`。shim 找不到可信二级时**自动进 MokManager**（`mmx64.efi`）。
  - **一次性注册**：首次开机进蓝底 MokManager → `Enroll key` → 选随包的 `zj-mok.cer` → 重启。`IsMokEnrolled()` 比对 `MokListRT` 判断是否已注册；**未注册就暂存还原会在动目标分区之前拦住并提示**（避免"重启后什么都没发生"）。
  - **构建**：`tools/build-uki.py`（objcopy 打 UKI + osslsigncode 签名），`make package` 自动调用；产物 `dist/bootfiles/sb/{zjrestore-uki.efi,zj-mok.cer}`。⚠️ objcopy 的 `--change-section-vma` 必须 **ImageBase + 偏移**（预编译 stub 的 ImageBase=0x14df90000，照抄 systemd 文档的 0x20000 会报 `section below image base`）。
  - **资产**：`bootfiles/sb/{shimx64.efi,mmx64.efi,fbx64.efi,linuxx64.efi.stub}`（Ubuntu shim-signed 1.59 / Debian systemd-boot-efi 257）+ `keys/zj-mok.{key,crt,cer}`（**私钥不入库，务必备份**：丢了以后老客户已注册的 MOK 就对不上新签名）。
  - **安装**（`src/boot/uefi.cpp`）：`IsSecureBootEnabled()` → 部署 shim 布局、启动项指向 `shimx64.efi`；否则走原来的"启动项直启内核"（PIT-060）。GUI「安装启动还原」在 SB 机器上会提示需要重启一次注册。
  - `diag` 会打印 `firmware=` / `secureboot=` / 启动项状态。✅ 2026-09-19（构建链验证通过：UKI 49.3MB + 签名 `Signature verification: ok`；**待 SB 环境实测引导**）

- PIT-063 **Secure Boot 两种走法对比：`shim`（标准）vs `bootapp`（零交互，借微软 bootmgr）**（2026-09-19）：真机 SB 测试时 `shim` 路线在 VMware 固件上只报 `Verification failed: (0x1A) Security Violation`、**没进 MokManager**（NTLite 的判据：只出错、无 MOK 界面 ⇒ 被拒的是 shim 链本身，常见于固件只信 Windows CA 或策略限制；Ventoy 文档也承认有这类固件）。为对比，加了第二条走法，用环境变量 `ZJ_SB_MODE` 切换（默认 `shim`）：
  - `shim`：固件启动项 → `shimx64.efi`（微软 CA 2011 签名）→ `grubx64.efi`(= 我们签名的 UKI)。**官方指定机制**，但首次要 MokManager 注册一次。
  - `bootapp`：`bcdedit /application bootapp` → `\EFI\ZJRESTORE\efiloader.efi`（[a1ive/efiloader](https://github.com/a1ive/efiloader)，**GPLv3，像 grldr 那样单独分发**，24KB，subsystem=16 正是 bootmgr 要求的格式）→ `loadoptions \EFI\ZJRESTORE\zjrestore-uki.efi` → 我们的 UKI。**零用户交互**，但要 `nointegritychecks true`（关掉 bootmgr 的完整性校验）+ `device boot`；单次启动用 BCD `bootsequence`（不走固件 `BootNext`）。代价：依赖 Windows/BCD 完好（ESP/BCD 坏了进不去），且 `nointegritychecks` 正是 BlackLotus(CVE-2023-24932) 在收紧的点。
  - 两条路都要**先有 UKI**（`tools/build-uki.py`）；`bootapp` 不需要 `zj-mok.cer`/MokManager。
  - ⚠️ 自签 MOK 证书**必须带 `extendedKeyUsage=codeSigning`**（`build-uki.py` 已加），否则 EFI 镜像签名永远验不过。
  - 🚧 待用户真机/VM 实测两条路并选定默认（`CurrentSbMode()` 里改一行即可）。

- PIT-064 **就地还原：目标分区没被占用时直接写，不重启**（2026-09-19，用户规格）：原实现无论在哪运行都是"暂存 → 重启 → Linux 救援层 apply"，**在 PE 里、或还原到非系统盘时纯属多余**（用户实测吐槽："在 PE 中恢复镜像到 C 盘居然提示重启"）。现在 `StageRestore` 自动二选一：
  - **判据 `CanRestoreInPlace()`**（`src/app/ops.cpp`）：① 目标**不是正在运行的系统盘**（`GetWindowsDirectoryW()[0] != target.letter`）；② 能对 `\\.\X:` 加**独占锁** `FSCTL_LOCK_VOLUME`（卷上有打开的文件/句柄就会失败）。
  - **满足 → `RunDirectRestore()`**：`format.com X: /FS:NTFS /Q /Y`（不传 `/V` 以保留原卷标）→ `WimEngine::Apply`（**目录模式**，PIT-008）→ 仅当目标里出现 `Windows\System32\winload.exe`（=系统镜像）才跑 `bcdboot`（UEFI：`/s <ESP> /f UEFI`；BIOS：`/s X: /f BIOS`）→ **完成，不重启**。
  - **不满足 → 原路径**（暂存任务 + 重启 + Linux 救援，PIT-059）。
  - API：`StageRestore(req, err, bool* needReboot)`；GUI 据此决定是否 `RebootNow()`（就地完成时弹"还原完成，无需重启"），CLI 打印不同提示。
  - ⚠️ §2 禁令 1（重启类还原必须走 Linux）**不变**：就地还原不是"重启类"，两条路并存。
  - 🚧 待实测（PE 直装、还原非系统盘）。

- PIT-065 **`nointegritychecks` 在 Secure Boot 开启时被策略保护 → bootapp 路线不可用**（2026-09-19，用户实测）：`bcdedit /set {GUID} nointegritychecks true` 在 Secure Boot 开启时报
  `设置元素数据时出错。该值受安全引导策略保护，无法进行修改或删除。`
  （= "The value is protected by Secure Boot policy and cannot be modified or deleted"）。⚠️ **这不是新补丁/BlackLotus 加固的产物**（初版 PIT 归因有误，已改正）：微软官方文档（`testsigning` 页）就把这条报错列为**预期行为**，并直接让用户"去 BIOS 关 Secure Boot"；早年（Vista/Win7 时代）的问答里报的是同一条错。**即：自 Secure Boot（Win8）引入以来，"关闭完整性校验"这类启动项就一直被策略保护**，与 Windows 版本/补丁新旧无关。
  - **结论**：Secure Boot 机器上"第三方工具零交互加载未签名代码"不存在可行路线（除非自费买 Microsoft UEFI CA 签名，见 PIT-062）。业界只剩：`shim + MOK`（一次性注册）或 `微软签名`。
  - `ZJ_SB_MODE=bootapp` 代码保留（宽松固件/未开 SB 时仍可用），但 `CurrentSbMode()` **默认 `shim`**。
  - 附带收益：这次测试同时验证了另外两个修复 ✓（ESP 空间：清理旧文件后 31.9MB → 81.3MB free；镜像说明截断：`%s`→`%ls` 后显示完整 `1 - Windows 10 专业版（Admin）`）。

- PIT-066 **零注册候选：借 Canonical 的签名链（`shim` → Canonical 签名的 GRUB → 我们的内核）**（2026-09-19，待实测）：调研发现 GRUB 用普通 `linux`/`initrd` 命令加载时**可能不校验内核签名**（只有 `linuxefi`/`initrdefi` 校验；多条资料明写 "…boot … DESPITE the kernel is NOT SIGNED"，以及 "checks the kernel signature but **not the initrd**"）。而 Ubuntu 的 `grubx64.efi.signed` 与 `mmx64.efi` **是同一把证书签的**（实测均为 `CN=Canonical Ltd. Secure Boot Signing (2022 v1)` ← `Canonical Ltd. Master Certificate Authority`），而 Ubuntu 的 shim 内嵌的正是这把证书 → **shim 直接信任这个 GRUB，无需任何 MOK 注册** ✓。
  - 新增走法 `ZJ_SB_MODE=grub`：部署 `shimx64.efi + mmx64.efi + fbx64.efi + grub-ubuntu.efi(改名 grubx64.efi) + grub.cfg + 我们的内核/initramfs`，启动项 → `shimx64.efi`；`grub.cfg` 用 `linux`/`initrd` 指向 `/EFI/ZJRESTORE/vmlinuz-zjrestore` + initramfs。
  - **若成立**：零注册、零成本、无任何"绕过"（全部可执行文件都有合法签名）→ 成为 SB 机器上的默认走法。**若被 GRUB 拦住** → 转"变体 D"：用 Canonical 签名的 Ubuntu 内核 + 我们的 initramfs（initrd 从不受校验），代价是救援内核要换 + 体积涨。
  - 资产：`bootfiles/sb/grub-ubuntu.efi`（Ubuntu `grub-efi-amd64-signed` 1.215）、`bootfiles/sb/grub.cfg`。🚧 待 VM 实测（SB 开启）。

- PIT-066 **零注册正解（已实测通过）：借 Canonical 的签名链 —— `shim` → Canonical 签名的 GRUB → **Canonical 签名的内核** + **我们的 initramfs****（2026-09-19）：Secure Boot 机器上"零交互 + 免费 + 不绕过"最终落在这条链上。调研与实测过程：
  - **变体 C（失败）**：`shim → Canonical GRUB → 我们的未签名内核`。QEMU/真机实测 GRUB 报 `error: bad shim lock signature` —— Ubuntu 的 GRUB **强制**校验内核（`shim_lock` 机制），未签名内核被拒（那条"普通 `linux` 命令不校验"的说法是过时的）。
  - **变体 D（成功）**：把内核换成 **Canonical 签名的 Ubuntu 内核**，initramfs 保持我们的（**UEFI 规则：initrd 从不校验** ✓）。Ubuntu 官方文档原文佐证：*"all pre-built binaries … **with the exception of the initrd image**, are signed by Canonical's UEFI certificate, which itself is implicitly trusted by being embedded in the shim loader"*、*"**Initrd images are not validated**"*。
  - **构建**：`tools/build-ubuntu-rescue.py` —— 复用 `build-alpine-initramfs.py` 的 Alpine 用户态组装（busybox/musl/wimlib/ntfs-3g **与内核无关**），只把内核+模块换成 Ubuntu 的：`linux-image-6.8.0-31-generic`（14.7MB，签名者 = `Canonical Ltd. Secure Boot Signing (2022 v1)`，**与 mmx64.efi/grubx64.efi 同一把证书**）+ `linux-modules-6.8.0-31-generic`（999 个 `.ko.zst`，**也带 Canonical 签名**）。产物仍是 `bootfiles/vmlinuz-zjrestore` + `initramfs-zjrestore.cpio.gz`（**文件名不变 → 部署逻辑零改动**）。
  - **两个坑**：① Ubuntu 的模块包**没有 `modules.dep`**（安装时才 depmod）→ 构建脚本从每个 `.ko` 内嵌的 `depends=` 字段自己生成；② **`modules.dep` 必须用二进制写**（Python 在 Windows 上文本模式会把 `\n` 变 `\r\n` → busybox 解析出带 `\r` 的路径 → `module X not found in modules.dep`，错误信息里路径尾巴那个点就是 `\r`）。
  - **QEMU 回归**：`tools/vmtest/uefi-ubuntu-smoke.ps1`（PASS：`Linux version 6.8.0-31-generic` → `SR: modules loaded` → `SR: block devs: … sda sda1`）。
  - **`ZJ_SB_MODE` 默认已改为 `grub`**（`shim`/`bootapp` 保留可选）。体积：内核 14.2MB + initramfs 32.5MB ≈ **47MB**（比 Alpine 版 49MB 还小）。
  - **安全性质**：没有绕过任何东西 —— 链上每个可执行文件都有合法签名，和 Ubuntu 正常开机完全一样；微软/Canonical 换证书时我们只换文件。
  - **后续**：① 服务器 RAID 模块（`vmd`/`megaraid`/老 `mpt*`/`isci`）在 Ubuntu 的 `linux-modules-extra`（113MB）里，需挑子集补入；② 内建模块的 `not found in modules.dep` 日志噪音可静音。
  - **验证**：✅ 2026-09-19 用户 VM（Secure Boot 开启）实测**通过**（零交互、无 0xc000007b、无 MokManager），`Secure boot mode: grub`。
  - ⚠️ **2026-09-23 已被取代**：为覆盖「只信 CA2023 的 2026 新固件」，整条链**换成 Debian**（shim 微软**双签** + Debian 签名 GRUB/内核），见 **PLAN §11.1**。本条目保留作历史记录；`ZJ_SB_MODE=grub` 的机制不变，只是资产换成 Debian 的。

- PIT-061 **UEFI 救援"黑屏像卡死"：内核 `CONFIG_SYSFB_SIMPLEFB=y` 顶掉内建 efifb，而 `simpledrm` 是模块、没打包 → 没有 fbcon**（2026-09-19，用户实测踩到）：症状 = UEFI 还原时屏幕停在 `EFI stub: Loaded initrd from command line option` 加一个一闪一闪的光标，看着完全像死机；**其实内核在正常跑** —— 用户按 Ctrl+Alt+Del 热重启后，还原**已经做完并且进的是新系统**（进度条一直在输出，只是屏幕没有）。根因：Alpine 内核带 `CONFIG_SYSFB_SIMPLEFB=y`，开机把 UEFI GOP 帧缓冲注册成 "simple-framebuffer" 平台设备，**因此内建 `efifb`（`CONFIG_FB_EFI=y`）不绑定**；能接管它的只有 `simpledrm`（`CONFIG_DRM_SIMPLEDRM=m`），而 `build-alpine-initramfs.py` 的 `MODULE_EXCLUDES` 把整个 `kernel/drivers/gpu` 排除了（旧注释写"文字控制台用内建 efifb/vesafb"——**该假设在 UEFI 下不成立**）。**修复**：①构建脚本新增 `EXTRA_MODULES = ['kernel/drivers/gpu/drm/tiny/simpledrm.ko.gz']`（依赖闭包自动带回 `drm.ko`/`drm_kms_helper.ko`/`drm_shmem_helper.ko`，整包只 +0.13MB）；②`bootfiles/alpine/init` 里提前 `ldmod simpledrm`；③cmdline 仍是 `console=tty0`。**回归测试**：`tools/vmtest/uefi-screen.ps1`（QEMU+OVMF，`-display none` + monitor `screendump` 抓两次，字节差异 8.4% ⇒ 控制台活着；PASS）。**屏上文字只能 ASCII**：内核内置字体没有中文字形（中文显示成方框，同 PIT-036 GRUB4DOS 的教训），所以救援横幅一律英文、中文细节进日志。**BIOS 路径不受影响**（走 `vgacon`，`CONFIG_VGA_CONSOLE=y`）。✅ 2026-09-19（QEMU 截图实证 + 用户 VM 实证还原成功）

- PIT-067 **第二次部署引导层必失败：`menu.lst` 带 Hidden+System，`CreateFileW(CREATE_ALWAYS)` 被拒**（2026-09-20 用户在笔记本实测并修复）：症状见 `dist\logs\SysRecover-20260920.log` —— `安装引导层失败: deploy drive=C`，而 `copy OK ... grldr.mbr/grldr` 全成功、**唯独 `write menu.lst FAIL`** → 暂存中止、还原从未发生。根因：`WriteMenuLst` 用 `CreateFileW(..., CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, ...)` 打开**已存在且带 Hidden+System** 的 `menu.lst`（上次部署 `Hide()` 设的）→ `ERROR_ACCESS_DENIED=5`（与 PIT-037 的 `CopyOne` 同一个坑，当时这里漏了）。**修复**（`src/boot/grub.cpp` 的 `WriteTextFile`）：打开前先 `SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL)`。✅ 2026-09-20（用户修复；16:52 那次运行该错误已消失 → 证实生效）

- PIT-068 **`bootfix\bootmgr` 第二次覆盖是同一个坑（静默失败 → `bootfix INCOMPLETE`）**（2026-09-20 笔记本日志暴露、AI 定位）：16:23 首次暂存 `bootfix: ok`，16:52 第二次变成 `bootfix: FAILED` + `bootfix INCOMPLETE` + `prepare bootfix failed` —— **但还原照样成功**，因为 `C:\ZJRESTORE\bootfix\` 里还留着**上次**写好的 BCD/bootmgr（"侥幸成功"；真机第一次跑就会真的缺补全包 → 目标机黑屏）。根因：`\bootmgr` 天生带 Hidden+System（`CopyFileW` 还会把源属性继承给目标），第二次 `CopyFileW(src, dst, FALSE)` 覆盖被拒；而那一行是 `ok &= CopyFileW(...)`，**没有失败日志**，只能看到上一层的 `INCOMPLETE`。**修复**（`src/boot/bootfix.cpp`）：新增 `ForceCopy()`（先归零目标属性 + 失败写 `copy FAIL <src> (err=N)`），`CopyTree` 内部与 `\bootmgr` 复制均改走它；并给 `CopyTree` 两处静默失败（源非目录 / `FindFirstFileW` 失败）补日志。✅ 2026-09-20（编译链接通过；**待下次还原验证**：期望 `bootfix prepared`）

- PIT-069 **`software_dir` 缺失时救援层日志落不到软件目录**（2026-09-20 笔记本日志 `debug log NOT persisted (software dir not found)`）：`zjrestore-lite.sh` 只从任务 conf 读 `software_dir`；conf 找不到时回退读 `_zjresy*.log`（PIT-035），而**日志里只有 `software_path`（exe 全路径）、没有 `software_dir`** → `SD_REL` 为空 → `find_soft_dir` 直接 return 1。**修复**：`software_dir` 为空时从 `software_path` 去掉文件名当目录（`sed 's|\\[^\\]*$||'`）。✅ 2026-09-20（**待下次还原验证**：期望 `debug log saved: <software_dir>/logs/`）

- 📌 **编号勘误（2026-09-20）**：§13 里有**两个 PIT-066**（前者"两种走法对比"、后者"零注册正解（已实测通过）"，是同一主题的两次记录）；代码注释与 §7/§14 引用 `PIT-066` 时指的是**后者**（Canonical 签名链）。暂不改号以免破坏交叉引用，日后整理时合并。

- PIT-070 **换机后重启蓝屏：BCD 恢复条目残留旧设备（`device=partition=D:`），而救援文件已在新布局的 C: → bootmgr `0xc000000F`**（2026-09-20 用户在笔记本实测并修复，= 用户报告的"第一次的问题"）：在笔记本上第一次暂存（16:23 `bootfix: ok`）后**重启即蓝屏**（Windows 引导错误 `0xc000000F`"所需设备无法访问"），救援层从未启动。根因：BCD 里的恢复条目 `{12345678-…}` **已经存在**（早前用旧布局把救援文件放数据盘 D: 时建的，条目里 `device=partition=D:`），而新布局（PIT-059）把 `grldr`/`grldr.mbr`/`menu.lst` 放在**目标盘 C:**；原 `BcdCreateBootsector` 遇到"条目已存在"就**跳过重设** → 文件在 C:、条目指 D: → bootmgr 去 D: 找 `\grldr.mbr` 找不到 → `0xc000000F`。**修复**（`src/boot/bcd.cpp`）：**条目已存在也不再跳过**，每次都把 `device partition=<本次部署盘>:` + `path \grldr.mbr` 重刷一遍（幂等 = 自愈），并顺带刷新 description 与 displayorder（先 `/remove` 再 `/addlast`，避免重复调用报错）。✅ 2026-09-20（用户修复，已由后续还原运行验证）

- ✨ **新功能（2026-09-20，用户在笔记本上实现）**：GUI 第一步支持**把镜像文件拖进窗口**（`.esd`/`.wim`），等价于点「浏览系统镜像文件」→ 设为还原镜像。涉及 `src/gui/{main_form,ui_skin}.{h,cpp}`。

- PIT-072 **PE 里明明"就地还原、不重启"，确认框却提示"重启后还原"：文案写死，与实际行为不符**（2026-09-21 用户实测）：用户在 **PE** 里还原，程序确实**不重启**就地开始，但确认框的按钮与正文仍是写死的「退出不重启」/「重启后还原」+「选择…将暂存任务并自动重启执行」→ 与实际行为矛盾、误导用户。**修复**：① 把 ops 内部的 `CanRestoreInPlace()` 提升为**对外可见**（`src/app/ops.h`），GUI 用它判断"这次要不要重启"—— 与 `StageRestore` 内部**同一个函数**，两处不会漂移；② `StartRestore` 分两套文案：需要重启 → `退出` / `退出并重启`（默认项）+「暂存任务，随后自动重启执行」；不需要重启 → `取消` / `开始还原`（默认项）+「目标分区当前未被占用，将立即就地还原，不需要重启」。✅ 2026-09-21（编译通过，`0.1.5`；**待用户实测**：PE 里应显示"不需要重启"那一套）

- PIT-073 **`wimlib_get_xml_data` 返回的是 UTF-16LE（不是 UTF-8）；且镜像大小必须用 `TOTALBYTES − HARDLINKBYTES`**（2026-09-21 做"还原前空间预检 P1"时踩到并实测校准）：要读某个子镜像的**未压缩内容大小**，但我们打包的 `wimlib.h`（1.14）**没有** `wimlib_get_image_info()` ✗，只能走 `wimlib_get_xml_data()` 解析元数据 XML。两个坑：
  ① **编码**：返回的 XML 是 **UTF-16LE 带 BOM**（开头 `FF FE`，每个字符后跟 `00`），按窄串找标签会**静默失败**（返回找不到 → 预检被跳过、形同虚设）→ 必须先用 `WideCharToMultiByte` 转 UTF-8 再解析（代码按"第 2 字节是否为 0"判定）；另外 **XML 缓冲区要用 `free()` 释放** —— `wimlib_free()` 是释放 `WIMStruct` 的，**不是**通用释放器 ✗。
  ② **指标**：`<IMAGE>` 里的 `<TOTALBYTES>` 把**硬链接按独立文件计数**，拿它当"需要多少空间"会**高估近一倍** → 必须减掉 `<HARDLINKBYTES>`。
  **实测校准**：`TOTALBYTES 10.16GB − HARDLINKBYTES 5.09GB = 5.07GB`，与救援层 apply 日志的 `Extracting file data: 5188 MiB (=5.07GB)` **完全一致** ✓。
  **验证手法（可复用）**：用 **ctypes 直调 `dist/libwim-15.dll`**（`wimlib_global_init` / `wimlib_open_wim` / `wimlib_get_xml_data`）把 XML 打出来看 —— **只读，不碰任何分区**。✅ 2026-09-21（`0.1.6`）

- PIT-074 **检测 UTF-16 不能只看 `buf[1] == 0`：带 BOM 时开头是 `FF FE`**（2026-09-21 做 P10 时踩到，**P1 也因此静默失效过**）：`wimlib_get_xml_data` 返回 UTF-16LE **带 BOM**（`FF FE`），而我们最初按"第 2 字节是否为 0"判断 UTF-16 ✗ → 带 BOM 时 `buf[1] == 0xFE ≠ 0` ✗ → 误判成 UTF-8 → 解析全空 → `images` 显示 `0.00 GB`、**空间预检静默跳过**（fail-open：不报错、看不出来 ✗）。**修复**：① 先识别 `FF FE` BOM；② 否则扫前 16 字节里有没有 `0x00`（UTF-16 文本特征）。**教训**：fail-open 的检查（解析失败就放过）**必须留一条日志**，否则坏了也发现不了 —— 这次是靠 `images` 打出 `0.00 GB` 才暴露的。✅ 2026-09-21（`0.1.7`）

- PIT-075 **就地还原"第一次进度回调就中止"：进度回调返回值语义写反（`true` = 取消）→ `rc=76 WIMLIB_ERR_ABORTED_BY_PROGRESS`**（2026-09-23 用户实测踩到）：用户在 **PE（从光盘运行）** 和**把程序拷到 D 盘后**都遇到 `暂存失败：就地还原：应用镜像失败 rc=76 (The operation was aborted by the library user)`，误以为是只读介质或权限问题 ✗。真实原因：`ops.cpp::RunDirectRestore` 传给 `wim.Apply` 的进度回调写成 `return true;` —— 而 `wim.cpp` 的约定是 **返回 true = 请求 ABORT**（`(*c->fn)(pct,stage) ? WIMLIB_PROGRESS_STATUS_ABORT : CONTINUE`）→ **第一次回调就中止** ✗。**修复**：`return false;`（继续），并在该处写明语义。**为什么一直没发现**：就地还原此前**从未跑通**（`docs/07` 一直标"待实测"✓）；而"暂存 + 重启"路径的 apply 跑在 Linux 救援层里（不经过这个 C++ 回调 ✓），所以一直正常。**教训**：`bool` 型回调的"真=取消"这种约定极易写反 —— 注释里其实写了（`wim.h`），调用方没看；日后这类语义建议直接叫 `onProgressReturningAbort()` 之类，或改用枚举。✅ 2026-09-23

- PIT-076 **手动输入镜像路径时按钮不变蓝（GUI 只监听"浏览…"/拖入，不监听输入框变化）**（2026-09-23 用户实测）：备份模式在"浏览保存位置"右侧输入框里**手打**路径/文件名 → "开始备份系统"仍是灰的 ✗，必须点一次"浏览…"才变蓝。原因：`CMainForm::Notify` 只处理 CLICK / SELECTCHANGED / ITEMSELECT，**没有监听输入框文本变化** → `m_wimPath` 不同步、`UpdateMainAction()` 不触发。**修复**：在主窗口 `HandleMessage` 加 `WM_COMMAND` + `EN_CHANGE`（`CSkinEditUI` 内部是原生 EDIT，其变化通知送到父窗口 = 主窗口）→ 同步 `m_wimPath` + 刷新按钮；还原模式下若路径指向存在的文件，顺手解析子镜像（用 `m_lastLoadedWim` 防抖，避免每敲一键就重开 WIM）。✅ 2026-09-23

- PIT-077 **手动输入/粘贴的路径"鼠标一移开就消失"、按钮一直灰：原生 EDIT 子窗口与控件文本从不同步**（2026-09-23 用户实测）：在"系统→文件"页往"浏览保存位置"输入框**手动打字或粘贴**路径/文件名 → **鼠标一移开字符就没了** ✗，"开始备份系统"始终灰 ✗，必须点一次"浏览…"才能用（用户原话："别人复制粘贴一段路径和名字过来却无法使用"）。根因：`CSkinEditUI` 内部是**原生 EDIT 子窗口**（用户输入进的是它），而 `DoPaint` 在**无焦点时改用控件自绘**（`if (!IsFocused())`），自绘的是**控件自己的 `GetText()`** ✗ —— 两者从未同步 → 失焦后画出空的 ✓；按钮读的也是控件文本 ✓。"浏览"能用纯属巧合：它调 `SetText()` 写的正是**控件** ✓。**修复**：① `CSkinEditUI::SyncTextFromNative()`（`GetWindowTextW` 回读子窗口 → 不同则 `SetText`），在 `DoEvent` 里每次兜一次；② GUI 的 `EN_CHANGE` 处理器**直接读原生子窗口**文本（双保险：EN_CHANGE 时控件文本可能还没同步）。**教训（流程）**：本轮有两次"构建失败但照样提交"（`EnableEnvPrivilege` 拼错、`ui_skin.cpp` 漏 `<string>`）—— **提交前必须单独跑一次构建并确认通过，不要把构建和提交写进同一条命令**（失败也会继续往下走）。✅ 2026-09-23（`0.1.20`）

- PIT-078 **两处 GUI 显示/文案回归**（2026-09-23 用户实测，都在"就地还原"流程里）：① **跑的过程中进度百分比一直空着** ✗（只有完成时才显示 100% ✓）—— 根因：`SetProgress()` 只设置进度条（`CRoundProgressUI::SetValue`），**从不写 `PercentText`**；那个标签只在 `OnTaskComplete` 里写 → 中途为空，且上一轮的 `100%` 会**残留**（用户看到进度条旁一个像被压扁的 `%）` 的怪字符）。**修复**：`SetProgress()` 一并更新 `PercentText`（`>0` 写 `NN%`，`==0` 清空以抹掉残留）。② 就地还原完成提示写的是"**系统还原已完成，无需重启**" ✗ —— 用户会理解成"现在就能用还原好的系统"（实际必须**重启**才进得去 ✗）。**修复**：改为"**系统还原已完成，用时 …。重启后即可进入恢复的系统。**"。✅ 2026-09-23（`0.1.23`）

- ✨ **备份信息与默认文件名（2026-09-21 用户规格）**：备份模式下——
  · **文件名下方（备注框）预填**「`日期 + 系统类型 + 备份`」，例如
    `20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`（仅当用户尚未手输时预填；该值同时成为 WIM 里的子镜像名）；
  · **默认文件名**改为 `20260921Win10.19044备份.esd`（`日期 + Win<家族>.<build> + 备份`；保存对话框与自动命名两处都改了）。
  · 实现：新增 `src/common/sysinfo.{h,cpp}` 的 `DescribeRunningSystem()` —— 读
    `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` 的 ProductName / EditionID / DisplayVersion /
    CurrentBuildNumber / UBR 组合。**版别名走 `EditionID → 中文映射`**：实测有的系统 ProductName 是英文且带年份
    （"Windows 10 IoT Enterprise LTSC 2021"），而我们要的是"Windows 10 IoT 企业版 LTSC"；另 Win11 的
    ProductName 仍写 "Windows 10"，家族号以 build ≥ 22000 纠正。

- PIT-071 **任务执行中点关闭"完全没反应"：`Notify` 的 `m_busy` 早退把关闭点击吞成死代码，而唯一的取消通道只在析构函数里**（2026-09-20 用户实测踩到）：用户在朋友机器上测试备份、CPU 占用高想退出，**点右上角叉叉没有任何反应**，只能干等任务跑完。根因两处叠加：① `CMainForm::Notify` 开头就是 `if (m_busy) return;`，把 `CloseBtn` 的点击也一起吞掉 → 下面那段"忙时弹框拒绝"的分支**永远执行不到**（死代码）；② 真正能停任务的 `m_cancel = true` 全文件**只出现在析构函数**（`main_form.cpp` 头部）→ 关闭被拦 ⇒ 窗口不销毁 ⇒ 析构永不执行 ⇒ **取消信号永远设不上**。次要因素：worker 是默认线程优先级 + wimlib 吃满所有核 → 界面点击发飘。**修复**（用户规格：默认「继续等待」，可选「终止并退出」）：① `Notify` 把关闭按钮**提到 `m_busy` 早退之前**处理；② 新增 `AskBusyClose()`（`CConfirmDlg` 通用化为 `Ask2`：两按钮文案 + 默认项可指定；此处左=「继续等待」[默认，回车/ESC 均落安全项]、右=「终止并退出」）；③ 新增 `CancelAndExit()`：置取消位 → 保持消息泵地等 worker 收手（正常 <1s）→ `join()` → `CleanupIncompleteOutput()` 删除未写完的**临时文件** `<目标>.tmp`（**不是**最终路径：产物是原子写的，`wim.cpp::Capture` 先写 tmp、成功才改名 → 取消既不损坏旧同名镜像、也留不下半截成品）→ 关窗；10s 仍未停 → 兜底 `TerminateProcess` + `MoveFileEx(..., MOVEFILE_DELAY_UNTIL_REBOOT)` 登记开机删除；④ `OnTaskComplete` 见 `m_cancel` 只记日志、**不弹失败框**（否则会挡住关窗）；⑤ 两个 worker 入口 `SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL)`。✅ 2026-09-20（编译通过，版本 `0.1.4`；**待用户实测**：备份中点叉叉应立即弹框；选「终止并退出」应 1 秒内退出且半截镜像被删）

- PIT-079 **救援层模块裁剪：白名单里 `kernel/fs/` 是"整目录" → 把网络/集群/嵌入式文件系统全拉了进来；加 EXCLUDE + 键盘补收后 779→494 模块、initramfs 36.3→20.5MB（dist 54.2→38.5MB）**（2026-09-24 调研 + 实施）：
  · **调研方法（可复用）**：解出包内全部 4225 个 `.ko.xz` 的 `depends=` 建依赖图 → 模拟"白名单 + 闭包" → 对候选删除集做**反向依赖校验**（保留下来的模块是否依赖被删模块）。这一步是"不破坏兼容性"的唯一依据。
  · **关键事实**：① `fuse`、`atkbd`/`i8042`/`libps2`/`serio`/`input-core`、`fb_efi`/`fb_simple`/`framebuffer_console`/`vt` 在 Debian 内核里都是**内建**（`modules.builtin` + `/boot/config-*` 可证）→ 不需要对应模块，PS/2 键盘本来就可用；② **PIT-061 的"UEFI 黑屏"是 Alpine 内核特有的**（它 `SYSFB_SIMPLEFB=y` 顶掉内建 efifb），Debian 的 `FB_EFI=y` 直接接管 → **整条 DRM/KMS 链（含 `gpu/drm/tiny` 的 bochs/cirrus）可删**；③ `lpfc`/`qla2xxx`（在 PLAN 的"关键 31 项"里）的 `depends=` 含 `nvme-fc`/`nvmet-fc` → 必须**连带保留** `nvme-fc/nvme-fabrics/nvmet-fc/nvmet/configfs/nvme-auth/nvme-keyring`，否则破坏 31/31；④ iSCSI/FCoE offload 卡（bnx2fc/bnx2i/cxgb3i/cxgb4i/qedf/qedi/qla4xxx）会经闭包拉进 `drivers/net`+`infiniband`+`target`+`libfc` 一大串 → 删卡即删整串。
  · **实施**：`build-debian-rescue.py` 新增 `EXCLUDE_PREFIXES`（对白名单**和**闭包**同时**生效）+ `EXTRA_KEEP`（USB HID：`hid`/`hid-generic`/`usbhid`，让救援 `#` shell 能用 USB 键盘）+ **构建期断言**：① 被排除却被保留模块需要 → 直接 `raise`；② 保留模块的依赖缺失 → `raise`。fail-fast，杜绝"静默产出缺依赖的坏包"。
  · **结果**：779 → **494** 模块；33.48 → 14.10 MB（`.ko.gz`）；initramfs 36.32 → **20.47 MB**；dist 54.17 → **≈38.5 MB**。（2026-10-06 起模块改存 Debian 原生 `.ko.xz`、initramfs 15.2MB，见 PIT-125）
  · **有意保留**（判断项，用户 2026-09-24 拍板）：`xfs/btrfs/bcachefs/f2fs`（镜像放这些分区时用得到）、`ext4`、FC HBA（维持 31/31）。
  · **附带发现（未处理）**：`kernel/drivers/ufs/` 从来不在白名单 → `ufshcd-core` 一直没进救援层（x86 Windows 上罕见，暂不加；init 里 `ldmod ufshcd-core` 一直静默降级）。
  · ✅ 2026-09-24（构建断言通过 + 产物自检：`hid`/`usbhid`/`hid-generic` 与全部关键存储/fs 模块 present，`nfs`/`cifs`/`ocfs2`/`kvm`/`ib_core`/`drm` 等 absent；**待 QEMU/VM 回归**）

- PIT-080 **裁剪后的完整回归：BIOS 端到端还原演练跑通；顺带修好三个已失效的测试资产**（2026-09-24）：PIT-079 删模块后，除"启动能起来"外还须证明"**真能还原**"。逐项验证：① 构建期断言 ✓；② `modules.dep` 一致性（494 行、0 悬空、0 缺失；`modules.builtin`/`modules.order` 在）✓；③ 产物自检（关键模块 present、被删项 absent）✓；④ UEFI SB 链（`uefi-ubuntu-smoke`）✓；⑤ UEFI 控制台（`uefi-screen`，见下）✓；⑥ UEFI 非 SB 直启（`uefi-smoke`）✓；⑦ 固件 `Boot####` 直启（`uefi-bootentry-smoke`）✓；⑧ BIOS/GRUB4DOS（新 `bios-smoke.ps1`）✓；⑨ **端到端还原演练**（`mk-drill.py` + `run-drill.ps1`）：`apply done` → `bootfix installed` → `boot region OK (0x54=fa33c08e, sector1 non-empty)` → `RESTORE DONE: /dev/sda2` → `reboot: Restarting system` ✓（证明 `mkntfs`/`wimlib apply`/PBR 所需的模块都在）。
  · **修的三个测试资产**：① `uefi-screen.ps1` 原用"两次截图字节是否不同"判断控制台 —— 裁剪后救援启动更快、30s 时已停在静态 `#` → **误报 FAIL**；改为**确定性串口断言**（`efifb: probing` + `Console: switching to colour frame buffer device` + `fb0: EFI VGA frame buffer device`），顺带证明**删掉 DRM/KMS 后 UEFI 控制台仍正常**（`CONFIG_FB_EFI/FB_SIMPLE=y` 内建接管，efifb 绑定）。② `mk-drill.sh`/`mk-testdisk.sh` 都引用了**已删除的 `bootfiles/restore.sh`**（`set -e` 直接中止）→ 删掉该行；`mk-drill.sh` 改为转调新写的 `mk-drill.py`。③ **`mformat` 多分区坑**：`mformat -i img@@off` 用**文件大小**决定卷大小 → 对多分区镜像会把整盘当成一个卷，三个分区 FAT 相互覆盖（症状：sda2/sda3 变 `non DOS media`、sda1 的 `grldr/menu.lst` 凭空消失）→ **正解：每个分区单独建精确大小的镜像 → mformat/mcopy → 再拼回磁盘**（`mk-drill.py`）。另：旧脚本注释的 sda3 偏移 `822704128` 是**错的**（正确 = `1606848*512 = 822706176`），排查时被它误导过一轮。
  · 新增 `tools/vmtest/bios-smoke.ps1`（自建 512MB FAT32 活动盘 + QEMU `-boot c`，确定性串口断言）。✅ 2026-09-24（全部 PASS；真机/VMware 实测仍待用户）

- PIT-081 **单元测试（`make check`）第一次运行就抓到真 bug：`sysinfo` 的 `shortTag` 用 `swprintf("%s")` → 默认文件名变成 `Win10.1`（PIT-007 重演）**（2026-09-24）：实现 PLAN §12 第 3 项的**零依赖单元测试**（`tests/`：自写微框架 `tiny_test.h` + `make check`，不引 gtest）。为可测，把**纯逻辑从 Windows 调用里抽出来**：`ComposeSystemDescription(RawSysInfo)`（sysinfo）、`DefaultExclusionConfig/BuildExclusionContent/CloudFolderNames`（exclude）、`BuildTaskConf/BuildTaskJson/BuildRestoreLogText`（task 契约文本）、`Crc32`（zip）。**首次运行 4 个用例失败**：`shortTag` 得 `Win10.1` 而非 `Win10.19044` —— 根因 `swprintf(tag, 32, L"Win%d.%s", family, build.c_str())`，MinGW 下 `%s` 当**窄**串（PIT-007）→ `build` 只剩首字符。**影响**：备份模式默认文件名（`<日期>Win10.<build>备份.esd`）一直是错的（`Win10.1`）。修 `%ls`。**附带收益**：`exclude` 测试里加了 **PIT-056 回归守卫**（默认排除清单绝不能含 `.LOG1/.LOG2/regtrans-ms/NTUSER.DAT/TM.blf`）。✅ 2026-09-24（17 C++ 用例 / 57 断言 + 13 Python 断言全绿；`0.3.1`）

- PIT-082 **启动器未嵌 `requireAdministrator` → 在 UAC 开启的机器上 `CreateProcessW` 真程序必失败（740）**（2026-09-24 用户 VM 实测）：新的发布形态「x86 启动器 + `x86/` + `x64/`」上线后，**Win7 x86（UAC 关）正常**，但 **Win10 x64 双击启动器报「找不到或无法启动 …\x64\SysRecoverUI.exe」**（而 `x64/` 下文件其实都在）。根因：真程序清单是 **`requireAdministrator`**，而非提权的启动器用 **`CreateProcessW`** 启动它 → **`ERROR_ELEVATION_REQUIRED(740)`**（`CreateProcess` **不会**自动提权，只有 `ShellExecute … runas` 才会）。**修复**：给**两个启动器也嵌入 `requireAdministrator` 清单**（复用既有清单资源对象 `SysRecover_rc.o` / `SysRecoverUI_rc.o`，与真程序一致 → 双击时由加载器弹一次 UAC，之后 `CreateProcess` 子程序因父进程已提权而成功）；并在启动器错误框里带上 `GetLastError()` 错误码（740/2/126/193 一眼可判）。✅ 2026-09-24（`0.3.4`；**待用户 Win10 x64 复测**）

- PIT-083 **外部审查（"问题清单"）逐条核实与修复：3 项属实已修、1 项不成立、1 项低价值不改；修复中自己又踩一坑（已修）**（2026-09-26）：外部 AI 给了 13 条清单（2 高/4 中/7 低），**逐条核实后**处理如下。
  · **属实并修复**：
    ① **H-02 / M-01（真实失效组合）**：`hv_vmbus/hv_storvsc/pci-hyperv/xen-blkfront` **在镜像里但不在 `init` 主名单**；且旧兜底只在"**零分区**"时触发，而 `usb-storage/uas` 在主名单 → **插着 U 盘 `list_parts` 就非空 → 兜底永不触发** → Hyper-V/Xen 系统盘不可见时直接失败。**修复**：4 个模块加进主名单；兜底条件改为"**找不到任务日志**"（把日志扫描抽成 `scan_for_log()`，扫不到就全量 modprobe 再扫一遍）。
    ② **H-01 / M-02（静默失败）**：`ldmod` 把"内建"与"真缺失"都报成 `builtin or not bundled (ok)` → **UFS 驱动缺失被静默**。**修复**：失败时查 `modules.builtin`，区分 `builtin (ok)` / `WARN not bundled` / `not applicable (no such device)`。顺带删掉 `simpledrm/nvme-common/t10-pi/linear`（既非模块也非内建；详见下），`multipath` 改为真实模块名 `dm-multipath`。
    ③ **M-04**：PE 下 `CanRestoreInPlace()` 直接放行、**完全不试卷锁**。**修复**：PE 下也试锁，失败只 `LogWarn` 继续（不拒绝）。
    ④ **L 系列**：`history.jsonl` JSON 转义不全（补 `\n\t\r\b\f` 与 `\u00xx`）、`grub.cpp::CopyOne` 固定 512 栈缓冲（改按长度分配）、`build-debian-rescue.py` 的 `gpu/drm/tiny` 白名单与 EXCLUDE 重复（删白名单）、`ls|head -1` 改循环、`init` 重复注释。
  · **不成立（不改）**：**M-03** "MBR offset 兜底可能匹配错盘" —— `_disk` 名字解析错（如 `nvme0n1p`）时 `dd` 会失败 → `_off=0` → **安全地匹配不上并退出**，不会写错盘。仍**顺手加固**（先验扇区 0 的 `55aa`、用 sysfs 父目录取整盘名），几乎零成本且更正确。
  · **低价值/有风险（不改）**：**L-07** BitLocker 检测只认中/英关键词（非中英系统可能漏判）—— 目标用户是中文 Windows；改成语言无关要引 WMI/未公开 API，风险 > 收益。
  · **⚠️ 修复中自己踩的坑（已修）**：把 HV/Xen 加进主名单后，**普通机器上 `modprobe hv_vmbus` 报 `No such device`**，被新 FAIL 逻辑当成失败 → **每台普通机 8 行 FAIL 噪音**。补 `*"No such device"*` 分支降级为 `not applicable (no such device)`（硬件不存在属正常）。
  · 回归：`make check` ✓、`bios-smoke` ✓（日志无 FAIL；UFS 显示 WARN）、**BIOS 端到端演练 ✓**（`apply done`→`boot region OK`→`RESTORE DONE`）。✅ 2026-09-26（`0.3.11`）

- PIT-084 **借鉴 DreamGrain 电子教室的工程实践**（2026-09-26）：读了同总项目下 `DreamGrainClass/AGENTS.md`（Veyon 定制），只挑"**机器可校验 / 能防错**"的几条落地：
  · **崩溃处理（我们原本完全没有）**：新增 `src/common/crash.{h,cpp}` —— `SetUnhandledExceptionFilter` 捕获未处理异常 → `<exeDir>\logs\crash\` 写 **`MiniDumpWriteDump`**（**动态加载 `dbghelp.dll`，零链接依赖**）+ **可读文本**（异常码/地址/访问违例方向+地址/**调用栈地址+所属模块+偏移**/命令行/版本）+ `last.txt`；CLI/GUI 入口各装一次；`diag --zip` 会把 `logs\` 一起打包。回归 **`make crash-test`**（故意触发访问违例；实测 `.dmp` 41KB + `.txt` 均落盘）。
  · **"指针"一致性校验**：新增 `tools/check-docs.py`（接进 `make check`）—— 校验 `version.h` 的 SemVer、`SYSRECOVER_CONTRACT_VERSION` ↔ `AGENTS`/`docs` 的 `contract_version`、**救援脚本 `get_task` 读的键 ⊆ `task.cpp`（BuildTaskConf+BuildRestoreLogText）写的键**、`dist/version.json` ↔ `version.h`。当前 22 键写 / 8 键读，PASS。
  · **AGENTS 新增 §17**：开工序、编译/打包纪律（**只有 `dist/` 是交付物**）、**品牌与术语约束表**（九转/SysRecover/`zj*` 技术标识/「讨论」链接/维护者 —— 防改名漏网）、**AI 工具资源纪律**（并发搜索 2~4 封顶、能 Read 就不搜、少跑 PS/短命令）、**删除暂存区 `docs\老旧文档暂存\`**（只进不出）、崩溃处理说明。
  · **启动计时改环境变量**：`SYSRECOVER_STARTUP_TIMING=1`（原为编译期宏 `ZJ_LOG_STARTUP`，要重编）—— 学 DreamGrain 的 `DREAMGRAIN_STARTUP_TIMING`。
  · **没抄**：他们的教师端/学生端双端分包、上游 rebase 台账、CMake/Qt 工具链（与本项目无关）。
  · 回归：`make check` ✓（含 check-docs）、`make crash-test` ✓、`make package` ✓。✅ 2026-09-26（`0.3.12`）

---

- PIT-085 **第二轮外部评审（多角色专家组）收拢 7 项并落地**（2026-09-26，`0.3.13`）：8 个角色（安装部署/运维/测试/安全/兼容/UX/代码/文档）独立评审后交叉印证，收拢成 7 项按顺序实施（清单见 PLAN §13.4）：
  · **P1 网络镜像防呆**：还原**系统盘**要重启进救援层，而救援层**没有网络** → 镜像在 UNC/映射盘上必然失败（且可能已格式化目标）→ `src/app/ops.cpp::IsNetworkPath()`（`\\` 开头或 `DRIVE_REMOTE`）在**暂存前**拒绝并给明确提示（"先复制到本地分区"）。**实测**：`\\localhost\D$\...\test.wim` → 走到 `restore mode: staged reboot` 后 `staged restore rejected: image on network path`，**未写任务/未动 BCD** ✓。
  · **P2 契约版本握手**（设计里早有、实现一直没有）：`task.cpp` 的 `BuildTaskConf`/`BuildRestoreLogText` 补写 `contract_version=%d`；`zjrestore-lite.sh` 用 `ZJ_CONTRACT=1` 核对 `get_task contract_version`（**缺键视为 1**，兼容旧版）→ 不匹配 `say ERROR` + `exit 1`。**实测两条**：① 演练 conf 无该键 → `ZJ: contract_version=1 (ok)` 且还原照常；② 把演练 conf 的 `contract_version` 写成 **2**（模拟旧版任务）→ `ERROR: contract_version mismatch: task=2 rescue=1`，**`apply`/`mkntfs` 均未发生**（fail-fast 保护目标分区）✓。
  · **P3 CLI 自描述**：`<命令> --help` / `help <命令>` / 无参打印总览 + 退出码含义（`src/cli/main.cpp`）。
  · **P4 `dist/README.txt`**：面向使用者的说明（UTF-8 **带 BOM** + CRLF，双击记事本可读），`make package` 拷进 dist（文件名 ASCII，避免 Makefile 命令行中文坑）。
  · **P5 日志轮转**：`src/common/logger.cpp::PruneLogs()`（`SysRecover-*.log` 保留 14 天、`logs/crash/crash-*` 保留最近 30 个），`LogInit` 末尾调用。**实测**：60/30 天前的假日志被清、近期保留 ✓。
  · **P6 图标**：**美术图由产品维护者提供**（`resources/icon-source.psd` 原稿 + `resources/icon-source.ico` 256×256，粉金环形"九转"螺旋）；`tools/make-icon.py` 只负责**降采样成标准多尺寸**（256/128/64/48/32/24/16 → `resources/SysRecover.ico`）——**单尺寸 ICO 会被 GDI 粗暴缩放，任务栏/资源管理器小尺寸发糊**（无源图时脚本回退到内置造型：品牌蓝圆角方块 + 白色环形箭头）。**仅 `src/gui/SysRecoverUI.rc` 加 `1 ICON "SysRecover.ico"`**（与 `1 24` 清单**同 id 不同类型，不冲突**）；**`src/cli/SysRecover.rc` 不带图标（2026-09-26 用户裁定：dist 根目录两个 exe，只有 GUI 入口保留品牌图标，避免"哪个是入口"歧义；复检 CLI 应无 RT_GROUP_ICON）**，Makefile GUI 的 windres 加 `-I resources`；`main_win.cpp` 建窗后 `WM_SETICON`(ICON_BIG/SMALL)。**实测**：从 GUI exe 提取到 32×32 该图案 ✓。⚠️ 16px 偏糊（3D 光泽画通病）——如需更清晰可另做 ≤24px 的简化造型。
  · **P7 `make smoke`**：顺序跑 `bios-smoke`/`uefi-smoke`/`uefi-ubuntu-smoke`（**不进 `make check`**，耗时数分钟）。
  · **顺带修**：CLI 退出码映射 `return rc==4?4:(rc==5?5:1)` → `(rc==2||rc==4||rc==5)?rc:1`，让"参数/用法错"回到 §9 承诺的 **2**。
  · **⚠️ 本轮自己踩的坑（已修，教训记此）**：改完 `bootfiles/zjrestore-lite.sh` 后**忘了按 AGENTS §17.2 重建 initramfs** → 演练跑的是**旧脚本**，新加的握手行一直"看不见"，一度怀疑逻辑没执行 ✗。**规则重申：改救援层脚本 ⇒ 必须 `python tools/build-debian-rescue.py`，再回归**（本次重建后 494 模块/20.5MB 不变 ✓，握手行立即出现 ✓）。另：`tools/vmtest/parse-initramfs.py` 的用法是 `<archive> <mode> [target]`，用错参数会**静默无输出**（别误判成"内容不在镜像里"）。
  · 回归：`make check` ✓（17 用例/57 断言 + 13 Python + check-docs PASS）、`make crash-test` ✓（dump OK）、`make package` ✓（x64+x86）、BIOS 端到端演练 ✓（`apply done` → `boot region OK` → `RESTORE DONE`）。✅ 2026-09-26（`0.3.13`）

- PIT-086 **坏盘健康检测落地；NVMe 协议数据必须放 `query->AdditionalParameters`（偏移 8），否则 `ERROR_INVALID_PARAMETER(87)`**（2026-09-26，`0.3.15`）：用户把「坏盘/文件系统损坏检测」从"讨论后再做"划出（与代码签名/GUI 提取一并裁定，见 PLAN §13.4）——还原要**格式化目标分区**，若目标盘已现坏道，还原完系统照样起不来，必须**先警告再动手**。
  · **实现**：`disk.cpp::QueryDiskHealth()` 双路 —— ① **ATA/SATA**：`IOCTL_ATA_PASS_THROUGH` 发 `SMART READ DATA(0xD0)` 取属性 5/197/198 + `SMART RETURN STATUS(0xDA)` 判"驱动器自报即将故障"；② **NVMe**（Win10+）：`IOCTL_STORAGE_QUERY_PROPERTY` + `StorageDeviceProtocolSpecificProperty`（=50）+ `ProtocolTypeNvme`/`NVMeDataTypeLogPage`/`RequestValue=0x02` 读 SMART/Health 日志页（Critical Warning / Media Errors / Percentage Used / Temperature）。判定：`caution = ATA 自报故障 || 待定>0 || 无法纠正>0 || 重映射>100 || criticalWarning!=0 || mediaErrors>0`。
  · ⚠️ **坑（本次核心）**：**协议数据必须从 `query->AdditionalParameters`（= 偏移 8）开始**（MSDN "Working with NVMe Drives" 原文："The start of the STORAGE_PROTOCOL_SPECIFIC_DATA is the AdditionalParameters field"）。最初按 `sizeof(STORAGE_PROPERTY_QUERY)=12` 顺排、又试 9 —— **三种布局全被驱动以 `ERROR_INVALID_PARAMETER(87)` 拒绝**，而普通属性（`StorageAdapterProperty`/`StorageDeviceProperty` plain 查询）**都正常** → 一度怀疑"MinGW 把枚举值定错了"，**其实 50 是对的**（SDK：`StorageDeviceIoCapabilityProperty=48`，其后 adapter=49 / device=50；MinGW 头与 SDK 逐字一致），**错的是偏移**。输出侧同理：数据地址 = `&descriptor->ProtocolSpecificData + ProtocolDataOffset`（不是"描述符尾"硬算），MSDN 的校验是 `offset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)` 即拒。
  · ⚠️ **排查手法（可复用）**：① 先用 plain 属性验证 ioctl/句柄本身可用，再逐个换布局打印错误码（一次性探针，别在产品代码里试错）；② **直跑 `g++.exe` 必须先把 `mingw64\bin` 加进 PATH** —— 否则 `cc1plus` 报 `0xC0000135`（缺 DLL），**exit=1 且 stderr 为空**，极易误判成"源码语法错"（`mingw32-make` 路径下不会遇到，因为 make 会带 PATH）。
  · **展示三处**：GUI `AskDiskHealthWarning`（`CConfirmDlg::Ask2`，**默认"取消"=安全项**；静默模式跳过、只记日志）、CLI `restore` 暂存前打印、`diag` 每盘一行（`smart=ata|nvme|unavailable` + 关键指标）。取不到 SMART（USB 桥 / RAID / Win7 无 NVMe 属性）→ `smartKnown=false` **fail-open 放行**（与 BitLocker 同款"只警告不硬拦"，见 PIT-083③）。
  · **实测**（本机）：`disk0` SATA HDD → `smart=ata realloc=0 pending=0 uncorrect=0`；`disk1` NVMe → `smart=nvme crit=0x00 media=0 errlog=0 used=1% temp=46C`（数值合理）。⚠️ **未实测"真坏盘告警"**（本机盘都健康）——需拿有坏道的盘回归一次"弹框/打印确实出现"。
  · **附带（同批提交）**：产品中文名改 **「九转还原」**（原"九转一键还原"，用户 2026-09-26："可读性和分辨性更强"）—— 全仓 22 处 + 2 处"产品定义行"同步（皮肤 XML、窗口/对话框标题、README、docs、AGENTS §15/§17.3 品牌表），复检零残留；`九转还原` 四字标题更居中好看。✅ 2026-09-26（`0.3.15`）

- PIT-087 **热备份前置 VSS 检查（用户 2026-09-26 规格）：服务不可用时 wimlib 只报 `rc=89`，只字不提服务名**（2026-09-27 实测定位并落地）：备份靠 `WIMLIB_ADD_FLAG_SNAPSHOT` → Windows VSS 卷影快照。**实测三事实**（Win11 dev 机）：① `vss=禁用` 或 `swprv=禁用`（任一）→ 备份均失败 `备份失败(rc=89): Unable to create a filesystem snapshot`，**报错完全不提服务** → 用户无从下手（"为什么没有成功"的根源）；② Windows 默认「停止+手动」态 wimlib 自己能拉起，但**跑完不关**（vss+swprv 留 Running 残留，与"完成后关闭"规格不符）；③ `wimlib.h` 明文 VSS 快照不支持 WoW64（64 位系统必须 64 位程序——正常由 selfarch 自举保证，x64 缺失时兜底提示）。**实现**：新增 `src/common/vss.{h,cpp}`（`vss::BackupGuard`，纯 Win32 SCM API `OpenSCManagerW/OpenServiceW/StartServiceW/ControlService/QueryServiceStatusEx`，禁 WMI）；`RunBackup` 算出 `snapshot` 后（RegFlush 之前）调 `Ensure()`：VSS + swprv 逐个查——**缺服务/被禁用/启动失败/30s 超时 → err = 多行中文处理指引**（services.msc 与 `sc config X start= demand && sc start X` 两条路，附 rc=89 因果说明），阻断在动数据之前；「停止」则启动并记录原状态。**析构回滚**（覆盖所有退出路径，含失败路径）：只停「亲眼确认过备份前是停止、且现在在运行」的（=只关我们开的；备份前就在跑 → 不动）。**坑**：`ErrorAdvice` 的 `has("管理员")` 会命中提示词里的"管理员窗口" → 追加误导性建议"以管理员身份运行"（失败根本不是权限）→ 在 `ErrorAdvice` **最前**加 VSS 短路分支返回 `{}`。**回归 4 场景全过**：正常（停→启→备份成功→停回）/ vss 禁用（指引+状态原样+exit=1+无 wim）/ swprv 禁用（指引+**已拉起的 VSS 回滚停回**）/ 预运行（成功+保持运行不关）。`make check`/`make package` 绿。✅ 2026-09-27（`0.3.17`）

- PIT-088 **i18n 落地三坑（2026-09-27，全部已修 + 单测/冒烟覆盖）**：
  · ① **`--lang` 开关"完全没生效"**：`Utf8Args()`（`src/cli/main.cpp:207`）**已经丢掉 argv[0]**，而我写解析循环时按惯例 `for (i = 1; ...)` 起跳 → 正好跳过位于**首位**的 `--lang` → `langArg` 恒空、参数也没被吃掉（**日志 `cmd: --lang en help` 原样回显就是判据**：参数若被吃掉应只显示 `cmd: help`）。改 `for (i = 0; ...)`。**教训**：加命令行开关前先确认 `Utf8Args()` 的口径，别照搬"argv[0] 是程序名"的肌肉记忆。
  · ② **`.lang` 的键含 `=` 被从中间截断**：格式是 `key=value`，`备份失败(rc=%d): ` / `就地还原：应用镜像失败 rc=` / `打开目标卷失败 err=` 等 **9 个键本身含 `=`** → `line.find('=')` 命中键内部 → 键被截成半截、值带着剩下的键（症状：门禁同时报"缺键"9 个 + "死键"9 个 + printf 占位符不匹配，极像译文表写错，**其实格式本身有缺陷**）。**修复**：`lang_escape` 把 `=` 转义成 `\=`，分隔符改为**首个未转义的 `=`**，`i18n.cpp::Unescape/LoadTextLocked`、`wrap::lang_escape`、`check-i18n.py::split_entry` **三端同口径**；单测 `i18n_escaping_and_equals_split` 直接用 `备份失败(rc=%d): ` 当回归用例。⚠️ 只把 `=` 转义而不改分割逻辑是**无效**的（`\=` 里还是 `=`，照样被切）。
  · ③ **门禁 `load_lang` 用了 `strip()`** → 把键的**前导/尾随空格**吃掉（`' 备份'`→`'备份'`、`'    已用 '`→`'已用 '`）→ 报"重复键"假阳性。`.lang` 的键值**可以带空格**，必须与 C++ 解析器一致：只去尾部 `\r`、空行/首字符 `#` 跳过，**绝不 strip**。
  · **冒烟口径（可复用）**：CLI —— `dist\x64\SysRecover.exe help`（应中文）/ `--lang en help` / `set SYSRECOVER_LANG=en && ... help`（后两者应英文），看首行 `SysRecover · Command Line` vs `SysRecover 九转还原 · 命令行`；GUI —— 起 `SysRecoverUI.exe` 读 `Process.MainWindowTitle`：zh=`九转还原` / en=`SysRecover`（读之前 `[Console]::OutputEncoding = UTF8`，否则控制台按 GBK 打出来像乱码）。✅ 2026-09-27（`make check` 24 用例/148 断言 + `make package` 绿，双语冒烟通过）

- PIT-089 **Duilib 的悬浮提示（tooltip）在我们配置下**永远不显示**，根因是 `cbSize` 传了 `sizeof(TOOLINFO)`：MinGW 的 `TTTOOLINFOW` 带一个 v6 才有的 `void* lpReserved`（x64 下 `sizeof`=**72**），而本 exe **无 comctl32 v6 清单**（PIT-018）→ 跑 **v5**，它只认 `TTTOOLINFOW_V1_SIZE`(56)/`V2_SIZE`(64) → `TTM_ADDTOOL` 返回 **FALSE**、`TTM_GETTOOLCOUNT`=0 → 窗口建了但没工具、永不显示（2026-09-27/28 定位）**：
  · **症状极具误导性**：`WM_MOUSEHOVER` 到达（`msg=673`）、控件 `GetToolTip()` 非空（`tip=32`）、`CreateWindowEx` 成功（`GetTooltipWindow()` 有句柄）—— 三层都"正常"，但 tooltip 窗口 `IsWindowVisible=0`、`rect=(0,0)-(0,0)`、`toolcount=0`，屏幕无气泡。极易误判为"光标没停够久""DPI 不对""消息被 UIPI 拦"。
  · **两层叠加（都要修）**：① `cbSize`（v5 拒绝 72）；② Duilib 的 `m_ToolTip.uFlags = TTF_IDISHWND` **没有 `TTF_SUBCLASS`**，且全库**从不发 `TTM_RELAYEVENT`** → 即使注册成功也收不到鼠标消息（MSDN：不设 `TTF_SUBCLASS` 就必须自己 relay）。
  · **修复（选"自管 tooltip"，不动 third_party）**：`CMainForm` 里自己 `CreateWindowEx(TOOLTIPS_CLASS,…)` + `cbSize=TTTOOLINFO_V1_SIZE` + `uFlags=0`（矩形工具）+ `uId=1`；`WM_MOUSEMOVE` 时把命中控件的 `GetPos()`/`GetToolTip()` 写进 `TOOLINFO`、`TTM_SETTOOLINFO`，再 `TTM_RELAYEVENT`（**relay 必须在更新之后**，否则命中测试用的是旧矩形）；移到无提示区域时清空 `rect` + `TTM_POP`；`WM_MOUSELEAVE` 时 `TTM_POP`；析构里 `DestroyWindow`。
  · **诊断手法（可复用，一步到位）**：`TTM_ADDTOOL` 后立刻 `SendMessage(hTip, TTM_GETTOOLCOUNT)` 打日志 —— `added=0 / toolcount=0` 就是没注册上，别再去调光标停留时间/DPI。`TTM_GETTOOLCOUNT = WM_USER+13`（A/W 同号），`sizeof` 用 `std::to_string(sizeof(TOOLINFO))` 一起打，72 vs 56 一眼看穿。
  · ⚠️ 想直接改 Duilib 也行（`m_ToolTip.cbSize = TTTOOLINFO_V1_SIZE;` + `uFlags |= TTF_SUBCLASS`），但那样得改 `third_party` 且 `TTM_TRACKPOSITION` 在没 `TTF_TRACK` 时是空操作（定位不可控）；**自管更可控**。
  · **冒烟口径（脚本已入库：`tools/ui/tooltip-smoke.ps1`，`powershell -File tools\ui\tooltip-smoke.ps1`，退出码 0/1）**：DPI-aware 探针把光标**先移开再落到**控件上（同点 `SetCursorPos` 不产生 `WM_MOUSEMOVE` → 不触发 hover），等 ~1.6s，枚举 `tooltips_class32` 里 pid 命中且 `toolcount>0 && IsWindowVisible` 的窗口，抓其矩形；再移到空白处断言它消失。中英文各 3 个控件（`RepairBootBtn`/`Silent`/`BootMenuBtn`）全过。✅ 2026-09-28（`make check` 24 用例/148 断言 + `make package` 绿，`0.4.1`）

---

- PIT-090 **UEFI 引导写入的三个"静默失败"全部堵上（bcdboot rc / 空间 / 产物）+ 找不到 ESP 的回退 + `repair-boot` 一键修复**（2026-09-29，`0.4.2` 批次；分析全过程见 `docs/15`）：背景是机房学生机"还原后只剩『选择操作系统』空菜单"（判定为**镜像侧**，已停止深挖）与"DiskGenius 重建分区后找不到引导盘"（**我们代码的锅**）。逐条核实后确认 **5 处真缺陷**并修复：
  · **① bcdboot 返回码被丢弃**（`ops.cpp` 暂存 UEFI / 就地 UEFI / 就地 BIOS 三处只 `LogInfo`）→ 现在 `rc != 0` 即 `fail-closed` 并把 BFSVC 原文给用户（实测拿假 Windows 目录触发：`rc=193 尝试复制启动文件失败`）。
  · **② ESP 空间**：`InstallUefiBootEntry` 余量只有 **1MB**，且**先**拷我们 33MB 载荷、**后**跑 bcdboot → 空间被自己挤爆（实测满盘时 bcdboot `rc=112` 无声失败）→ 余量提到 **16MB**（bcdboot 实测需 ~8.6MB），并把**暂存流程改成"先 bcdboot、后放载荷"**。
  · **③ 没有产物断言**：新增 `boot/bcd_parse.{h,cpp}`（纯逻辑 `BcdEnumShowsOsEntry`）+ `uefi.cpp::VerifyEspBcd` —— 断言 BCD 里 `displayorder` 非空且有 `winload` 条目，且 `bootmgfw.efi`/`Resources\bootres.dll`/`Fonts\*_boot.ttf`/`zh-CN|en-US` 齐全（bcdboot "写一半"的特征）。**这是无论根因都能拦住"空菜单"的保险丝。**
  · **④ 找不到 ESP 只认精确类型 GUID**：新增 `disk.cpp::FindEspPartitionFallback`（**FAT 分区且根下有 `\EFI\Microsoft\Boot\bootmgfw.efi`** —— 固件只认路径不认类型 GUID）＋ `DescribePartitions()`（纯 ASCII 分区表摘要），报错文案改成能指导"漏建 ESP / 被标成 Basic Data"。
  · **⑤ 没有修复手段**：新增 CLI `SysRecover.exe repair-boot [--disk N --part M]`（找系统盘 → 找 ESP（含回退）→ 模板/空间预检 → bcdboot → 产物校验），**已坏的机器不必重装**。
  · 顺手：`extract --dest X:\`（**盘符根**）此前必报"无法创建输出目录"（`CreateDirectoryW("X:\\")` 失败）→ 现在把根目录视为已存在；失败时 `progress.json` 不再一律写 `percent=100`，改写**最后一个已知进度**。
  · 验证：`make check` 28 用例/178 断言 + `check-i18n`（**322** keys）全绿；`--lang en help repair-boot` 正确；`repair-boot` 负例带分区表摘要、`rc=1` 且**零写盘**；`extract --dest W:\` 实测成功。
  · ⚠️ **待做**：UEFI 分支的**机器侧回归**（本机 BIOS+MBR，走不到 ESP 分支）。✅ 2026-09-29

- PIT-091 **`make package`（双架构）必须把两个工具链的 `bin` 都放进 PATH，否则 x86 那半**静默**失败**（2026-09-29，本机重装 i686 工具链时踩到）：只把 `D:\Prog\ProgIDE\mingw64\bin` 加进 PATH 就跑去 `make package`，x86 侧第一条编译就挂：
  `D:/Prog/ProgIDE/mingw32/bin/i686-w64-mingw32-g++ … -c src/disk/disk.cpp -o build-x86/…` → `mingw32-make: *** [build-x86/app/disk/disk.o] Error 1`，**编译器一行诊断都不打**（`cc1plus` 找不到自己的 DLL，stderr 空）→ 极易误判成"源码有错"。实测把 `D:\Prog\ProgIDE\mingw32\bin` 也加进 PATH 后，同一条命令立刻成功、`make package` 双架构通过（PIT-086 记的是"直跑 `g++.exe`"的同款坑，这里是 **make 场景**）。
  · **i686 工具链来源**（本机 2026-09-29 装回）：mingw-builds（niXman）与 x64 **同源同版本** —— `i686-14.2.0-release-posix-dwarf-ucrt-rt_v12-rev0.7z`（88MB，`https://github.com/niXman/mingw-builds-binaries/releases/tag/14.2.0-rt_v12-rev0`），解压出的顶层目录就是 `mingw32/`，直接解到 `D:\Prog\ProgIDE\` 即可（本机 x64 也是 mingw-builds：`x86_64-posix-seh-rev0` + UCRT `rt_v12-rev0`）。
  · 验收：`i686-w64-mingw32-g++.exe --version` → `i686-posix-dwarf-rev0 … 14.2.0`；`objdump -f dist\SysRecover.exe` → `pei-i386`、`dist\x64\…` → `pei-x86-64`。✅ 2026-09-29

---

- PIT-092 **UEFI 固件 + MBR 盘（CSM/Legacy 装的 Windows）被误判成 UEFI 引导 → "未找到 ESP 分区"（0.4.1 起一直存在，2026-09-29 才修）**：用户在 **Win10 x64 + MBR 分区**上还原，直接失败并把截图存成 `C:\2222.png`：**"暂存失败 / UEFI 机器未找到 ESP 分区，无法部署引导层"**。
  · **根因**：`ops.cpp` 的引导链判据用的是 `disk.cpp::IsUefiFirmware()`（= `GetFirmwareType()`，只看**平台**是不是 UEFI，**不看当前是怎么启动的**）。"UEFI 平台 + legacy 引导的 MBR 系统"很常见（OEM 预装、克隆盘、GPT→MBR 转换、在 Legacy 模式下装的系统），这类机器**根本没有 ESP**，于是：① 暂存：`FindEspPartition` 失败 → abort（0.4.3 后文案变成带分区表摘要的"未找到可用的 ESP 引导分区"，但**依然失败**）；② 就地还原：同样走错链。
  · **修法**：**按"目标磁盘分区风格"选引导链**，而不是固件类型 —— 新增 `src/boot/bootpath.{h,cpp}`（纯逻辑 `ShouldUseUefiBoot(firmwareIsUefi, diskStyle)`：**GPT→UEFI/ESP、MBR→BIOS/GRUB4DOS**、风格未知→退回固件类型）+ `ops.cpp::UseUefiBootFor(target)`；暂存/就地/`repair-boot` 三处判据、以及"单次启动"（`BootNext` vs `bcdsequence`）全部改用同一判据，并新增日志 `boot path: UEFI/ESP | BIOS/GRUB4DOS (firmware=…, target disk style=…)` 便于现场判断。
  · **回归**：单测 `boot_path_choice`（UEFI+MBR / BIOS+MBR / GPT×2 / Unknown×2），`make check` **29 用例 / 184 断言**。✅ 2026-09-29
  · ⚠️ **待做**：真机（UEFI 固件 + MBR 盘）跑一次暂存→重启，确认日志是 `boot path: BIOS/GRUB4DOS` 且能进救援层。

- PIT-093 **「安装菜单」原来只装"恢复环境"、不绑镜像 —— 用户以为装了却没内容（2026-09-29 用户反馈并定义新语义）**：用户在本机（MBR）与家里台式机（UEFI）都试过：**没选镜像时点「安装菜单」也提示安装成功**，于是问"都没有镜像，这是安装了个什么菜单？"。核实：`main_form.cpp::ToggleBootMenu` 只做"部署救援环境（内核+initramfs；BIOS 另加 `grldr`/`grldr.mbr`/`menu.lst`）+ 常驻启动项"，**完全不涉及镜像与目标分区**；而救援层找不到任务文件时直接 `no log and no conf, abort` → 落到 `#` shell（PIT-035/PIT-042）。即：单独点它装出来的入口"进去也没活干"，只有"暂存+重启"那条路（`StageRestore` 写契约 + 单次启动）才真正还原。
  · **新语义（用户 2026-09-29 规格）**：菜单项 = **一键还原"当前选中的镜像 → 第二步选中的目标分区"**。
    - 未选镜像（或镜像不可用）/ 备份模式 → 按钮**灰色**；已安装 → 文字变「删除菜单」；**换镜像 = 先删除再安装**（悬停提示写明当前绑定的镜像→分区）。
    - 点「安装菜单」弹确认框（3 行文案，明确"选该菜单项会格式化该分区"），确认后写入常驻契约 + 常驻启动项（**不设单次启动、不重启**，留给用户在开机菜单里自己选）。
  · **实现**：`ops.cpp` 把 `StageRestore` 重构为 `StageRestoreImpl(..., bool menuEntry)`，新增 `StageRestoreMenu`（共用全部安全检查/契约写入/引导层部署，差别只在"是否设单次启动"）；`task.cpp` 新增纯逻辑 `TaskConfGet()` + `ops.cpp::ReadMenuBinding()`（GUI 判断"绑了哪个镜像/是否换过"）；GUI `RefreshBootMenuBtn()` 改三态、`UpdateMainAction()` 末尾统一刷新（选镜像/选分区/刷新按钮一处不漏）。
  · **坑**：`CConfirmDlg` 正文**只显示 3 行、单行标签超宽会静默裁切**（PIT-039）→ 确认文案必须压成 3 行短句（第一版 7 行被裁了一半）；回车默认项要与高亮按钮一致（右=动作=回车，ESC=取消）。
  · **回归**：单测 `task_conf_get`（含"值不去空格/前缀键不误命中/注释跳过"）；`make check` **30 用例 / 195 断言**；GUI 截图实测四态：未选镜像=灰、选镜像后=亮、已安装=「删除菜单」+悬停说明、确认框 3 行不裁。✅ 2026-09-29

- PIT-094 **常驻菜单装好了却"按什么键都选不了"：`{bootmgr} timeout=0` 时 bootmgr 不停留直接进 Windows**（2026-09-29 用户实测）：用户在笔记本（Win10 x64 MBR）与本机都装了「安装菜单」，重启后**无论按什么都进不去菜单**；手动 `bcdedit /set {bootmgr} timeout 30` 后成功进菜单、选中并**完整还原系统**（= PIT-093 的端到端真机验证通过 ✓）。
  · **timeout=0 的来源**：凡我们还原过的机器，目标 BCD 由 bootfix 生成且**故意写 timeout 0**（`bootfix.cpp`："启动菜单不等待"——单条目时无感知，一旦加上常驻恢复条目就不停留）；新装机器本身也可能就是 0。
  · **修复（用户 2026-09-29 规格：1 秒；"原来大于 1 就保留原值"）**：`StageRestoreMenu` 的 BCD 分支（BIOS + Secure Boot 的 bootapp 模式）装菜单时**只升不降**：原值 ≥1 一律保留、0/缺失补成 **1 秒**（1 秒足够用上下键选、几乎不拖慢开机）；设置失败 **fail-closed 中止**（菜单装上但选不了 = 静默坏，正是本次的坑）。**纯 UEFI 不碰** —— 其常驻项在固件 `BootOrder`（**开机按 F12** 选），与 bootmgr timeout 无关。
  · **实现**：`bcd.h::ParseBootmgrTimeout()`（纯逻辑、单测覆盖；**不依赖标签语言**——按"整行恰好两 token、第二 token 全数字且 ≤1 天"取值，兼容本地化/其它写法）+ `bcd.cpp::BcdGetBootmgrTimeout/BcdSetBootmgrTimeout`；`ops.cpp` 菜单分支调用；GUI 成功提示与悬停提示按 `UseUefiBootFor()`（已提到 `ops.h`，与 `StageRestoreImpl` 内部同一函数）分链措辞：BIOS=开机菜单选、UEFI=按 F12。
  · **顺带**：BCD 恢复条目描述由 `Tr(L"一键还原恢复环境")` 改为 **ASCII `SysRecover Recovery Environment`** —— bootmgr 菜单里中文描述在部分机器上显示成方框（PIT-054 实测），且与 UEFI bootapp 条目（desc `SysRecover`）、GUI 提示「选 SysRecover」三处一致（旧译文键随 skeleton 自动清除）。
  · **边界（有意不改）**：还原完成后目标 BCD 是 bootfix 的单条目 + timeout 0 → 菜单自动消失、开机不停留，无需回写；**删除菜单也不回写 timeout**（单条目不显示菜单、无感知；双系统用户自己的停留时间不被动）。已手动调到 30 的机器按"只升不降"继续 30，想改回 1 秒手跑 `bcdedit /set {bootmgr} timeout 1` 即可。
  · **回归**：单测 `bcd_parse_bootmgr_timeout`（英文输出/本地化标签/CR/无行尾/非数字/超限值跳过）→ `make check` **31 用例 / 204 断言**、`check-i18n` **328 keys**、`check-widths` OK（新 EN 状态文案曾超 638px 宽度门禁，已缩短）；`make package` 双架构。**待复测**：新构建在真机装菜单应自动把 0 → 1（日志 `bootmgr timeout: 0 -> 1s`），开机能用上下键选。✅ 2026-09-29

- PIT-095 **VMware EFI 下按 F12 后"黑屏 + 光标数分钟"不是死机：菜单逻辑已就绪、仅画面渲染极慢**（2026-09-29 VM 实测 + 用户定位，`0.5.0` 结案）：测试机 `初心Win10x64`（Workstation 17.6.1，UEFI+SecureBoot+NVMe）按 F12 后停在黑屏左上一个光标（截图 `1111.png`），**什么都不按要几分钟后才画出 Boot Manager 菜单**（`2222.png`：`Boot normally / Windows Boot Manager / EFI NVME / EFI SATA / EFI Network / SysRecover / Enter setup / …`，**无倒计时行**），极易误判死机（本会话最初即如此误判，自动化探测绕了很久）。
  · **真相（用户破案）**：黑屏光标阶段菜单**输入已在线** —— `回车` = 默认 `Boot normally` = 秒进原系统；**盲按 ↓ 导航到 `SysRecover` 回车 = 秒进还原**（用户实测光标态 6 次↓；完全画出的菜单里 `SysRecover` 是第 5 次↓ —— 光标态起点/项数与画出后略有出入，以实测为准）。慢的只有**渲染**，构建与输入都不慢。
  · **日志铁证**：`vmware.log` `2026-09-29T14:49:40 About to do EFI boot: SysRecover` → 固件成功加载条目 → 恢复全流程正常跑完（`mkntfs` + apply + 重启进新系统，用户确认"可以正常恢复"）。即恢复链路（F12 菜单列出条目 / 固件加载 / 救援层执行）全部验证 ✓。
  · **定性**：VMware Workstation EFI 固件的**菜单绘制延迟**（固件侧；UEFI 变量没有"菜单默认项/菜单超时"开关，我们改不了），**与产品无关** —— 真机（PIT-093/094）无此现象；理论上菜单构建只读 NVRAM 变量（毫秒级）。**可选未做**：摘 `Boot0003` 的 A/B 对照定量定责（需关机离线改 NVRAM + 人工按 F12）。
  · **规避（记录在案，日后排查不再当死机）**：F12 后不要等画面 —— 直接 ↓×N + 回车选 `SysRecover`；正常恢复走暂存 `BootNext` 主流程，根本不按 F12。✅ 2026-09-29

- PIT-096 **「镜像搜索」选第二项必闪退：自定义消息 `WM_PROGRESS_UPDATE = WM_APP+1` 与 Duilib 的内部异步通知泵撞车**（2026-09-30 用户两次实测崩溃，`0.5.1` 修复）：用户实测"搜索找到文件 → 点输入框下拉 → 选中第 2 个 → 程序立即退出"，崩溃报告 `0xC0000005 read address 0x8`，两次（`F:\...\logs` 与 `dist\logs`）**RVA 逐字节相同**（+0xB1E7 / +0xFDB2 / +0x77C44）。
  · **定位手法（可复用）**：崩溃转储 txt 有 RVA → `addr2line -a -f`（**PE 必须给完整 VMA = ImageBase 0x140000000 + RVA，给 RVA 只会输出 `??`**）+ `objdump -d --start-address=<VMA>` 反汇编 —— 一眼看出 `HandleMessage` 在 `call OnProgressUpdate` 之后，`OnProgressUpdate` 开头 `mov 0x8(%rdi),%rsi` 且 `%rdi`=lParam=**0**（`std::wstring` 的 size 字段 = +8 → "read 0x8"）。
  · **根因**：`src/gui/main_form.h` 曾把 `WM_PROGRESS_UPDATE` 定义为 `WM_APP+1`，而 **Duilib `CPaintManagerUI::PostAsyncNotify()`（UIManager.cpp:2870）也 `PostMessage(WM_APP+1, 0, 0)`**（供 `MessageHandler` 的 `case WM_APP+1` 消费，调用链在 `WindowImplBase::HandleMessage` 我们基类调用**更深处**）。我们先截获该消息并把 lParam=0 当 `wstring*` 解引用 → 空指针。触发源：下拉框第二次重建（`LoadWimImages` → `RemoveAll` 旧条目 → Duilib 延迟清理 `AddDelayedCleanup` → `PostAsyncNotify`）—— **正好解释"第一次填入（空框）不崩、一选中就崩"**。副作用：撞车期 Duilib 的异步通知**从未被泵过**（`m_bAsyncNotifyPosted` 卡 true）。
  · **修复（双保险）**：① 消息重编号 `WM_APP+10/11/12`（Duilib 仅用 +1；代码注释写明禁回 +1）；② `HandleMessage` 的进度分支加 `if (lParam)` 防御 —— 再有撞车/异常投递只 `LogWarn` 丢弃，**永不解引用**。
  · **教训**：自定义 `WM_APP+N` 前必须 grep 第三方 UI 库的 `WM_APP` 用法；另外本崩溃再次证明 `PIT-084` 崩溃报告的价值 —— 有 RVA + 反汇编就能离线定罪，不需要复现时挂调试器。✅ 2026-09-30

- PIT-097 **点输入框弹下拉后"文字跟着鼠标拖选、必须再点一下才恢复"：`EN_SETFOCUS` 里直接 `TrackPopupMenu` 吃掉了这次点击的 `WM_LBUTTONUP`，原生 EDIT 永远停在"左键按住"状态**（2026-09-30 用户实测反馈，`0.5.2` 修复）：选完镜像后，镜像名从鼠标位置开始处于选中态，鼠标左右拖动会继续扩大选区，像"左键没松开"（实际已松开）。
  · **机制**：原生 EDIT 在 `WM_LBUTTONDOWN` 处理链里 `SetFocus` → 同步发 `EN_SETFOCUS` 给父窗口 → 我们在此直接开模态菜单 → 菜单立即捕获鼠标，随后那次真实的 `WM_LBUTTONUP` 被菜单循环吞掉 → EDIT 内部 `fMouseDown` 永不复位（且菜单关闭后 capture 已归零，不会自动补），之后在文本上移动/拖动鼠标都被当成拖选，直到下一次完整的点击（DOWN+UP）才恢复。症状与"菜单窗口自己卡住"无关，**全部状态都在 EDIT 控件内部**。
  · **修复**：`EN_SETFOCUS` 只 `PostMessage(WM_OPEN_PICK_MENU)`（`WM_APP+13`，避让 Duilib 的 +1，PIT-096），处理时**先等左键抬起**（`GetAsyncKeyState(VK_LBUTTON)`，未抬起则 15ms 后重投），确认这次点击已完整结束再 `ShowImagePickMenu()`；并复查焦点仍在输入框（用户已点别处则不弹）。副作用排查：`m_pickGuard` 仍保留（菜单关闭后 `SetFocus` 的防重入），Tab 触发（无鼠标）时左键未按 → 立即弹出，与原行为一致。
  · **排查手法**：用户描述即典型"控件错过 UP"特征 —— 选区锚点=鼠标点击点、拖动即扩选、再点一下恢复；凡"模态菜单/对话框从鼠标事件处理链里同步弹出"都要过这一遍（同类：文件对话框从 EN_SETFOCUS 弹也会有此问题，本项目文件对话框走按钮点击不受影响）。✅ 2026-09-30

- ✨ **备份/恢复 ESP 分区（2026-09-30，方案 C「单文件双子镜像」，`0.6.0`，论坛 66 楼用户拍板）**：备份时可选把 ESP 作为**同一镜像的第 2 个子镜像**（子镜像名 `ESP` + 英文 description），还原系统时按契约 `esp_index` 从**同一个 `.wim/.esd`** 恢复到 ESP 分区。⚠️ 早期 A 方案（同目录同名 `.esp` sidecar，`0.5.3`）已废弃、实现已删除；导入时用户调研后拍板改 C（单文件传输/校验/复制都只管一个文件）。
  · **入口**：GUI 备份模式第三步 `EspChk`（`skin/main.xml`，复用隐藏的 SearchBtn 位 639,84,710,109，还原模式隐藏；无 group 属性 = 独立开关，与 `Silent` 互不取消）、CLI `backup --esp`；默认不勾选；悬停提示写明"写入镜像（子镜像名ESP）/ 恢复系统时自动恢复回 ESP"。
  · **原子性（`ops.cpp::RunBackup`）**：`stage = esp && !append`；主镜像先写 `<dest>.stage`（Capture 原子 tmp）→ esp 并入（新建 `WimEngine`，`Append(..., L"ESP", false, EnsureEspExclusionConfig(), …, kEspImageDesc)`；`ProgressUpdate("backup",100,"esp")`）→ `MoveFileEx(REPLACE|WRITE_THROUGH)` rename 到 `<dest>`；erc≠0 时先 `Probe(stage)`：坏 → 删 stage 报"主镜像写入损坏，已删除，请重新备份"，好 → 照常交付再报错。append 模式不 stage（原地追加，erc==-1 分支保留），失败报"（镜像写入损坏，请重新备份）"；esp 并入失败 tail = "（主镜像已生成，但未包含 ESP 备份）"；rename 失败 = `备份写出失败：无法提交镜像文件(err=N)`。无 ESP 仅 `LogWarn("esp backup skipped")` rc=0；失败路径不 AppendHistory（与既有 capture/probe 失败一致）。
  · **契约（增量键，`contract_version` 保持 1）**：`esp_index`（0/缺省=没有）写进 `restore-task.conf`（`image_index` 后）+ `restore-task.json` + `_zjresy*.log`；救援层早期 `ESP_INDEX=$(get_task esp_index)` + `case ''|0 → 置空`，另打一行 `say "esp subimage: index=…"`。老任务缺键=跳过、老救援层多键=忽略 → 双向兼容（`ZJ_CONTRACT` 不动，check-docs 仍 PASS）。
  · **还原防呆（`StageRestoreImpl` 步骤 0.5，Probe 之后、一切写盘之前）**：`ListImages` 后名字含 `L"ESP"`：① **== 还原目标 index**（eff = `req.index<1?1:req.index`）→ `err = Tr("子镜像「")+W2U(d.name)+Tr("」是 ESP 分区备份，…请选择系统子镜像。")` + `AppendHistory("restore-rejected", …, "esp image selected as restore target")` + **return 4**（CLI 透传 rc=4；GUI 走统一失败提示）；② 否则取首个含 ESP 的 index 当 `espIndex`（**从镜像内容发现，不依赖备份时是否勾选 --esp**），随任务写契约。`extract` 不拦（按路径提取是合法用途）。ListImages 失败仅 LogWarn、espIndex=0。
  · **恢复侧（`zjrestore-lite.sh`，改后已 `build-debian-rescue.py` 重建，494 模块/20.5MB 不变）**：sidecar 发现段整体删除；`find_esp_dev()` 不变（GPT 类型 GUID `c12a7328-…` 优先 → FAT+根有 `\EFI` 回退，跳过 TARGET/loop/ram）→ mount → **`$WIMLIB apply "$IMG_FILE" "$ESP_INDEX"`**（挂载中的镜像分区直读；`umount "$SRC_M"` 从原位置移到 ESP 块**之后**）→ 目录模式 apply 只新增/覆盖、不删 → 失败仅 `say ERROR` 不中断主流程。
  · **就地还原侧（`RunDirectRestore`，2026-09-30 用户指明补齐）**：PE / 还原到非系统盘**不走救援层**，原来这条路径漏了 ESP 恢复（tooltip 承诺"恢复系统时自动把ESP一并恢复"不成立）。补法：主 apply 完成后、**bcdboot 之前**，按 `espIndex`（0.5 步已从镜像内容发现）对 `AcquireEspRoot` 找到的本机 ESP 做目录模式 apply；**空进度回调**（`ProgressFn{}`，`ProgressThunk` 对空回调安全）不回卷已到 100% 的进度条；**非致命**——失败或找不到 ESP 只 `LogWarn`，主系统已还原不动摇（对齐救援层 `say ERROR` 不中断先例）；bcdboot 放在最后写、保证引导文件权威。BIOS 机器通常无 ESP → 自然跳过。
  · **⚠️ 坑（实测）**：① **`esp capture rc=47 Failed to open a file` 的真凶是 `\EFI\Microsoft\Boot\BCD`（+`.LOG*`）被运行中的 Windows 当 hive 挂着（`HKLM\BCD00000000`）独占锁**（PIT-051 同源）——**不是** `System Volume Information`（它读得到，A 方案时归因错过一次）；BCD 必须进排除清单（还原后由流程内 `bcdboot` 重建，PIT-090；且 apply 不删 → 现役 BCD 安全）。② wimscript 排除目录**不能带尾斜杠**：`\System Volume Information\` 的 glob 不匹配任何东西（wimlib/DISM 语义 = 匹配目录条目本身即跳过整棵），必须照 `exclude.cpp:26` 写 `\System Volume Information`。③ 排除配置走临时 `zjrestore-esp-exclusion.ini`（同 `EnsureExclusionConfig` 做法）；`wim.h` 不外泄 `wimlib.h` → 未初始化错误用 `-1` 特判。④ **wimlib 1.14 没有 `wimlib_get_image_count`** → 新子镜像 index 用 `wimlib_get_wim_info().image_count`；description API 拼写是 **`wimlib_set_image_descripton`**（少一个 i）。⑤ GUI 取消清理 `CleanupIncompleteOutput` 的候选路径要含 `<dest>.stage` / `<dest>.stage.tmp`（**绝不能删最终路径**：stage 已 rename 交付的主镜像是有效产物）。
  · **验证（全过）**：`make check` **31 用例/208 断言** + 341 i18n keys + check-docs PASS；`make package` 双架构；**Windows 真机冒烟**：`backup --esp` → rc=0、单文件 12.4MB、**无 `.esp`、无 `.stage/.stage.tmp` 残留**、`images` = `1 | Backup` + `2 | ESP`（英文 description 正确）；`--append --esp` → 4 子镜像（Backup/ESP/Backup(2)/ESP(2)）；`restore --index 2` → **rc=4 + 拒绝提示**、零契约写入（无 restore-task.conf）、history 有 `restore-rejected`；**QEMU 端到端演练**（`mk-drill.py` 改造：conf+log 带 `esp_index=2`、sda1 兼作假 ESP——根下 `EFI/` 命中 find_esp_dev 的 FAT 回退 + 预置 `EFI/DRILL-MARKER.txt` 验证只加不删、`test.wim` 用 `--esp` 重建）：`restore esp subimage idx=2 -> /dev/sda1` → `esp restored (rc=0)` → `RESTORE DONE: /dev/sda2 (index 1)` → `reboot: Restarting system`；盘上复核：**marker 幸存**（只加不删 ✓）、`EFI/Microsoft/Boot/bootmgfw.efi` 3,087,872B 落进假 ESP、`BCD` 不在（排除生效 ✓）。🚧 GUI 勾选框外观/文案待用户实测；UEFI 真机全链（暂存→重启→恢复 ESP 子镜像）待回归；**就地还原侧 ESP 恢复（PE/非系统盘）待实测**（就地还原本身此前就标"待实测"，见 PIT-064）。✅ 2026-09-30

- PIT-098 **备份 100% 后到完成弹窗之间是「静默期」：收官阶段只写 progress.json、不回调用户可见进度，GUI 状态栏冻在上一阶段**（2026-10-01 用户实测反馈）：非静默模式的完成弹窗本来就存在（`OnTaskComplete`），但用户在 100% 后等「很久」才弹出、状态栏一直停在「写入 100%」（只有"已用"时钟在走），怀疑是勾选了 ESP 备份。核实：① 主镜像 capture 到 100% 后**永远**还要跑 `Probe`（`wimlib_open_wim(CHECK_INTEGRITY)` = 全文件哈希扫描，**无回调**，大镜像几十秒），`--verify` 时另有 `engine.Verify`（同样无回调）—— 这才是静默期大头；② ESP 并入本身**有**进度（`Append` 传了 `progress`），但阶段名是 wimlib 的 `write`，看不出在做 ESP；③ `ProgressUpdate()` 只写 `progress.json`（外部工具看得到、用户看不到）；④ GUI 进度节流 `elapsed<100ms && pct<100` 会把 100% 后 100ms 内刚发出的阶段标记也吞掉。**修复**：① `ops.cpp::RunBackup` 收官各阶段补 `progress()` 回调（esp / verify / probe，含两处失败路径的 Probe）；② `WimEngine::Verify` 增加 `ProgressFn` 参数并注册回调，`ProgressThunk` 接 `WIMLIB_PROGRESS_MSG_VERIFY_STREAMS`（真实字节进度 + 速度/ETA）—— CLI `--verify` 与 `verify` 命令不再全程静默（`CmdVerify` 先手动 `ConsoleProgress(0,"verify")` 亮阶段名：open 扫描期注册不了回调）；③ `StageCn` 新增 esp/verify/probe → 中文（备份ESP / 校验镜像 / 完整性检查，前缀匹配兼容速度后缀）；④ 节流放行"阶段名变了"的首次上报（新成员 `m_lastStageRaw`，备份/还原两 worker 同款）。**顺手修**：`CmdBackup` 没设 `g_phase` → capture 期间 `ConsoleProgress` 把 `progress.json` 的 Phase 覆盖成 `idle`（补 `g_phase="backup"`）。✅ 2026-10-01（`make check` 31 用例/208 断言 + check-i18n **344 keys** + `make package` 双架构；`0.6.4`；GUI 100% 后状态栏依次显示 备份ESP→（校验镜像）→完整性检查 **待用户实测**）

- PIT-099 **救援层日志永远停在第一次快照：`find_soft_dir` 的缓存被命令替换丢进子 shell，后续 `persist_log` 全部失败**（2026-10-02 客户日志取证定位，`0.6.8`）：客户报"Linux 还原后 explorer 卡"，但发回的两份 `zjrestore-debug.log` 都停在 `found image`、没有任何 apply 记录 —— 一度无法判断卡在哪一步。读代码发现：`persist_log` 用 `_m=$(find_soft_dir)` 取挂载点，而命令替换跑在**子 shell**，函数里写的 `SOFT_MNT` 缓存赋值就地丢失；可函数成功时**已经把软件分区挂在 `/tmp/zj_soft` 且不卸载** → 之后每次 `persist_log` 重扫分区，重新挂载同一分区到同一挂载点必然 busy 失败 → 返回 "software dir not found"。于是**只有第一次快照落盘**（正好停在 `found image`），apply/失败/成功全都写不进去。**修复**：`find_soft_dir` 改为在**当前 shell** 直接设置全局 `SOFT_MNT` 并 `return 0`（调用方不再用 `$(...)`），缓存真正生效；并对失效挂载加 `[ -d ... ]` 复核。**同批"诊断底座"**（0.6.8）：① 新增**日志实时镜像** `start/stop_log_mirror`（后台每 2s 把完整 log + `apply.out` + `apply_io.log` 镜像到软件目录，EXIT trap/重启前停掉）；② 目标准备与扫描全阶段 `timeout`（blkid 30s / dd 30s / mkntfs 300s / mount·ntfsfix 60s），超时落 `dmesg`；③ 修 `mode=` 骗人 bug：dir 回退时置 `APPLY_MODE=dir` + `APPLY_FALLBACK=1`，汇总行如实打印 `mode=… fallback=…`，并写 `WARN: DEGRADED RESTORE` + `zjrestore-DEGRADED.txt`；④ 失败路径统一 `say_dmesg`；⑤ apply 失败打印 wimlib 输出**尾部**（`[ERROR]` 在末尾）+ `rc_name` 解码（46=NTFS_3G / 59=SET_SECURITY / 72=WRITE…），回退日志记录实际挂载类型（ntfs3/fuse）；⑥ 新增**内核 cmdline 实验钩子**（`zjapply=block|dir|dirfuse`、`zjfs=ntfs3|fuse|auto`、`zjtarget=format|keep`，见 `bootfiles/alpine/init`）+ `build-debian-rescue.py` 的 `ZJ_RESCUE_APPLY_MODE/ZJ_RESCUE_OUT`（把默认模式注入**打包副本**、输出到不同文件名）——用于"强迫降级"印证实验，**不影响默认流程/产线包**；已产出 `tools/vmtest/base/initramfs-dir-ntfs3.cpio.gz`（一级）与 `initramfs-dir-fuse.cpio.gz`（二级），QEMU 演练两种模式全链 PASS（日志 `mode=dir fs=ntfs3|fuse`、`apply rc=0 fallback=0`、`RESTORE DONE`，软件目录落盘完整）。**回归**：`mk-drill.py` 现在在演练盘 sda3 预置 `ZJRESTORE/` 软件目录，QEMU 演练后可从盘上读出**完整** `ZJRESTORE/logs/zjrestore-debug.log`（含 `log mirror: on`、`apply rc=0 elapsed=… mode=block fallback=0`、`RESTORE DONE`）+ `zjrestore-apply.out`（wimlib 原文）；BIOS 端到端演练全链 PASS。✅ 2026-10-02（`make check` 31 用例/208 断言 + check-docs/i18n/widths 全过；`0.6.8`）

- PIT-100 **SMART「即将故障」误报：`SmartReturnStatus` 读错寄存器（查了 [2]/[3]，应为 [3]/[4]）**（2026-10-02 客户实测反馈，`0.6.9`）：客户笔记本上「换了两块盘（含旧 SSD）都提示 SMART 阈值已超」，但第三方工具正常、只显示盘「清过零」。核实代码：ATA8-ACS 规定 SMART RETURN STATUS 的结果在 **Cylinder Low/High（`CurrentTaskFile[3]/[4]`）** —— 健康 = `0x4F/0xC2`、预测故障 = `0xF4/0x2C`；旧实现查的是 `[2]/[3]`（SectorNumber/CylinderLow）。在「控制器**不回填** CurrentTaskFile」的机器上，`[2]/[3]` 仍是请求时写的 `0x4F/0xC2` → **健康盘被判"阈值已超"**（部分控制器会回填/清零，所以本机 PIT-086 测试一直没暴露；本机现在实测 `st=unk srcl=0x00 srch=0x00`）。**修复**：按 `[3]`/`[4]` 判 OK/over，回读值既非 `0x4F/0xC2` 也非 `0xF4/0x2C` → 不下结论（`failingKnown=false`，fail-open）；`DiskHealth` 增加 `failingKnown/srCl/srCh`，`diag` 输出 `st=ok|over|unk` + 原始寄存器（现场一眼可证）。✅ 2026-10-02

- PIT-101 **备份 100% 后静默十几分钟才弹完成框：`Probe` 用 `open(CHECK_INTEGRITY)` = 全文件扫描且注册不了进度回调**（2026-10-02 客户反馈收拢 + PIT-098 收尾，`0.6.10`）：客户 44G 备份「显示百分百后等 12 分 14 秒才弹出完成」。根因：写出后必做的 `WimEngine::Probe` 走 `wimlib_open_wim(WIMLIB_OPEN_FLAG_CHECK_INTEGRITY)` —— 打开阶段还没拿到 `WIMStruct`、注册不了回调，GUI/CLI 只能停在上一阶段。**修复**：`Probe` 改两步 —— ① 普通 `open`（快）查 `write_in_progress`/`image_count`（保留 PIT-057 的"半截镜像"友好报错）；② `wimlib_verify_wim` 做全文件校验，**带 `VERIFY_STREAMS` 真实字节进度**；`RunBackup` 里 `--verify` 与默认后置检查**合并成一次**（以前两者都跑 = 双倍全扫）。还原暂存前的 Probe 同样受益（「校验镜像」现在有百分比）。CLI 实测：写出后 `verify 0%→35%→100%`。✅ 2026-10-02。⚠️ **2026-10-03 PIT-106 已删除自动全量校验**（用户实测"全测一遍无法忍受"）：`Probe` 只做秒级快检，全量校验仅 `--verify`/`verify` 显式触发。

- PIT-102 **降级（block 失败→目录写）默认关闭：实测会系统性篡改元数据，产出"能开机但语义不对"的系统**（2026-10-02 实验证据 + 用户裁定"做不到就直接报错"，`0.6.11`）：用**复刻客户布局**（GPT：MSR + **1GB ESP** + MSR + 目标分区 1200MB@偏移 1056MiB + 数据盘）在 QEMU 直启救援层，对含隐藏+系统/只读/ADS/显式 ACL/长名/junction 的测试镜像做三组对照：
  · **block（产线）全保真**：创建时间=备份时刻、Hidden/System/ReadOnly 保留、ADS `myzone` 在、ACL 显式 Administrators+SYSTEM、短名 `LONGFI~1.DAT`、junction tag `0xa0000003`；
  · **dir(ntfs3) 与 dir(FUSE) 全部丢失/篡改**：创建时间=还原时刻、Hidden/System/ReadOnly 丢失、ADS 丢失、**ACL 变 `Everyone 完全控制`**、8.3 短名丢失、junction 变 symlink（ntfs3）/普通文件（FUSE）。
  · 同时证明**"非标布局"不是 block 失败的原因**：同一布局 block 全绿（mkntfs + hidden sectors + `apply rc=0 fallback=0` + ESP 子镜像恢复 + RESTORE DONE）。
  **策略（最终，2026-10-02 调研后定稿）**：**彻底不提供降级方案** —— `block`（wimlib NTFS 卷模式经 libntfs-3g 直写）是唯一对用户开放的还原通道。同行方案对照：wimlib 官方 = `mkntfs` + `wimapply <wim> <idx> /dev/sdX`（NTFS 卷模式；目录模式明确写着"只在元数据不重要时用"）；Clonezilla/Rescuezilla = partclone/ntfsclone **块级**克隆；Windows 侧工具（DISM/WIMGAPI/傲梅/易数）= Win32 或自家驱动 —— **没有任何一家用"挂载目录逐文件写"还原 Windows 系统**（ntfs-3g 本身没问题，是"挂载目录写"这个通道没有 Windows 元数据概念）。故 `block` 失败即 **fail-closed**：报 rc/错误码 + `say_dmesg` + 完整日志落盘，提示改用 PE 就地还原；GUI/CLI **不提供**「允许降级」选项（0.6.12 曾短暂加入、0.6.13 移除）。`zjapply=dir|dirfuse`（内核 cmdline）仅保留为**实验室对照钩子**（复现 PIT-102 对比实验用，用户流程永远不可能走到）。另：`get_task/get_log` 取值统一去掉尾部 `\r`（防 CRLF 契约静默失配）。✅ 2026-10-02（`0.6.13`）

- PIT-103 **版本混淆：客户"换了新版"实际仍在跑旧救援层（日志又被 PIT-099 截断）→ 救援层加"构建版本戳"**（2026-10-03，`0.6.14`）：2026-10-03 用户日志再次停在 `found image`，据此判定为**旧版**（0.6.8 修复 PIT-099 后日志不会截断）。典型原因：只换 exe 不换 `bootfiles/`（救援 initramfs 来自 exe 目录树）、或仍点旧文件夹/旧快捷方式。以往日志头只有内核版本，**无法区分 Windows 侧与救援层各自的版本**。**修复**：`build-debian-rescue.py` 构建时把 `src/common/version.h` 的 `SYSRECOVER_VERSION` 写进 initramfs（`/zjrescue-version`），`bootfiles/alpine/init` 启动即 `say "rescue build <ver>"` —— 以后任何救援日志一眼可判救援层版本；配合 `SysRecover.exe version`（GUI 标题栏同名）即可确认整包一致。✅ 2026-10-03

- PIT-104 **运行目录搬迁：日志统一到「程序目录\logs」（用户 2026-10-03 规格，`0.6.15`）**：此前程序在光盘/写保护介质上时 **Windows 侧日志完全写不出**（logger "写失败静默忽略"），救援侧才回退 `<数据盘>\ZJRESTORE`，日志散落两三处、支持成本高。新规则（用户拍板）：
  · 正常（固定盘 / PE 的 X:）→ 原地运行；日志/契约/救援回写全部在 `<程序目录>\logs\`（旧回退仅在"程序在还原目标盘"时保留）；
  · **光盘 / U盘**（可写也弹）→ GUI 启动即询问（默认「复制并运行」、可「留在原处」）；**CLI 一律自动搬、不询问**；
  · 目的地 = **装了 Windows 的分区之外的第一顺序可写固定分区**（系统在 C: 选 D:、在 D: 选 C:；PE 下用 `\Windows\System32\winload.exe` 探测、同样避开离线 Windows 分区，防止搬进将来会被格式化的还原目标）；只有系统分区时退回它，GUI 文案注明"还原系统盘时程序与日志会被覆盖"；
  · 整包复制（跳过 `logs\`，目的地日志只增不减；覆盖前归零文件属性，PIT-067/068 教训）；复制完由原进程启动新副本（GUI 不等待、单实例互斥由副本接手；CLI 等待并透传退出码）；"盘符错乱找不到日志"不成立：**救援按分区扫描找 `software_dir`、不依赖盘符**（PIT-035/099）。
  · 实现：`src/common/relocate.{h,cpp}`（目的地选择为纯逻辑 `SelectRelocateDrive`，单测 `relocate_drive_choice`）；GUI 入口 `main_win.cpp`（复用 `CConfirmDlg::Ask2`，owner=nullptr，启动早期、单实例检查之后）、CLI 入口 `main.cpp`（`InitI18n` 之后、命令分发之前）。✅ 2026-10-03（`make check` 32 用例/214 断言、i18n 360 keys、check-widths OK；**待真实光盘/U盘实测**）

- PIT-105 **软件装在系统盘时日志会随系统盘一起被格式化 → 日志根改到数据盘**（2026-10-03 用户规格，`0.6.16`）：PIT-104 把日志统一到程序目录后暴露一个洞——程序装在 `C:\Program Files` 之类时，Windows 侧日志（`SysRecover-*.log`/`history.jsonl`/BCD 备份/`crash\` dump）全在 C:，而**还原/重装系统盘会格式化 C:** → 日志随程序一起消失（救援日志本来就会落 `D:\ZJRESTORE`，Windows 侧不会）。**修复**：新增 `LogBaseDir()`（`src/common/relocate.{h,cpp}`）——软件在**系统盘**上 → `<数据盘>\ZJRESTORE`（优先**不含 Windows** 的盘：PE 下避开离线 C:；优先 D:，其次第一块非系统固定盘；都不可写才退回程序目录）；其他位置 → 程序目录。GUI/CLI 的 `LogInit`/`ProgressInit`/`InstallCrashHandler`/`AppendHistory`/`diag --zip`/`history` 全部改走它，与救援层 `software_dir` 的既有回退（PIT-059）落在**同一个文件夹**；`diag` 新增 `log_dir=` 行。✅ 2026-10-03（`make check` 32 用例/214 断言；实测：程序放 C: 跑 CLI → 日志确实落 `D:\ZJRESTORE\logs`、C: 不产生 logs；**待"软件装系统盘 + 还原系统盘"实测**）

- PIT-106 **自动"全量校验"太慢 → 只保留秒级快检；全量校验仅显式 `--verify`/`verify` 触发**（2026-10-03 用户实测反馈，`0.6.17`）：PIT-101 把 `wimlib_verify_wim`（**全文件扫描**）并进了 `WimEngine::Probe`，于是**每次还原暂存前**都要先把整个镜像扫一遍（20GB+ 要十几分钟），备份收尾默认也全扫。用户实测 0.6.13 后明确："正式操作之前的测试镜像时间太长……全部测试一遍无法忍受，删除这种长时间测试"。**修复**：`Probe` 回到**秒级快检**（普通打开 + `write_in_progress` + `image_count`）——PIT-057 的"半截镜像"防护保留；备份收尾默认同样只快检（阶段标记改 `probe`）；**全量校验只在用户显式要求时跑**：CLI `backup --verify` 与 `verify` 命令（`WimEngine::Verify`，带 `VERIFY_STREAMS` 进度）。**取舍**：数据级损坏但未置"写入未完成"标记的镜像不再被自动拦住（概率低；有疑虑时显式 `verify` 一次）。还原暂存前的检查从此是毫秒级，不再出现"点开始后干等十几分钟"。✅ 2026-10-03（`make check` 32 用例/214 断言；**待用户实测确认提速**）

- PIT-107 **自诊断/痕迹收集全自动：启动即写 diag.txt/list.txt 到 logs、自动收集部署痕迹到 logs\collected（用户排错只管发 logs 文件夹）**（2026-10-03 用户规格，`0.6.18`）：此前让用户敲 `diag --zip`、`list`，还得满盘找我们留在盘根/`ZJRESTORE\` 的文件（grldr/menu.lst/_zjresy 日志/bootfix…），繁琐且容易漏。**修复**：新增 `src/app/selfdiag.{h,cpp}` ——
  · `WriteDiagFiles(logsDir)`：GUI/CLI 启动即把 `diag.txt`（固件/Secure Boot/启动项/工具路径/`log_dir=`/SMART 原始值）与 `list.txt`（磁盘/分区整表）写进 logs；`diag`/`list` 命令保留（同源实现，开发用）。
  · `CollectDeployArtifacts(dst, drive)`：启动扫所有固定盘、暂存完成后立即收目标盘——盘根 `grldr/grldr.mbr/menu.lst/restore-task.*/_zjresy*.log/zjrestore-boot.log` + `<盘>\ZJRESTORE\{bootfix,scripts,logs}` 子树（单文件 >32MB 只进清单）+ `ZJRESTORE-listing.txt` 整树清单 → `logs\collected\<盘>\`；覆盖时归零属性；**跳过等于日志根的那个 ZJRESTORE**（软件在系统盘时 = `<数据盘>\ZJRESTORE`，否则会把 logs 自己吞进去）。
  · 踩坑：`logs\collected` 顶层必须先 `CreateDirectory`，否则 `CreateDirectoryW(...\collected\D)` 因父目录不存在而失败 → 收集**静默全废**（实测抓出）。
  · 效果：用户排错只需发 **logs 文件夹**（+ 失败屏摄 / explorer 转储截图），`diag --zip` 不再需要用户敲。✅ 2026-10-03（`make check` 32 用例/214 断言；实测：盘根放 `zjrestore-boot.log`/`_zjresy*.log`/`ZJRESTORE\logs` → 启动自动收进 `logs\collected\D\` + 清单生成 + 日志行 `deploy artifacts collected: 4 file(s)`）

- PIT-108 **客户磁盘结构复刻 + 结构自检 PASS：排除"分区结构"是幽灵卡死的原因**（2026-10-03 排查用；**无产品代码改动**）：客户机（Win10 19044.1319）Linux 块模式还原后 explorer 首启动卡死（"重启 explorer 就好、开机复发"），怀疑点之一是它非常规的 GPT 布局。做法与结论：
  · 由 `list.txt` + 契约反推**精确到扇区**的布局：Netac 256G = ESP 1GiB **@LBA40** + MSR 16MiB + C: **@LBA2129960**(1090539520B)/497988199 扇区（C: 尾+1 正好 = 备份 GPT 起点 → 整盘 500118192 扇区）；数据盘 HKVSN 2T = D300G/E500G/F550G/G≈557.7G。
  · 新增工具：`tools/vmtest/mk-customer-layout.py`（diskpart 建动态 VHD → `\\.\PhysicalDriveN` 直写手搓 GPT + mtools 造 ESP 镜像；只写非零块，VHD 不膨胀）、`tools/vmtest/prep-cust-disks.ps1`（C:/数据分区快格 NTFS；drill 盘再放 `_zjresy` 契约到 C: 根）、`tools/vmtest/run-cust-drill.ps1`（OVMF → ESP `startup.nsh` → 救援内核 → 全链断言）。
  · **结果 6/6 PASS**：`found log (dev=/dev/sda3)` → `target from log location: /dev/sda3` → `mkntfs /dev/sda3 (start_lba=2129960)` → `hidden sectors = 2129960 (OK)` → `mode=block apply rc=0` → `esp restored (rc=0)` → `RESTORE DONE`（TCG 下 26s）→ **"结构导致还原异常"排除**（含 ESP@LBA40、C: 非 1MiB 对齐、C: 顶到备份 GPT 这些非常规点）。
  · 产物（供 VM 复现第三方场景）：`tools/vmtest/base/custdisk0.vmdk`（系统盘：ESP 空 FAT32 + C: 空 NTFS，结构同客户）+ `custdata.vmdk`（D/E/F/G 四个空 NTFS）+ 对应 `.vhd`；drill 盘 = `custdrill.vhd`。
  · 备注：同一 drill 有一次卡在内核 1.8s（宿主对刚重写过的 VHD 的瞬时 I/O 抢占），重跑即通过——与产品无关。

- PIT-109 **「日志」按钮 + `support` 命令：一键支持包（各盘日志 + explorer 转储 + 事件日志 + 安全模式引导）**（2026-10-03 用户规格，`0.6.19`）：排错时要发的东西太散，把"让用户做的事"全部自动化：
  · **GUI**：还原模式第三步新增「日志」按钮（静默模式与清除引导之间；静默模式左移到 x360 腾位），悬停提示"搜集还原日志与诊断信息并打包到桌面（含 explorer 转储、事件日志）"。点击后 worker 线程收集，完成后弹结果框：左「完成」（顺手用资源管理器选中刚导出的包）/ 右「重启进安全模式再测」（当前已在安全模式时右按钮变「退出安全模式并重启」）。⚠️ 结果框文案必须压成**3 行短句**（`CConfirmDlg` 只显示前 3 行，PIT-093；曾把安全模式问题拼在第 6 行导致整段没显示——`0.6.20` 修复）；按钮文字宽度上限：左 126px / 右 156px（GDI 实测「重启进安全模式再测」=126px 放得下，12 字版 168px 会裁）。
  · **支持包内容**（`src/app/selfdiag.cpp::BuildSupportBundle`）：①当前日志目录递归（含 `collected`）②**所有固定盘** `<X>:\ZJRESTORE\logs`（覆盖"单文件/temp 运行、日志落数据盘"）③**explorer 进程转储**（`common/crash.cpp` 新公开 `WriteProcessMiniDump`，MiniDumpNormal|WithThreadInfo = 含全部线程栈、体积小）④**事件日志**文本（wevtutil：Application Hang/Error、System 存储错误、System 错误级；PE 无 wevtutil 自动跳过）⑤diag.txt/list.txt/version.json/bundle-info.txt → `SysRecover-logs-<时间>.zip`。默认输出到**桌面**（`SHGetFolderPathW` 自动尊重重定向的桌面路径，实测本机落到 `E:\我的下载\DESKTOP.INI\桌面\`）；`--out` 可指定。
  · **安全模式**（`boot/bcd.cpp::BcdSetSafeBoot`）：`bcdedit /set {current} safeboot minimal` ⇄ `/deletevalue ...`；进入/退出两个方向都在，提示里带 BitLocker 警告（可能要恢复密钥）。注意 safeboot **持续有效**，必须靠按钮的第二方向退出（或用户 msconfig 取消）。
  · **CLI**：`support [--out <zip>]`（PE/批处理/开发用）与 `help support`。
  · ⚠️ **i18n 工具坑（本批实测）**：`src/app/selfdiag.cpp` 此前**不在** `tools/i18n-wrap.py` 的 `WRAP_FILES` 里 → `--skeleton` 扫不到它的词条，**还会把它们当死键从译文表删掉**（实测丢了 `ON (需签名引导)`、CA2023 警告两条）。已加入白名单；以后新增含 Tr 的源文件**必须同时加白名单**。
  · 实测：CLI `support` 产出 1.31MB 包（logs=23 / dumps=2 / events=4）；GUI 点击冒烟全链通过（日志有 `support bundle exported`）。✅ 2026-10-03（`make check` 32 用例/214 断言、i18n 379 keys、check-widths OK；`make package` **0.6.19**）

- PIT-110 **"暂存+重启"还原从不自动重启、还显示"还原完成"（元凶：`needReboot` 被无条件清零；自 ≤0.6.4 起就存在）**（2026-10-04 客户支持包日志定位，`0.6.21`）：客户 0.6.20 日志显示 `restore mode: staged reboot` + `restore staged, reboot to execute`，但 GUI 状态栏却是 **"还原完成（用时 00:01）"**、且**没有自动重启**（用户描述"点了开始后中途停止了"）。根因：`ops.cpp::StageRestoreImpl` 返回前一行 `if (needReboot) *needReboot = false;`（注释写"菜单项模式不重启"，但没有 `menuEntry` 条件）→ 暂存路径也被清成 false → GUI/CLI 走"就地完成"分支（弹"系统还原已完成…重启后即可进入恢复的系统"、**不调 `RebootNow()`**）。**0.6.4（已提交版）里就是同一行**，即所有"暂存+重启"还原实际都要用户**手动重启**；10-03 客户截图"还原完成（用时 01:04）"当时被误判成"就地还原"，实为暂存路径。**修复**：`if (menuEntry && needReboot) *needReboot = false;`（暂存保留 true ⇒ 提示"暂存完成（用时…），正在重启..." + `RebootNow()`，与 PIT-072 规格一致）。⚠️ 排查启示：`bool` 出参"默认由调用方初始化"时，被调方**无条件清零**等于废掉调用方默认值——此类语义建议显式命名（如 `outNeedReboot`）或改为枚举。✅ 2026-10-04（`make check` 32 用例/214 断言；`make package` **0.6.21**；**待真机复测自动重启**）

- PIT-111 **救援层「黑匣子」：任何失败都必须留下文件 + 开机即报 + 收集后清理 + 日志总量上限**（2026-10-04 客户三盘机"脚本快速 rc=1、屏幕外零证据"的教训；用户 5 条规格，`0.6.22`→`0.6.23`）：0.6.20 在那台机上救援脚本 rc=1 退出，console 之外**没有任何文件证据**（软件目录找不到就全丢），只能靠录像逐帧分析。改造（按用户规格 1–5）：
  · **探测表**（规格 1/2）：Windows 启动把盘/分区表写进主日志（`LogFileInfo("inventory: …")` —— 新增 `LogFileInfo` **只进文件不回显控制台**，避免污染 CLI stdout 与脚本解析）+ `collected\drive-map.txt`（盘符↔diskXpY/偏移/大小/serial 对照表，解决 PE 与正常系统盘符互相打架）；Linux 侧每个分区的 mount/open 结果进 `/tmp/zj-probe.txt` **且镜像进主日志**（`PROBE …` 行，init 与脚本共用）。
  · **多面落盘**（规格 3）：`zjrestore-lite.sh` 黑匣子把 `ZJRESTORE-last.log`（完整日志）+ `ZJRESTORE-probe.txt` + `ZJRESTORE-status.txt`（回执：result/step/target/image/build）+ mkntfs/apply/esp 输出写到**每个可挂载分区根**（失败全量扫；成功只写软件目录 + ESP，**不碰还原后的目标盘**）；ESP 有 `\EFI\ZJRESTORE` 时写进其 `logs\`。软件目录找不到时**尽早认领兜底日志窝** `ZJRESTORE-logs\`（非目标分区、保持挂载、持续镜像 2s，防中途断电无日志）；`/tmp/zj-bb.done` 记录已写设备，init 在"脚本没来得及写盘"时调 `zjrestore-lite.sh zz-blackbox <reason>` 兜底（逻辑单源）。**修掉误导消息**：原来无条件打印 `no restore task -> shell`；现在按真实结果打印 `RESULT: restore FAILED (rc=…)` + 日志位置（脚本 rc≠0 = 任务找到了但脚本失败）。
  · **目标校验防劫持**：采信"日志所在分区 = $1"前用契约 `target_part_offset` 校验设备实际偏移；不符（参照机曾出现 `target from log location: /dev/sda1`，被别处旧 `_zjresy` 劫持）→ 拒绝并回退 offset 精确匹配。镜像扫描加第二轮（3s 后重试，防枚举晚/瞬时挂载失败）。
  · **失败回执 + 开机即报**（规格 2）：`selfdiag::CheckLastRescueFailure()` 扫 `logs\collected` 里 `result=FAILED` 的回执 → GUI 启动弹 3 行提示（失败步骤中文化/时间/版本/日志位置），左「知道了」右「打开日志文件夹」（`WM_APP+15`；GUI/CLI 共用去重标记 `logs\.last-rescue-status`，同一回执只报一次）；CLI 启动在 stderr 打同样提示。
  · **收集后清理**（规格 4）：`support`/「日志」出包成功后 `CleanupStrayLogs()` 删各盘根黑匣子三件套 + `zjrestore-boot.log` + `ZJRESTORE-logs\` + 非活动 `<盘>\ZJRESTORE\logs\` + ESP `\EFI\ZJRESTORE\logs\*`（契约 `restore-task.*`/`_zjresy*` 与引导文件**不动**）。
  · **总量上限**（规格 5，二次修订）：`logger.cpp::PruneBySize()` —— logs 目录（含 crash/collected，深度≤3）超 **32MB** 触发：**先删一周以上的旧日志**；仍超 32MB 则最旧优先删到 **8MB 以内**；当前活动日志永不删（文本日志一次运行仅数百 KB，8MB 足够多次运行）。
  · **回归**：QEMU drill 成功路径（黑匣子进 ESP + 软件目录、`RESTORE DONE`、盘上 `ZJRESTORE-status.txt result=OK`）+ **造坏演练**（conf 改指向不存在的镜像）：`PROBE` 全表、`target … (offset verified)`、两轮扫镜像、`blackbox: log -> /dev/sda1|sda2|sda3`、init 打印 `RESULT: restore FAILED (rc=1)`，三个分区上均有 `ZJRESTORE-status.txt`（`result=FAILED step=image-not-found`）；Windows 侧合成回执冒烟（第一次提示/第二次静默/标记正确）。`make check` 32 用例/214 断言 + i18n **393** keys + check-docs PASS。✅ 2026-10-04（`0.6.22`–`0.6.24`；**待真机失败场景复测**）

- PIT-112 **三盘客户机全真复刻（MA 0902 2T MBR + ST1000 1T MBR(扩展分区) + KIOXIA 250G GPT）：端到端还原 6/6 PASS；复刻过程抓出并修掉两个真问题**（2026-10-04，`0.6.25`）：
  · **工具**：`tools/vmtest/mk-3disk-layout.py`（diskpart 动态 VHD + 手写 MBR/EBR/GPT + mtools 造 ESP；参数全取自支持包 list.txt/契约，精确到 LBA：目标 @2129960=1090539520B）+ `prep-3disk.ps1`（格式化 + 投放客户**原文契约**与 F: 的 `九转还原/` 软件目录+镜像）+ `run-3disk.ps1`（QEMU q35：ST1000=sda、MA=sdb、KIOXIA=nvme0n1；`ide-hd` 带 model/serial 复刻品牌；6 项断言；检测到"救援 shell"提前收工）。产物 `base/3disk/*.vhd`（动态 VHD，各 ~250MB）。
  · **验证**：0.6.25 全链 PASS（`found log nvme0n1p3` → `offset verified` → 镜像在 F: 找到 → `apply rc=0` → `esp restored` → `RESTORE DONE`）；0.6.20 的原失败**不可复现**（与"旧版/环境瞬态"一致，但下次任何复发都有黑匣子证据）；扩展分区逻辑盘在 Linux 下是 `sda5/sda6`、扩展容器有 `sda2` 节点——正好解释客户 list.txt 的绿色幽灵 `Part 0` 与屏幕上的 `sda2` 扫描行。
  · **修复 1（开发期回归，自捕）**：新加的"目标 offset 校验"最初只用 `blkid -s PART_ENTRY_OFFSET` + MBR 兜底 → **GPT 盘返回 0**（blkid 空、保护性 MBR 读出来是 1/0）→ 会把**正确目标**当 stale log 拒掉。改为**首选 sysfs `/sys/class/block/<p>/start`**（GPT/MBR/EBR 逻辑盘全准），blkid/MBR 仅兜底。教训：GPT 上任何"分区偏移"都不能依赖 MBR 结构。
  · **修复 2（静默卡死）**：`bootfiles/alpine/init` 的 `mnt_dev` 没有 timeout（`zjrestore-lite.sh` 版有）→ 复刻首跑在某个挂载上**静默卡死 400s**（与客户"卡住"同类症状）。已给 init 全部 mount/blkid 加 `timeout 30/60`，bootfix 扫描加逐盘日志与 `bootfix scan done` 标记——挂载挂死从此变成一条 FAIL 记录而不是死机。
  · ⚠️ 复刻机还实证："**给 Windows 手写 GPT 必须带保护性 MBR 的 `55AA` 签名**"（本脚本初版漏写 → Windows 认成 RAW；QEMU/Linux 不受影响）。
  · 回归：`make check` 32 用例/214 断言 + check-docs/i18n PASS；BIOS 演练（`mk-drill.py`+`run-drill.ps1`）在新 initramfs 下通过（`RESTORE DONE` + 黑匣子进 ESP）。✅ 2026-10-04（`0.6.25`）

- PIT-113 **真实镜像压测 + 小内存边界（三盘复刻环境，`0.6.25`/`0.6.26`）**（2026-10-04，用户要求"用我的本机备份镜像多测、刻意试小内存"）：
  · **真镜像全链**：Win10 ESD（7.57GB 未压缩、无 ESP 子镜像）8G 内存**全程通过**（apply 438s）；Win11 WIM（18GB 文件 → **43.35GB 未压缩** + ESP 子镜像）8G 内存**全程通过**（apply **1486s**、`esp restored (rc=0)`、ESP 回执 `result=OK step=done`）。
  · **小内存**：**512MB 无故障**（大 WIM 正常推进——PE 做不到这点）；**192MB 会 OOM**——wimlib 在 apply **最后阶段**（文件全写完、设安全描述符时）被内核 OOM 杀（rc=137，等于整盘白写）。
  · **失败留证（实测）**：`apply FAILED rc=137` + OOM dmesg 原文 + **三块盘每个分区**的 `ZJRESTORE-status.txt`（`result=FAILED step=apply-failed`）+ F: 软件目录完整日志（含 mirror）——"小内存故障是否被记录" 的答案是**全面记录**。
  · 新增**低内存预警**（`zjrestore-lite.sh`，`0.6.26`）：`MemTotal < 400MB` 时开机即打 `WARN: low memory … may be OOM-killed; increase VM RAM`，避免"整盘写完才失败还不知道原因"。
  · 工具：`mk-3disk-layout.py` 支持 `ZJ_3DISK_IMAGE/ZJ_3DISK_IMGNAME/ZJ_3DISK_ESP_INDEX` 换真实镜像；`run-3disk.ps1 -Mem/-Smp`；新增 `check-3disk.ps1`（挂载三盘打印回执与日志尾部，验收失败路径证据）。✅ 2026-10-04

- PIT-114 **契约清场 + 部署校验 + 控制器清单 + 设备排序（用户 2026-10-04 A2/A5/A6/A9/A1 五条规格，`0.6.27`）**：
  · **A2 契约清场（杜绝目标劫持）**：Windows 暂存写新契约前，`task.cpp::CleanupStrayContracts()` 清掉**所有固定盘/可移动盘根**上的 `_zjresy*.log` / `restore-task.conf|json`（只留即将写入的唯一一份；不碰 `<盘>\ZJRESTORE\` 与软件目录）；Linux `init::scan_for_log` 改为**优先采用"日志里 target_part_offset == 该分区 sysfs start"的那份**（多份时不再挑错），无匹配才回退第一份并 `WARN`。script 的 offset 校验保留为第二层。
  · **A6 旧版救援混用杜绝**：BIOS/GRUB4DOS（`grub.cpp::CopyBootFiles`）与 UEFI/ESP（`uefi.cpp::InstallUefiBootEntry`）部署后**逐文件 CRC32 校验**（源 vs 落盘，不一致即部署失败）；两处均落**明文构建戳 `<盘>\ZJRESTORE\rescue-build.txt`**（= `SYSRECOVER_VERSION`；收集器会收进 `logs\collected`，现场一眼核对盘上救援版本）。
  · **A5 控制器清单**：Linux 救援层开机打印 `storage/usb controllers (pci)`（读 `/sys/bus/pci` 的 vendor:device / class / 已绑定驱动，零新依赖）；Windows `diag` 增加 `pci-ctrl` 行 —— ⚠️ `Enum\PCI` 受 ACL 保护（**管理员也读不到**，实测），改走 `Control\Class\{SCSIAdapter,HDC,USB}` 读 DriverDesc/Service/MatchingDeviceId。以后"是不是缺/错驱动"可直接对照。
  · **A9 U 盘/光盘可感知**：`list_parts`（init 与脚本）改为 **内置盘分区 → 可移动盘分区 → 无分区的可移动整盘（光盘/未分区 U 盘）** 顺序；probe 行加 `rem=0/1`；`find_esp_dev` 跳过可移动盘（U 盘上的 ESP 类型分区不再被当成目标机 ESP）；`scan_image` 命中时记录设备号+rem。**顺带修了一个真缺口**：此前 `list_parts` 只取"有分区的设备"，`sr0`/未分区 U 盘实际扫不到（"镜像可放光盘"在代码上并不成立）；现在它们排在最后但会被扫描。
  · **A1 重试分级**：目标按 offset 匹配失败 → 3s 后重试一轮；镜像扫描第二轮 3s，**仅当出现过挂载失败**再第三轮 10s（`/tmp/zj_img.fail` 标记）——覆盖瞬时 I/O 错误恢复/设备晚到，避免空等。
  · 回归：BIOS 演练（`SR: found log ... offset match 105906176` + PCI 清单 + `rem=0`）✓；三盘客户复刻 6/6 PASS（`offset match 1090539520`、PCI 列出 `nvme`/`ahci`）✓；Windows `diag` 实测输出 `pci-ctrl`（NVMe/SATA/USB）✓；`make check` 32 用例/214 断言 + check-docs/i18n PASS。
  · **复刻验证"第2盘分区挂不上"假设（2026-10-04 追加）**：`qemu-io` 把 ST1000 的 F: 分区引导扇区清零模拟"分区挂不上" → 0.6.27 完整留证：`mount FAILED /dev/sdb5 ... NTFS signature is missing`（probe 含原文）、`status=FAILED step=image-not-found` 写满所有可挂载分区 + ESP、`fallback log home` 生效、init 打印真实结果——**同一故障在 0.6.20 只会剩看不见的屏幕行**。
  · **复刻验证 A2 契约选择（2026-10-04 追加）**：目标根同时放 `_zjresy-fake.log`（offset 不符、字母序在前）+ 真契约 → init 仍选中真契约（`offset match 1090539520`、无 fallback 警告），全链 6/6 PASS（若挑错，offset=999999999 会直接 target-not-found，故具备判别力）。
  · **R4**（`0.6.28`）：GUI `RebootNow` 的 `ExitWindowsEx` 与 `shutdown.exe` 两条路都**记返回码**（含特权启用/`GetLastError`）——"说重启却没动"从此有账可查。✅ 2026-10-04（`0.6.27`/`0.6.28`）

- PIT-115 **证据覆盖审计：补上"暂存成功但救援从未执行"的静默空洞（待执行标记 + 启动比对）**（2026-10-05，`0.6.29`）：审计整个失败面后发现**唯一可操作的洞**——"引导失败/开机断电导致救援层从未跑过"时，磁盘上没有任何 `ZJRESTORE-status.txt`，启动检查自然什么都不报（用户视角："点了开始，重启后什么都没发生"）。修复：① **暂存+重启**路径写 `<日志根>\logs\pending-restore.txt`（time/target/image；**菜单安装不写**，避免常驻菜单误报）；② 启动检查改为"最新回执时间（**含 OK**）vs 标记时间"：标记更新（或压根没回执）→ 提示"上次系统还原任务未执行（可能未进入恢复环境）"+ 删除标记（只报一次）；有更新回执 → 走原有 FAILED/成功逻辑并删标记。合成用例实测：T1（标记最新 → "未执行"提示 + 标记删除）、T2（标记旧 + FAILED 新 → 正确走失败回执）。**单测（`0.6.33`）**：判定逻辑抽到 `src/common/rescue_decision.h`（纯函数 `DecideRescueReport`），`rescue_report_decision` 9 场景断言——**单测当场抓出一处误判**：只填 FAILED 时间、未填 `latestStatusTime` 时会把 FAILED 当"无回执"→ 已改为取两者最大值（对不完整输入稳健）；顺带把该头文件加进 `TEST_BIN` 依赖（此前改 `.h` 不触发重编，测试跑的是旧二进制）。`make check` **33 用例/223 断言** + i18n **396** 键全绿。✅ 2026-10-05（`0.6.33`）

- PIT-116 **"屏幕也是证据面"：无文件可写 / 内核级挂死的表达 + 失败日志尾部重打 + 超时标记（用户 2026-10-05 规格，`0.6.30`/`0.6.31`）**：
  · `mnt_dev`（init + 脚本）每次尝试前打印 `mnt <dev>` —— 挂死时最后一屏就是"卡在哪个设备"（无文件可写时的唯一线索）。
  · `timeout 60` 命中（rc=124）时把 `TIMEOUT(60s:ntfs3/ntfs-3g)` 写进 `/tmp/zjmnt.err` → 进 probe 行：区分"挂死超时"与"普通失败"。
  · 失败时 init 把 `ZJRESTORE-last.log` 的**最后 20 行重打到屏幕**（先快照再打头，避免头部被 tail 自读）；**全盘不可写**时打 `===== PHOTOGRAPH THIS SCREEN =====` 横幅 + 日志尾 25 行 + 内核消息 20 行。
  · 脚本 `bb_final` 失败分支补 `say_dmesg`：内核 I/O 错误 / ata / nvme reset 进黑匣子日志（"任何失败都要能从记录中评估"）。
  · 局限（诚实记录）：真正的内核冻结/断电无法"事后打印"（只能靠最后一条 `mnt` 标记定位）；`timeout` 对 **D 状态**（不可中断 I/O）挂载杀不动——同样靠 `mnt` 标记定位，不静默。✅ 2026-10-05（`0.6.30`/`0.6.31`；QEMU 失败演练实测 `---- log tail (last 20 lines) ----` 块与 `RESULT: restore FAILED` 正常、黑匣子回执不变）

- PIT-117 **支持包带屏幕截图 + diag 补整机/BIOS/环境变量 + 救援层控制台文本转储；日志上限提到 64MB/16MB（用户 2026-10-05 规格，`0.6.32`）**：
  · Windows：`support`/「日志」出包时 GDI `BitBlt` 抓桌面 → `logs\screens\screen-<时间>.bmp`（24bpp BMP，1080p 约 6MB；零依赖、PE 可用；随 logs 递归进包）；`diag` 新增 `bios`（整机/主板厂商型号、BIOS 版本/日期，读注册表）、`env`（PROCESSOR_ARCHITECTURE/SystemRoot/windir/TEMP）与 `locale` 行。
  · 救援层：开机打 `dmi:`（sys_vendor/product_name/board_name/bios 版本日期）与 `cmdline:`；失败时 `dump_console` 读 `/dev/vcs*`（VT 字符矩阵 → tr 去 NUL + fold 折行）把**屏幕文本**写进黑匣子日志（QEMU 实测抓到 52 行真实内核消息；生产 cmdline 带 `tty0` 必有内容）。
  · 日志上限（第三次修订）：`>64MB` 触发 → 先删一周前 → 仍超则删到 **16MB** 以内（加截图/更多文本后仍余量充足）。
  · ⚠️ 构建坑：CLI 链接需补 `-lgdi32`（截屏用到 GDI；GUI 早就链接了）。
  · 回归：支持包实测含 `logs/screens/screen-*.bmp`（6.0MB）+ diag 的 `bios`/`env` 行；三盘失败演练含 `console text (/dev/vcs)` 且内容非空。✅ 2026-10-05（`0.6.32`）

- PIT-118 **客户 0.6.33 实测抓出：`mnt` 屏幕标记污染命令替换 → ESP 子镜像恢复/成功回执失效（`0.6.34` 修复）；同批收获大量真机验证**（2026-10-05）：
  · **根因**：PIT-116 的 `mnt <dev>` 标记用 `echo` 走 **stdout**，而 `ESP_DEV=$(find_esp_dev)` 会把函数内所有 stdout 一起捕获 → `ESP_DEV` 变成 `"ZJ:   mnt /dev/sda1\n/dev/sda1"` → 后续 `mnt_dev` 必然失败。客户机（GPT ESP）的 Alpine blkid **没返回 `PART_ENTRY_TYPE`** → `find_esp_dev` 走 **FAT 回退**（此路径会调用 `mnt_dev`）→ 触发；QEMU 演练盘 blkid 能返回 GUID → 走 GUID 路径（不调用 mnt_dev）→ 此前所有 drill 全绿也没暴露 ✗。
  · **影响**：① ESP 子镜像恢复失败（`ERROR: esp mount failed`，非致命；因 ESP 未被改动 + 暂存时 bcdboot 已刷新 C: 引导文件，系统照常启动）；② 成功路径的状态回执（原先只写 ESP）一并失败 → 下次启动会把 pending 标记**误报成"任务未执行"**。
  · **修复**：① 两个脚本的 `say()` 一律走 **stderr**（`1>&2`；屏幕/串口照旧，禁止再污染 `$(...)`）；② 成功路径把 `ZJRESTORE-status.txt` **也写进软件目录 logs**（BIOS 无 ESP 也保证有 OK 回执）；③ Windows `CheckLastRescueFailure` 增加扫描日志根直下；④ `run-drill.ps1` 新增 7 项断言（含 `esp restored` + `blackbox ESP`）——此前只打印不判定，是这次静默漏检的原因。
  · **真机实证**（客户 LENOVO 90MU000CCD，Netac 256G + HKVSN 2T GPT，**7-Zip SFX 单文件包在 `%TEMP%\7ZipSfx.001` 运行**）：VSS+ESP 备份 ✓、暂存 ✓、**自动重启（`reboot: ExitWindowsEx ok`）✓**、救援全链 `apply rc=0`（12.7GB/123s）✓、`RESTORE DONE` ✓；A2 清场（0 stray）、DMI（`dmi: LENOVO | 90MU000CCD`）/PCI 清单、pending 标记、sysfs offset 校验（该盘 blkid 偏移也为空，靠 sysfs 救回）均在真机落地。✅ 2026-10-05（`0.6.34`）

- PIT-119 **客户 0.6.33 复测反馈两件事：①还原后 explorer/右键卡死（新备份里原样复现）②PE 还原把易数启动项干掉了 —— 修 bcdboot 重建 BCD + 新增壳扩展审计（`0.6.35`）**（2026-10-05）：
  · **背景**：客户重新备份（0.6.33）→ 还原 → 症状不变（登录后 explorer 卡、任务管理器重启 explorer 才能用；**桌面右键必转圈**、左键正常）。"重新备份再还原仍复现" → **故障在源系统里**（我们的还原是如实的）；右键菜单卡死是**第三方 shell 扩展死锁**的教科书症状；救援日志可见 C: 根有 `Coodesker`（酷呆桌面）——头号嫌疑（易数/火绒外壳项次之）。
  · **修复 1（BCD 第三方条目保护）**：`bcdboot` 会**重建 BCD**（只剩 Windows 条目），易数等第三方启动项被抹掉。三条路径（PE 就地 UEFI/BIOS、暂存 UEFI）全部改为：**先判定 BCD 已有效就跳过 bcdboot**（`VerifyEspBcd` / 新增 `BiosBcdLooksValid`；同机还原原 BCD 本就指向同一 C:），只有缺失/损坏才重建，且重建前 `BcdExport` 备份为 `bcd-backup-before-bcdboot`。
  · **修复 2（壳扩展审计）**：`diag` 新增 `shellext` 行 —— 枚举右键处理器（`*`/`AllFilesystemObjects`/`Directory`/`Directory\Background`/`Drive`/`Folder`/`ThisPC`）/ 图标叠加 / `ShellExecuteHooks` / 被屏蔽项，CLSID→`InprocServer32` DLL 并查文件存在。**只有"解析到真实路径但文件没了"才标 `*** MISSING ***`**（Windows 内建无 InprocServer32 的形态不当故障）；两种注册形态（子键名=CLSID 或 默认值=CLSID）都识别——本机实测出 7-Zip/火绒外壳项与内建项的正确 DLL。
  · 待复测：0.6.35 客户机暂存日志应出现 `skip bcdboot (preserve third-party...)`、易数条目保留；壳扩展排查需客户在卡死系统上跑 `support` 发回（先停用 **Coodesker** 试验，安全模式对照）。✅ 2026-10-05（`0.6.35`；bcdboot 跳过逻辑待真机复测）

- PIT-120 **客户"别的软件还原没事、我们还原后右键卡死"的实锤差异：Linux 侧 wimlib 丢 NTFS 扩展属性（EA），Windows 侧不丢（`0.6.36`）**（2026-10-05）：
  · **证据 1（客户）**：0.6.33 的 `zjrestore-apply.out` 里唯一一条警告：`[WARNING] Ignoring extended attributes of 804 files`。
  · **证据 2（二进制）**：`Ignoring extended attributes` 字符串只存在于 **Linux 侧 wimlib**（initramfs 的 wimlib-imagex），**Windows 的 libwim-15.dll 里没有**（Windows 侧支持写 EA）。
  · **证据 3（实验室，可复现）**：新增 `tools/ea-scan.cpp`（`--set` 用 `NtSetEaFile` 造 EA；无参扫描列出带 EA 文件）。造 `ea1.bin`（EA `ZJTEST=1`）→ 我们工具 `backup`（Windows）→ `extract`（Windows wimlib）→ `fsutil file queryea` **EA 仍在** ✅。结论：**PE 就地还原（Windows 侧 wimlib）保 EA；暂存重启（Linux 侧 wimlib）丢 EA** —— "别人没事、我们有事"的保真度差异坐实。
  · **取证命令**：新增 `SysRecover.exe scan-ea [--root <目录>] [--out <文件>]`（`selfdiag.cpp::ScanEaFiles`，`NtQueryEaFile` 列出路径 + EA 名/值前 32 字节）→ 客户在 PE 还原后的系统上跑一次，即可拿到那 800 个文件的名单。
  · **BCD 保护（同批）**：`bcdboot` 重建 BCD 会删第三方条目（客户 BCD 备份实证：`Boot Windows created by 石头`、`Windows_PE`、UEFI 设备条目等）。三条路径改为 **BCD 有效即跳过 bcdboot**（`VerifyEspBcd`/`BiosBcdLooksValid`）；重建前 `BcdExport` 备份。客户手上 `logs\bcd-backup` 可用 `bcdedit /import` 找回条目。
  · 待定：EA 丢失与"右键卡死"的**因果**需客户交叉验证（0.6.36 的 PE 就地还原 → 右键应恢复；或 Dism++ 应用我们的镜像对照）。若坐实，Linux 侧 EA 恢复方案（捕获 sidecar + 首启补写）另行设计。✅ 2026-10-05（`0.6.36`；实验室差异已复现）

- PIT-121 **三项小修：①「删除菜单」后按钮不回「安装菜单」②备份完成自动做 EA 审计（清单落日志）③多款还原软件对照信息（`0.6.37`）**（2026-10-05）：
  · **① 根因**：`RefreshBootMenuBtn` 的"已安装" = `BootMenuInstalled()`（引导条目/文件）**或** `ReadMenuBinding()`（读 `<exeDir>\restore-task.conf`）；「删除菜单」只删了引导条目/文件，**没删副契约** → 永远判"已安装"，按钮停在「删除菜单」。修复：新增 `ops::DeleteMenuBinding()`（删 conf/json + 清 pending 标记），「删除菜单」与「清除引导项」都调用并刷新按钮。
  · **② 自动取证**：`RunBackup` 收尾自动调用 `ScanEaFiles(源)`（阶段名 `ea-scan`，状态栏中文"文件属性检查"），清单写 `<logs>\ea-scan-<时间>.txt`；发现 EA 时 `WARN`（附"Linux 救援还原会丢 / PE 不丢"）。实测全盘 C: **30.2 万文件仅 15 秒**，用户零操作；开发机 0 个 EA（客户机的 804 个来自其特定软件，名单下次备份自动拿到）。
  · **③ 客户对照**：**多款还原软件（易数等）均正常，只有我们的（Linux 救援路径）有问题** → 与 PIT-120 的 EA 差异互相印证；下一步让客户用 **0.6.37 在 PE 就地还原 `20261005Win10.19044备份.wim`**（Windows 侧 apply 保 EA + 不再重建 BCD），右键应恢复，随后 `scan-ea` 出名单即定案。✅ 2026-10-05（`0.6.37`）

- PIT-122 **EA（NTFS 扩展属性）全自动修复链路落地：备份采集 → ZJEA 子镜像 → 救援投放 → 首启补写器（`0.6.38`，2026-10-06）**：Linux 侧 wimlib apply 丢 Windows EA 是 PIT-120 实锤的客户"还原后桌面/右键卡死"根因差异；"Linux 侧直接写回 EA"经两组 QEMU 实验**否决**（`ntfs-3g user_xattr` 把 `user.*` 写成 **ADS 流**而非 EA；对真 EA 读不到、`system.ntfs_ea` 写被拒）→ 改为**首启补写**（Windows API 写 EA 是唯一可行通道，PIT-123 提供钩子）。链路与落点：
  · **备份端**（`src/common/ea.{h,cpp}` + `ops.cpp::RunBackup`）：`CaptureVolume(源)` 一次遍历产出审计清单（`logs\ea-scan-<时间>.txt`，沿用 PIT-121）**+ eapack.dat**（二进制格式 `ZJEA1`：每文件 u32 路径字节数+UTF-16LE 相对路径+EA 列表[名/值，值二进制安全]，值上限 65535、包上限 128MB 超限只审计）；发现 EA → 把 `eapack.dat + zj-ea-apply.exe（自 bootfiles/ 拷入）+ zj-regpol.bin（策略片段）` 打成**子镜像 `ZJEA`**（复用 ESP 的 Append 机制；`RunBackup` 的 `stage` 改为**非 append 一律暂存**，任何子镜像并入失败不损主镜像）。**非致命**：采集/并入失败只 WARN，主镜像照常交付。
  · **还原端**：`StageRestoreImpl` 从镜像内容发现名含 `ZJEA` 的子镜像 → 契约 **`ea_index`**（增量键，不升 contract_version）；名含 ZJEA 的子镜像**拒绝单独还原**（同 ESP 防呆，rc=4）。**就地还原（PE/非系统盘）无需此链**——Windows 侧 apply 原生保 EA。
  · **救援层**（`zjrestore-lite.sh`）：apply 完主系统后按 `ea_index` 解出子镜像 → 目标盘投放 `\ZJRESTORE\ea\{eapack.dat, zj-ea-apply.exe}` + 写 GPO 钩子（cmd + scripts.ini 合并 + gpt.ini 合并 + Registry.pol 追加，见 PIT-123）→ **落盘复核**（6 个文件逐个查，缺一即 WARN "deploy INCOMPLETE"，不静默）。
  · **首启补写器**（`src/tools/ea_apply_main.cpp` → `bootfiles/zj-ea-apply.exe`，**x86 构建**（x86/x64 目标都要能跑），无 wimlib 依赖，~900KB）：登录前 SYSTEM 执行 → `NtSetEaFile` 逐文件写回（`FILE_FULL_EA_INFORMATION` **链式**、逐条 4 字节对齐、末条 NextOffset=0、**无外层头** —— 对齐 go-winio/restic 实现，实验室 `--set` 单条目版已 fsutil 复核）→ 写 `ea-apply.log`（含 `shell_started=` 预登录判据）+ `ea-result.txt`（一行计数）→ **自清理**：scripts.ini 删条目、gpt.ini 版本 +1、Registry.pol 剥记录（解析失败原样不动）、删 pack、cmd/自身 `MOVEFILE_DELAY_UNTIL_REBOOT` 延迟删除（下轮启动生效；文件被占用时删不掉是 cmd 常态）→ 失败保留重试（attempts 计数，3 次后改名 `.failed` 停重试）。
  · **测试全绿**（全部由 AI 独自完成，无用户参与）：单测 6 个新用例（包往返/拒绝截断/pol 剥记录/EA 缓冲布局/ini 清理/gpt 递增）→ `make check` **39 用例/259 断言**；本机 e2e（造 EA → backup → extract → ea-apply → `fsutil queryea` 确认写回）；**QEMU 演练 9/9 PASS**（`ea subimage: index=3` → `ea: deployed (payload + GPO hook` → 盘上字节级复核：ini CRLF/双 CSE/版本/pol 382B 头全对）；**VM 首启 e2e（真实 Win10，round6~9）**：预登录执行（`shell_started=no`）、**含 `System32\` 路径**的 EA 写回成功、清理全项完成、延迟删除生效、**次轮不重跑**。✅ 2026-10-06（`0.6.38`）
  · **全链 e2e（0.6.39，2026-10-06）**：对**真实 Win10 系统镜像**（2.22GB WIM、**1677 个 EA 文件**）走完整链一次贯通：冷备采集（EA 包 563KB）→ QEMU 救援层真实还原（5.5GB apply + EA 投放 + 对**已有**钩子文件做合并：scripts.ini idx=0 / gpt.ini 双 CSE 已存在仅升版本 / pol 追加）→ 首启（预登录 `shell_started=no`）**ok=1676 / missing=1（被备份排除的文件，正常）/ failed=0** + 清理全项 + 日志可读。**唯一未在运行时跑过的**：Windows 暂存端从镜像发现 `ea_index`（与 ESP 共用同一 ListImages 循环，drill 用手写契约覆盖了后半链）。
  · ⚠️ **遗留**：客户**已有旧镜像**（EA 完好但无 ZJEA 子镜像）→ 需 Windows 侧还原（PE 就地）或后续 `ea-repack`（从旧镜像在 Windows 上物化 EA 再打包，**尚未实现**）；客户真机确认因果（首启后右键/桌面是否恢复）。

- PIT-123 **本地组策略「启动脚本」当首启钩子的机理定稿 + 三个实测坑（WOW64 重定向 / gpt.ini 版本撞车 / ccs 日志编码）（2026-10-06，`0.6.38`）**：EA 补写器必须在**登录前**（explorer 加载前）以 SYSTEM 跑完，否则卡死的 explorer 已经挂了。方案 = **纯文件**写本地 GPO（免注册表编辑），QEMU 对真实 Win10 做了 9 轮实验定稿：
  · **布局**：`Windows\System32\GroupPolicy\Machine\Scripts\Startup\<cmd>` + `Machine\Scripts\scripts.ini`（`[Startup]` 段 `NcmdLine=<cmd>` + `NParameters=`，索引=全文件数字键 max+1，插入点在 Startup 段内/新段）；`gpt.ini` 的 `gPCMachineExtensionNames` **必须声明两个 CSE 对**：Scripts `[{42B5FAAE-…}{40B6664F-…}]` + Registry `[{35378EAC-…}{D02B1F72-…}]`（缺 Scripts 对 → **脚本 CSE 根本不被调用**，策略处理了也不跑脚本，Extension-List 里没有它）；`Machine\Registry.pol`（记录格式 = `[key;\0 name;\0 type(4LE);\0 size(2LE)+00 00;\0 data]\0`，头 `PReg`+u32 1）追加两条 **REG_DWORD=1**：`Software\Microsoft\Windows\CurrentVersion\Policies\System!RunStartupScriptSync` + `Software\Policies\Microsoft\Windows NT\CurrentVersion\Winlogon!SyncForegroundPolicy`（ADMX 权威值；没有它们 Win10 默认**异步**执行启动脚本 → 实测登录后 ~1 分钟才跑，赶不上）。追加 pol 片段要**剥掉片段头 8 字节**（`tail -c +9`）再 cat，保留用户已有记录。
  · **坑① gpt.ini Version 撞车**：gpsvc 拿 `gpt.ini Version` 与注册表 `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Group Policy\State\Machine\GPO-List\0\Version` 比对，**相同 = "无变化"直接跳过**（实测初装写 65537 恰好撞上镜像里的残留记录 → 脚本从不执行，事件日志 `no changes detected`、Extension-List 只有 `{00000000}`）。规则：**有文件 → 机器字 +1；没有 → 写 `2147418113`（0x7FFF0001，压过常见残留）**。另实测：**脚本每引导都会跑**（不需要反复改版本；失败重试天然可行）。
  · **坑② WOW64 文件系统重定向（PIT-082 家族，2026-10-06 实测）**：补写器是 **32 位** exe（x86/x64 目标通吃），在 64 位 Windows 上访问 `C:\Windows\System32\GroupPolicy\…` 被**重定向到 `SysWOW64`** → 组策略文件全读不到（清理静默跳过）、延迟删除删错对象；EA 目标若在 System32 下也会写错文件。修复：wmain 开头 `Wow64DisableWow64FsRedirection`（GetProcAddress 两步 cast）进程级关闭。
  · **坑③ CRT `ccs=UTF-8` 日志吞文本**：`_wfopen(…, L"a, ccs=UTF-8")` + `fputs` 写出的日志只剩 BOM + 每行 `\r\n` 的 UTF-8 形态（U+0A0D 乱码），全部文本丢失 —— 改用 `CreateFileW(FILE_APPEND_DATA)` + `WriteFile` 裸字节追加。
  · **清理语义**：SYSTEM 身份下可覆写/删除 gpt.ini/scripts.ini/Registry.pol（实测 ok）；正在执行的 cmd 与自身 exe 用 `MOVEFILE_DELAY_UNTIL_REBOOT`（下轮启动删；**注意**：延迟删除会在下轮启动删掉**同路径的新文件** —— 实验室重新部署后曾被上一轮的延迟删除误杀，真机不会遇到，但排查时别被绕）。
  · **验证手法（可复用）**：真实 Win10 的 VMDK → `qemu-img convert` → VHD（清 sparse 标志才能挂载）→ 开发机挂载离线注入 → QEMU/OVMF 启动（`-boot order=c`，ESP 放 `\EFI\BOOT\BOOTX64.EFI` 兜底）→ monitor `screendump`/`system_powerdown` → 重新挂载查 marker/日志。全自动、无 guest 凭据需求。
  · ✅ 2026-10-06（`0.6.38`；EA 链整体见 PIT-122）

- PIT-124 **VSS 对挂载的离线 VHD 卷不可用（rc=89）→ 新增 `--no-snapshot` 冷备开关；junction 源被 wimlib 拒（rc=40）**（2026-10-06，`0.6.39`）：全链 e2e（PIT-122）需要备份一台**离线**的测试 VM 系统卷（挂载中的 VHD）：`backup --source F:/` → VSS precheck 正确拉起 vss/swprv，但快照创建失败 `rc=89 Unable to create a filesystem snapshot`（VSS 不支持这种卷）。两个绕道失败：① 建 junction（`D:\src` → `F:\`，非卷根 → 不触发 VSS）→ wimlib `rc=40 Expected a directory`（**拒 reparse point 作源**，带尾斜杠也一样）；② 无。正解：**新增 `backup --no-snapshot`**（`BackupRequest::noSnapshot`，`snapshot=(req.snapshot||volumeRoot)&&!noSnapshot`）—— PE / 离线卷 / VHD 卷上 VSS 不可用本来就是真实场景（冷备语义：卷静态、无需快照）。全链 e2e 用它完成。✅ 2026-10-06（`0.6.39`）

- PIT-125 **发布体积两轮优化（55.8→40.6MB）：①exe 剥符号 ②initramfs 模块保持 `.ko.xz`（不再转 `.ko.gz`）**（2026-10-06，`0.6.40`/`0.6.41`）：
  · **① 剥符号（`-s`，`0.6.40`）**：全部 exe 链接加 `-s` —— x64 CLI -838KB / GUI -1.1MB / EA 补写器 -495KB（四套共 ~4MB；崩溃报告只用"模块+偏移"（PIT-084），不依赖符号表）。另 `package` 现在自动清 `dist\logs`（从 dist 直接运行 exe 产生的运行期日志/诊断包，属垃圾，不再随交付分发，-6.5MB）。
  · **② initramfs 模块格式（`0.6.41`）**：原实现是 `gzip(cpio(模块 .ko.gz))` —— **双重压缩**（模块早已 gz，外层 gzip 几乎无效）。内核配置实测 `CONFIG_MODULE_DECOMPRESS=y` + `CONFIG_MODULE_COMPRESS_XZ=y`（Debian 原生 .ko.xz，**内核原生解压**）→ 构建脚本改为**原样拷贝 `.ko.xz`**（不再 lzma 解压+重压），`modules.dep` 路径随之 `.xz`；`bootfiles/alpine/init` 的 3 处后缀处理（ldmod 的 builtin 判定、两个"全量 sweep" find 循环）加 `*.ko.xz`。**initramfs 19.5→15.2MB（-4.3MB），494 模块一个不少**；rootfs 里模块反而更小（内存占用下降），模块加载走内核原生解压（速度≈不变）。回归：QEMU drill 9/9 PASS（模块加载/ntfs3 挂载/wimlib apply/EA 投放全链）+ UEFI SB 链冒烟 PASS。
  · **未采用（用户 2026-10-06 裁定"影响兼容性/速度的一律不做"）**：a) "模块解压存储 + xz 外层"（可再省 ~7.8MB，但模块整棵进 RAM → **+50MB 内存**，侵蚀低内存兼容余量（PIT-113：512MB 才稳、192MB OOM），且外层解压更慢）；b) 裁小众模块（VDO/Ceph/DRBD/bcache 等，属兼容覆盖面，保留全部 494）。**可再议**：外层 gzip→xz（再省 ~0.5-1MB，需改产物名/引用，收益小）。
  · **当前体积账**：exe ~5.4MB（已剥）+ initramfs 15.2 + vmlinuz 11.6（SB 签名，不可动）+ sb 资产 3.6 + UCRT ~3（Win7 兼容）+ wimlib/grldr/补写器/skin/lang ~2 → **≈40.6MB**。

- PIT-126 **论坛反馈核实（Win11 精简版装菜单失败）+ 两个真 bug 修复：①ESP 引导校验误伤精简系统 ②引导层失败后契约不回滚（按钮假「删除菜单」）**（2026-10-06，`0.6.42`）：
  · **反馈原文**：英文精简 Win11 + v0.6.3 装菜单报 `ESP boot verification failed (boot was NOT properly repaired; aborted before touching the target partition): …Fonts\*_boot.ttf`，且失败后按钮变成「删除菜单」。**逐字核实属实**（报错 = `Tr("ESP 引导校验未通过（未真正修好，已中止，未动目标分区）：")` 的英译 + 字体详情；且 0.6.3→0.6.41 代码顺序未变 → **当前版本同样会复现**）。
  · **根因①（误伤精简系统）**：`VerifyEspBcd` 把 bootres/引导字体/语言资源当**硬项**；精简镜像把 `C:\Windows\Boot\{Fonts,Resources}` 裁掉了 → bcdboot 无源可拷 → ESP 缺字体 → fail-closed 中止。而机器本来就能正常开机（字体只影响 bootmgr 菜单渲染；我们的 UEFI 菜单是**固件启动项**、不经 bootmgr）→ 纯误伤。**修复**：软项**源感知** —— `VerifyEspBcd(espRoot, srcWinDir, detail)`：ESP 缺 + **源也缺** = 精简镜像既成事实 → 放行 + detail 记 `[lite: source lacks …]`；ESP 缺 + **源有** = bcdboot 写一半 → 仍 fail-closed（保住 PIT-090 的保险丝语义）。判定抽成纯逻辑 `EspSoftItemVerdict`（uefi.h inline）+ 单测。副作用：此类机器的**跳过 bcdboot 判定**也随之通过 → 不再无谓重建 BCD（连带保住第三方启动项）。
  · **根因②（契约不回滚）**：`StageRestoreImpl` 里契约（目标根 `_zjresy` + exeDir `restore-task.conf`）在引导层**之前**写（菜单绑定/暂存任务都需要它）；引导层失败 return 时契约已落盘 → GUI `ReadMenuBinding` 误判"已安装"（按钮变「删除菜单」，用户实测）；且目标根留一份 `action=restore` 陈旧日志（将来进救援层可能被误执行）。**修复**：加 **RAII 回滚守卫**（`repairBoot` 时武装；引导层+单次启动段内**任何失败 return 都自动** `CleanupStrayContracts` + `DeleteMenuBinding` + 清数据盘副本；成功路径 `return 0` 前 disarm）。**注意**：只回滚契约，**不删**已部署的引导文件/启动项 —— 用户可能是在旧菜单基础上重装，失败时不能把旧菜单一起干掉。
  · **验证**：单测 **40 用例/263 断言**（新增 `esp_soft_item_verdict` 四态）+ `make check` 全绿 + CLI 编译零警告。🚧 待真机/精简系统复测（本机无精简 Win11；论坛用户升级后验证）。
  · **给受影响用户的处置**：① 失败是 **fail-closed 中止、未动目标分区**（系统没坏）；② 0.6.3 的"卡在删除菜单"是 PIT-121 的旧 bug（0.6.37 已修），升级即可；③ 手工清残留：删程序目录 `restore-task.conf|json` + 目标盘根 `_zjresy*.log`；④ 升级到 `0.6.42+` 后重装菜单应直接成功。

- PIT-127 **跨固件还原实测（GPT/UEFI ↔ MBR/BIOS 双向均通过）；顺带证实两条设计关键点（ESP 子镜像不含 BCD / bootfix 恒为 BIOS 味道）**（2026-10-06，`0.6.42` 代码验证、无代码改动）：
  · **背景**：用户提问核实"备份从 GPT 恢复到 BIOS、或反之，会不会出问题（含镜像有无 ESP 两种情况）"→ 实验室两轮**真启动**验证（QEMU 直启内核跑救援 + 真 Windows 首启）：
    ① **GPT/UEFI 源（镜像含 ESP 子镜像）→ MBR/BIOS 目标**：救援层 `apply rc=0` → bootfix（BIOS 味道）→ PBR 引导区 → **`WARN: task has esp subimage (idx 3) but no ESP partition found (skip)`**（带 ESP 的镜像到无 ESP 机器 = 正确跳过、不中断）→ **SeaBIOS 启动到桌面 ✓**（截图 `D:\EA-lab\e2e2\shot-bios1/2.png`）。
    ② **MBR/BIOS 源（无 ESP）→ GPT/UEFI 目标**：`apply rc=0` → `UEFI/GPT: ESP untouched, skip PBR` → EA 投放 → 用 `bcdboot /f UEFI /nofirmwaresync` 补 ESP（等价真机暂存侧 bcdboot；`/nofirmwaresync` 避免动本机 NVRAM）→ **OVMF 启动到桌面 ✓**（`shot-uefi3/4.png`）。
  · **两条设计关键点（本次实测顺带证实）**：a) **ESP 子镜像不含 BCD**（捕获时排除 `\EFI\Microsoft\Boot\BCD`；当初的原因是被运行中的 Windows 锁住，见 ESP 备份条目）→ 还原到 UEFI 目标时**不会覆盖目标机自己的 BCD**（目标 BCD 指向同一分区 → 恢复后照样启动，这是"UEFI 目标机跳过 bcdboot"安全的前提）；b) `PrepareBootFixFiles` **恒为 BIOS 味道**（无 `C:\Boot\BCD` 时先 `bcdboot /f BIOS` 生成，再改写 `{default}` 为 `device boot` + `path \Windows\system32\winload.exe`）→ 目标无论固件类型都可用（UEFI 下仅作摆设，UEFI 引导走 ESP）。
  · **边界/注意**：a) UEFI 目标若 **ESP 全空**（新盘）→ 依赖**暂存侧 bcdboot** 先写好（0.6.35+ "BCD 有效即跳过"，无效时会重建）；b) UEFI 目标 ESP 的 BCD 若指向**别的分区**（多系统 / 还原到非原系统分区）→ 跳过逻辑不比较目标分区，可能启动旧系统（边缘场景，暂记录）；c) 实验室 OVMF 需**空 NVRAM** 或 `\EFI\BOOT\BOOTX64.EFI` 兜底（旧 vars 会优先走失效条目 → PXE）；真机 NVRAM 里有目标机自己的条目，不受影响。
  · 实验素材：`D:\EA-lab\e2e2\`（实验室目录，不入库）。✅ 2026-10-06（`0.6.42`）

- PIT-128 **目标盘误判隐患：Linux 给两块 IDE 盘的 sd 字母会变 + 多盘首分区都在 1MiB → 只比 offset 会把镜像盘当目标盘；修复 = offset 匹配必须同时校验 size（双端）**（2026-10-06，`0.6.43`，实验室意外触发）：
  · **怎么发现的**：批量测试（docs/19 批次 A，Win10 精简镜像）时漏写目标盘 `_zjresy` 日志（实验室失误），救援层退到"按 offset 匹配"；恰好那次两块 IDE 盘（目标 40GB / 镜像 16GB，**首分区都在 1MiB**）的 **sd 字母与上次相反**（libata 异步探测顺序不保证，实测同一 QEMU 参数两次启动 sda/sdb 对调）→ 镜像盘被选成"目标"→ 镜像扫描又跳过"目标"→ 在另一块盘上找不到镜像 → 安全中止（**镜像扫描先于 mkntfs，没动任何盘** —— 顺序救了场）。
  · **修复（双端）**：① `zjrestore-lite.sh`：新增 `dev_size()`（sysfs size×512）；`target_by_offset` 与"日志候选"校验改为 **offset 必须匹配 +（双方都有 size 时）size 也必须匹配**，不符即 `target-reject … size-mismatch` 跳过；② `bootfiles/alpine/init` 的 `scan_for_log` 同样加 `target_part_size` 校验（防旧日志把别的同 offset 分区指成目标）。
  · **直接验证**：确定性布局（镜像盘挂 NVMe=枚举在前、目标挂 IDE、无目标日志）→ 日志出现 `target-reject dev=/dev/nvme0n1p1 offset-ok size-mismatch 17177772032/42948624384` → 正确选中 40GB 目标 → `found image`（在镜像盘）→ `apply rc=0` → RESTORE DONE ✓；drill 回归 PASS（正常流程不受影响）。
  · **教训**：任何"按位置猜设备"的逻辑都必须**多字段校验**（offset+size，能加 serial 更好）；`/dev/sdX` 字母不可靠（跨启动可变，勿持久化）。

- PIT-129 **"不支持的系统镜像"主动拒绝（I-1 落地）：镜像必须有 `\Windows\system32\winload.exe`，否则写契约/格式化前拒绝**（2026-10-06，`0.6.44`；docs/19 批次 G 实测定稿）：
  · **为什么**：老系统镜像（XP/2003/2000/98）与"安装源类"镜像（实测某 XP esd 连 ntoskrnl 都没有）还原后会"**格式化完才发现起不来**"（Win7+ 才有 winload.exe/BCD 体系）。批次 G 实测：XP x86/x64、2000、98、2003 全部无 winload；Win7 对照组有 ✓。
  · **实现（双端）**：① Windows 侧 `StageRestoreImpl` 步骤 0.7：新增 `WimEngine::ImagePathExists()`（`wimlib_iterate_dir_tree`，只读元数据）探 `\Windows\System32\winload.exe`，缺失即拒绝（rc=4，未动任何东西）；② 救援层 `zjrestore-lite.sh` 在找到镜像后、**动目标分区前**同款检查（`wimlib-imagex extract` 单文件探测，带 timeout；缺 → `STEP=unsupported-image` 退出）。
  · **四个实测坑（都踩了）**：a) **`wimlib-imagex dir` 在 solid ESD 上会卡死**（首次调用 rc=2、二次调用挂到超时）→ 弃用 `dir`，改单文件 `extract` 探测；b) **Linux 侧 wimlib 路径匹配区分大小写**（真实镜像里是 `System32` 大写 S）→ 必须**双大小写各探一次**（Windows 侧不区分）；c) **`set -u` 下 `$WIMLIB` 未定义**：变量原本在 apply 段才赋值、预检在前 → 脚本 rc=2 直接退出（已把解析提前）；d) 初版用 ntoskrnl 当"系统镜像"门槛是 **fail-open 漏洞**（安装源类镜像没有 ntoskrnl → 被当"非系统镜像"放行）→ 改为单一硬规则"必须 winload.exe"。
  · **演练盘适配**：`drill-src` 补 `Windows\system32\winload.exe`（dummy），test.wim 重建后 drill 照常 PASS（演练素材本来就不是真系统镜像）。
  · **验证**：XP esd 负例（无 mkntfs/无 apply + 明确报错）✓；Win7 x86 正例（precheck ok → apply rc=0 → RESTORE DONE）✓；drill 回归 PASS ✓。

- PIT-130 **`repair-boot` 机器侧回归完成 + 实验室"重启类"实验的休眠陷阱 + lite 镜像 GPO 行为修正**（2026-10-07；docs/15 §13 全过程）：
  · **回归结果**（在还原后的真实 Windows 里自动执行 `SysRecover.exe repair-boot`，0.6.44）：**UEFI 正常 ESP** ✅（`target=C: disk=0 part=4` → `bcdboot (UEFI) rc=0` → `displayorder {default}` → 校验通过 rc=0）；**UEFI + ESP 类型 GUID 被改成 Basic Data**（模拟 DiskGenius 重建分区）✅（走 `FindEspPartitionFallback`：`[WARN] ESP partition type is not EFI System; used fallback` → rc=0）；**BIOS/MBR** ✅（`bcdboot (BIOS) rc=0`）。docs/15 §12.3 两项待做全部关闭。
  · **⚠️ 实验室休眠陷阱（做"重启类"实验必读）**：5509 lite 镜像上 `system_powerdown` 触发的是**休眠**（写 6.4GB `hiberfil.sys`）→ 下一次"启动"实为**恢复会话**：**不执行任何启动脚本、无需登录**（本轮曾因此得出"GPO 脚本不执行"的**假阴性**）。**做重启类实验前必须**：删 `<系统盘>\hiberfil.sys` + 置 `HiberbootEnabled=0`（SYSTEM hive `ControlSet001\Control\Session Manager\Power`），并确认下次是真冷启动。
  · **GPO 钩子在 lite 上"能用但时机存疑"**：Scripts CSE 实测**能执行**（BIOS 目标 `gpupdate /force` 后 / UEFI 目标冷启动**开机期**，均 `nt authority\system`，脚本 rc=0）；但 `Registry.pol` 的 `RunStartupScriptSync`/`SyncForegroundPolicy` 在 lite 上**未落注册表**（标准镜像上生效、EA e2e 曾实测登录前 `shell_started=no`）→ 脚本可能是**异步执行（登录后 ~1 分钟）而非登录前同步**。**影响 EA 修复链**（首启补写要在 explorer 前跑完）——**2026-10-07 已定案（见 PIT-131）**：根因是 gpt.ini 版本低字碰撞导致 gpsvc 静默跳过处理；修复后实测**登录前执行**（`shell_started=no`），无需备用钩子。
  · 次要：ESP 被 `mountvol` 挂上盘符时 `LogBaseDir()` 会把它当数据盘，把 `logs\` 写进 ESP 的 `ZJRESTORE\logs\`（轻微污染，记录备查）；`cmd.exe /c x.cmd` 注册成服务（ImagePath）在本机**未触发**（SCM 日志被 lite 精简，原因未查）——**RunOnce 通道稳定可用**。
  · 测试环境：实验室 QEMU（OVMF/SeaBIOS）+ 5509 备份还原出的真实 Windows；钩子（本地 GPO 四件套 + RunOnce）已按 PIT-123 配方部署，测后已清理（`C:\zjtest\` 保留作证据）。

- PIT-131 **EA 首启补写器在"带组策略残留"的镜像上静默不执行——根因 gpt.ini 版本低字碰撞；修复 = 时间基版本**（2026-10-07，`0.6.45`；5509 lite 镜像全链实测定位，客户 EA 修复的关键加固）：
  · **现象**：5509 系统备份（Windows 侧 `backup --no-snapshot`，EA 采集 255 文件 → ZJEA 子镜像）经救援层还原后，**首启补写器不执行**（无 ea-apply.log、EA 未写回、清理未发生）；多次重启、`gpupdate /force` 均不触发；而同一镜像经 ESD 还原的另一台机器上 `gpupdate` 后能跑——一度极难定位（曾误判为"lite 镜像 GPO 不工作"）。
  · **根因（对比实验定死）**：镜像注册表里遗留 `...\Group Policy\State\Machine\GPO-List\0\Version = 0x00010001`（本地组策略"已处理"的常见残留），而部署的 gpt.ini 写 `0x7FFF0001` —— gpsvc 的变更检测**只比较低 16 位**（用户版本字）：两个低字都是 `0x0001` → 判"无变化"→ 跳过整个处理 → **Scripts CSE 永不执行**（PIT-123 的 65537 碰撞是同一机制的另一种撞法）。实验：State 清零（低字 0）→ 立即处理并执行 ✓；gpt.ini 改 `0x7FFF7FFF`（低字 0x7FFF）而 State 保持 0x10001 → 同样执行 ✓（且 State 被记录为新版本）。
  · **修复**：`zjrestore-lite.sh` 的 EA gpt.ini **新版本改用时间基低字**：`2147418112 + (date +%s) % 32768`（= `0x7FFF0000 | 低字`；合并分支的 `VER+1` 天然低字不同，保留）——保证与任何残留/上次部署的版本都不同，杜绝静默跳过。
  · **验证**：① 实验室全链（5509 lite 备份 → 救援还原 → 首启）：`shell_started=no (pre-logon)`、`ok=255 missing=0 failed=0`、清理全项完成、EA（`ZJTEST`）实测写回、State 正确记录新版本 ✓；② `make check` 40 用例/263 断言 + check-docs/i18n 全绿；③ QEMU 演练 PASS（`ea: gpt.ini created (version=2147427948)` → `RESTORE DONE`）；④ `make package` 双架构。
  · **教训**：凡"版本号当变更检测"的机制，要假设比较可能**只取部分位**——固定魔数（0x7FFF0001）会撞上常见残留（0x10001）；**时间基/随机基最稳**。另（复测再踩）：补写器的延迟自删（`MOVEFILE_DELAY_UNTIL_REBOOT`）会删掉**下次启动前重新部署的同路径新文件**（PIT-123 已记）——重放实验必须先把 exe/cmd 补回并注意其注册已消费。

- PIT-132 **"悬空引用"根治：注册表指向备份排除目录（Temp 等）的组件还原后必坏 —— 备份扫描 + 首启自动清理**（2026-10-07，`0.6.46`；客户右键卡死的最终修复）：
  · **背景**：客户"用我们的软件还原后右键打圈/卡死"在 EA 修复（PIT-131）后依旧（原话："一点右键就打圈；重启资源管理器后左键能用、右键不行"）。定因：**右键菜单处理器损坏** —— 豆包便携版把扩展 DLL 注册在 `HKCR\*\shellex\ContextMenuHandlers\{A5AF131F-…}` → `…\AppData\Local\Temp\DoubaoPortableTemp\…\shellext.dll`；**我们的备份排除 `\Users\*\AppData\Local\Temp*`（`exclude.cpp:35`）→ DLL 不进镜像，注册项却随注册表还原** → "有注册、无 DLL"的坏处理器 → 每次右键加载它 → 卡死。易数/DiskGenius 是**扇区/整分区级**还原（Temp 原样保留）→ 不卡 —— "只有我们卡"的差异吻合。等待链（UI 线程等 shell 工作线程）+ explorer 转储（shell32→combase 处理器加载路径阻塞）佐证。
  · **不止右键**：启动项（Run/RunOnce）指向 Temp、COM 注册、服务、计划任务等"注册表引用指向被排除路径"都会在还原后悬空（启动项→登录报错弹窗；COM→应用异常）。
  · **实现（通用扫描器 + 保守自愈，`src/common/refscan.{h,cpp}`）**：① 扫描 shell 扩展（7 个右键根 + 覆盖图标 + ShellExecuteHooks；**HKLM 双 WOW64 视图**——x86 补写器必须 `KEY_WOW64_64KEY` 才看得到 64 位视图，PIT-082 家族）+ 启动项（Run/RunOnce：机器双视图 + 用户 hive——备份扫已加载的 HKU，首启从磁盘加载 `Users\*\NTUSER.DAT`）；② 判定 = 路径片段落在易失目录（AppData\Local\Temp / Windows\Temp / CbsTemp / winsxs\InstallTemp / INetCache）；③ **备份时**（`ops.cpp`）：统计"易失目录引用"（`missingNow` / `at-risk` 两状态都算）→ >0 时并入 ZJEA 修复包（无 EA 也可只有补写器）；④ **首启**（补写器、登录前 SYSTEM）：`CleanAllDanglingRefs` **只删"易失目录 + 文件已缺失"**的项（存在的不动），删前落 `refs-backup.txt`（可恢复）、结果落 `refs-result.txt`；⑤ 救援脚本载荷校验放宽（**补写器必需、eapack 可选**）+ 启动 cmd 双模式（有包 `--pack`，无包直接跑）；⑥ `diag` 输出 `volatile-ref: N (missing now=…; at-risk=…)` 供支持包取证。
  · **验证**：① 单测 `refscan_volatile_path` / `refscan_extract_exe`（`make check` **42 用例/282 断言**）；② **机内全链 e2e**（注入假引用 → utarget 机内 VSS 备份：`ref scan: 2 volatile-dir reference(s) (0 missing, 2 at-risk)` → `EA/fix pack appended (ea=255 refs=2)` → 救援还原（`applier=yes`、时间基 gpt 版本）→ 首启：**EA 255/255 + `refs: found=2 cleaned=2 failed=0 atRisk=0`**；离线核验两条注册项消失、`refs-backup.txt` 在案、EA 抽查在）；③ drill PASS；④ `make package` 双架构。
  · 备注：用户 hive 加载尽力而为（实测 `admin` 的 NTUSER.DAT 加载 rc=32 共享冲突 → 优雅跳过并记录；主清理在机器级）；`[ExclusionException]` 路线被否（**wimlib 不扫描被排除目录的子树**，例外救不回目录内的文件，官方论坛/MS WIMGAPI 同款行为）。
  · **扩展（`0.6.47`，同 PIT-132）**：扫描范围再加三类 —— ① **COM 注册**（`HKLM\SOFTWARE\Classes\CLSID\*\InprocServer32|LocalServer32` 默认值，**双 WOW64 视图**；删除 = 删整棵服务器子键）；② **服务 ImagePath**（`HKLM\SYSTEM\CurrentControlSet\Services\*`，不分视图单遍；删除 = 删 ImagePath 值）；③ **计划任务**（`System32\Tasks` 下 XML 的 `<Command>`，含子目录深度 ≤3；删除 = `schtasks /Delete /TN <相对名> /F`，登录前实测可用）。**e2e（6 类引用一次全测）**：注入 shell 扩展/Run/COM×2/服务/任务 → 备份 `ref scan: 6 (0 missing, 6 at-risk)` → `EA/fix pack (ea=255 refs=6)` → 救援还原 → 首启 `refs: found=6 cleaned=6 failed=0` + 离线核验 5 处注册表全消失、任务文件已删 ✓。**扫描耗时**：真机 `diag` 实测全扫描 **~1s**（含 ~2 万 CLSID ×2 视图）；QEMU TCG 下约 23s（一次性、登录前）。单测加 `refscan_task_commands`（`make check` 43 用例/286 断言）。

---

## 14. License 合规（SBOM，随版本更新）

| 组件 | 版本/来源 | License | 链接/分发方式 |
|---|---|---|---|
| libwim | 记录版本 + wimlib.net | LGPLv3 | 动态链接，随包放 DLL，保留声明，允许用户替换 |
| Duilib 系 | 记录 fork+commit | BSD/MIT | 静态链接可闭源 |
| grldr/grldr.mbr | 记录来源 URL + SHA | GPL | 仅分发二进制，不修改不链接，独立聚合 |
| wimlib-imagex 源码 | — | GPLv3 | **禁止引入** |
| Alpine linux-lts 6.6.142 | 模块（440+ .ko.gz） | GPLv2 | ⚠️ **内核已换成 Debian 的（见下）**；这些模块不再随包 |
| Alpine busybox-static 1.36.1 | `/bin/busybox` | GPLv2 | 仅分发二进制 |
| Alpine musl 1.2.5 | `ld-musl-x86_64.so.1` | MIT | 仅分发二进制 |
| Alpine ntfs-3g/ntfsprogs 2026.2.25 | `ntfs-3g` + `mkntfs` | GPLv2 | 仅分发二进制 |
| Alpine wimlib 1.14.4 | `wimlib-imagex`（动态链接 libwim） | LGPLv3 | **动态链接**，随包放 `libwim.so.15`，允许用户替换 |
| Alpine util-linux 2.40.1 | `blkid` + `libblkid`/`libuuid`/`libeconf` | GPLv2 / LGPL | 仅分发二进制 |
| **Debian 内核** `linux-image-6.12.107+deb13-amd64` | `vmlinuz-zjrestore` | GPLv2 | 仅分发二进制（未修改，**Debian 签名**），独立聚合（PLAN §11.1） |
| **Debian 内核模块**（同一包内的存储/文件系统子集；2026-09-24 经 EXCLUDE 裁剪）| `initramfs` 里 **494** 个 `.ko.xz`（2026-10-06 起保持 Debian 原生格式，见 PIT-125） | GPLv2 | 同上（均带 Debian 签名） |
| **GRUB**（Debian `grub-efi-amd64-signed` 1+2.12+9+deb13u2）| `bootfiles/sb/grubx64.efi` | GPLv3 | 仅分发已签名二进制（未修改），独立聚合；Secure Boot 链（PLAN §11.1） |
| shim（Debian `shim-signed` 1.51+16.1-2）| `bootfiles/sb/shimx64.efi` | **BSD-2-Clause** | 仅分发已签名二进制（未修改，**微软 CA2011+CA2023 双签**）；Secure Boot 链入口 |
| ~~systemd-stub / UKI / efiloader~~ | 随 MOK 备选线**停止分发**（`ZJ_ENABLE_MOK_PATH=0`） | — | 仅仓库留存，`dist` 不含 |
| osslsigncode 2.9 | 构建期给 UKI 签名（备选线用） | GPLv3 | **仅构建工具，不进产品** |
| ~~BG-Rescue 9.0.0~~ | 已弃用（PIT-044） | — | — |
| Dism++ 主程序 | — | 闭源 | **禁止抄袭** |

> **豁免依据**：本产品自身代码**未静态链接任何 GPL 组件、未修改任何第三方源码**（`libwim-15.dll`
> 动态链接；其余为「单独分发」的聚合）→ 不受 copyleft 衍生作品条款约束。**本产品自有代码以 MIT 许可开源**（见仓库根 `LICENSE`）。
> 上表全部内容 + 各许可全文已生成 `THIRD_PARTY_LICENSES.txt`，由 `make package` 拷进 `dist/` ✓。
> 构建/测试期工具（MinGW-w64、osslsigncode、QEMU/OVMF、mtools）**不随产品分发**。

---

## 15. UI 外观设计（1:1 对齐老项目 WPF，2026-09-05 定稿；2026-09-13 逐像素校准完成）

老界面（`SysRestore/src/ZjRestore.Gui/MainWindow.xaml`，870×410，无边框圆角）**外观确认无问题**，C++ 版照抄布局，只换实现（WPF XAML → Duilib XML）。详细映射见 `docs/ui-design.md`，骨架见 `skin/main.xml`。

布局速览（上→下）：
1. **顶栏**（`#f7f9fc`）：左=模式 Tab（Radio：`镜像恢复为系统` / `系统备份为镜像`，选中下划线 `#96aaf0`）+ 中=标题`九转还原 v0.1` + 右=最小化/最大化/关闭（关闭 hover `#e81123`）。
2. **第一步**（白卡）：`浏览系统镜像文件`按钮 + 镜像路径圆角输入框；第二行`镜像说明：`+ 子镜像下拉（`Index - Name`）/ 备注输入。
3. **第二步**（`#f0f5ff` 蓝卡）：`系统安装位置` + 四列分区下拉（`盘符·系统类型 | 磁盘 | 分区 | 容量·可用`，行内含用量进度条）。
4. **第三步行**：大主按钮（`开始恢复系统`，`#8ca0de`，禁用态 `#e2e6ee`）+ 右侧`静默模式`勾选、`格式：`下拉（`.esd(慢速小体积)`/`.wim(正常大小)`/`.wim(高速大体积)`）、`清除引导项`、`生成启动菜单`。
5. **状态栏**（`#fafbfe`）：`执行进度` + 进度条（`#6a5ce0`）。

配色/字体 token（Duilib 与 WPF 对齐，勿自创）：
`bg #eef1f6 / card #ffffff / blue-card #f0f5ff / text #1b2432 / secondary #5f6c80 / border #dfe5ee / accent #2b5ce0 / primary #8ca0de / orange-step #e08a2e / progress #6a5ce0`；字号 14–16，标题 20 Bold。

> **顶栏改版（2026-09-16 用户规格）**：顶栏由浅色 `#f7f9fc` 改为**蓝色水平渐变**（左 `#8aa0e6` ≈ Tab 区较浅 → 右 `#5c74d0` ≈ 标题区更深，中点即用户指定的 RGB(111,133,222)）；选中 Tab 用**更深的蓝圆角底** `#3d53be` + 白字白图标，未选中 Tab 浅蓝白字 `#d7e0f7`；标题白字、副标题 `#dce4f8`；窗口按钮字形浅色 `#e8edfb`、hover 半透明白、关闭 hover 红底。渐变用新自绘控件 `TopBar`（经典 `CControlUI` 只支持纯色 `bkcolor`），左侧 Tab 文案改为 `文件→系统` / `系统→文件`。

控件名与老项目一一对应（`ModeRestore/ModeBackup/ImagePath/ImageIndexBox/NoteText/PartitionBox/MainAction/Silent/FormatBox/Progress/StatusText`），C++ 侧用同名 id，便于对照 `MainWindow.xaml.cs` 移植逻辑。

**实现方式（全自绘，零图片资源）**：经典库的 `Button/Option/CheckBox/Combo/Progress` **没有"无图回退"**（图片链为空时什么都不画），所以在 `CMainForm::CreateControl()` 里注入 `src/gui/ui_skin.cpp` 的 10 个自绘类：`SkinButton`（圆角按钮）/ `SkinEdit`（圆角输入框，`underline="true"` 时只画底边）/ `SkinLabel`（圆角徽标与文字）/ `TabOption`（模式页签，选中态底部圆角下划线）/ `GlyphCheck`（勾选框）/ `RoundProgress`（圆角进度条）/ `PartItem`（四列分区行，重写 `DoPaint`+`DrawItemText` 以便收起框也显示）/ `TextItem`（纯文字下拉条目）/ `TitleLabel`（标题+副标题）/ `WinBtn`（最小化/最大化/关闭）。**必须用这些新标签名**（内建名会被 `UIDlgBuilder` 拦截，见 PIT-016④）。控件映射表见 `docs/ui-design.md §2`。

**实测几何（对 `界面1.png` 逐像素，屏幕坐标；XML 里写的是面板坐标 = 屏幕坐标 −2，见 `docs/ui-design.md §6.6`）**：

| 元素 | 实测值 |
|---|---|
| 窗口 | **745×410**（2026-09-17 由 870 收窄 −125；靠右元素左移，靠左不动）；`CreateRoundRectRgn` 圆角 24（`borderround` 是直径） |
| 外框 1px 环 | 左 `#e1e7ef` / 右 `#d6dbe3`（露底色 `#eef1f6`） |
| 顶栏 | 高 52，`#f7f9fc` |
| 第一步卡片 | x18..851 y66..142，边框 `#f4f6f9` |
| 第二步卡片 | x18..851 y151..285，边框 `#f8f8fb` |
| 表头带 | x35..836 y194..224，`#e8effb` |
| 分区下拉 | y230..273，边框 `#c9d6f2` |
| 浏览按钮 | x102..241 y78..117，边框 `#eef1f6` |
| 主按钮 | x119..239 y295..333，底 `#e2e6ee`、字 `#9aa3b2` |
| 状态栏分隔线 | y372，`#eef1f6` |
| 分区行文字 | 三带：磁盘型号 mid−9 / 盘符列 mid / 卷标·可用 mid+9（老图 y241.5 / 251 / 261） |
| `镜像说明：` | 文字 y120..131（右对齐到 x232），其右侧老图为空；新设计改为「只留一根横线」，底线收在 y137（第一步卡片内） |

**两个模式的几何：第一/二步与第三步主按钮完全一致（切换时不跳），差异只在第三步右侧。** 别把两张老图搞混：`界面1.png` 是**备份模式**（"备份源分区"/"开始备份系统"），`old1.png` 才是还原模式。实测老图备份模式的内容整体比还原模式低 7px、第一步卡片高 7px —— 但那会让模式切换时画面明显错位，**按用户要求统一成一套几何**（若日后要 1:1 复刻老图，原始实测值是：卡1 y66..149 / 卡2 y158..292 / 表头带 y201..231 / 分区下拉 y237..280 / 主按钮 y302..339）。

| 第三步右侧（面板坐标，2026-09-17 收窄后实测；2026-10-03 加「日志」按钮） | 还原模式 | 备份模式 |
|---|---|---|
| 静默模式 | 勾选框起点 x**360**（文字到 460） | 勾选框 x**250**.. + 文字（面板） |
| 日志（支持包，还原专属） | x466..524（悬停提示"搜集还原日志与诊断信息并打包到桌面"） | —（隐藏） |
| 格式： | —（隐藏） | 标签 x350..388（**右对齐**）+ 下拉 x388..510 |
| 清除引导项 / 生成安装菜单 | x530..612 / x618..706 | **同样显示**，位置相同 |

- 两模式的「清除引导项 / 生成启动菜单」**都显示**（备份模式不是只剩一个主按钮）。
- 主按钮初值两模式都是**灰色**：`UpdateMainAction()` 条件统一为 `!m_wimPath.empty() && m_selPart >= 0 && !m_busy`，选完第一、二步才转蓝。
- `FormatBox` 的条目是 XML 静态写的（`.esd(慢速小体积)` / `.wim(正常大小)` / `.wim(高速大体积)`），首次显示前没有选中项 → 收起框空白，需 `SelectItem(0)` 补默认值。
- **标签与下拉要贴紧**：`CTextItemUI` 内部文字有固定左缩进 `kItemPadX`（`ui_skin.cpp`，2026-09-17 由 10 调到 6）；`格式：` 用 `align="right"` 让文字右边界顶到下拉左边界（x388），间隔只剩下拉内部的 6px。
- 窗口按钮（两模式相同）：最小化/最大化/关闭在 x618..658 / 659..699 / 700..740，图标均 y23..32；最大化方框实心 9×9。
- 第三步右侧控件的 `SetPos` 必须在**两个分支里都写**（切回还原模式要复位），见 `CMainForm::ApplyModeUi()`；「日志」按钮为还原模式专属（备份模式 `Vis=false`）。
- 第一步备注行的横线（"镜像说明/备份备注"右侧）长度与上方输入框**等宽**：x251..710。

**验收环**：`tools/ui/shot.ps1`（启动 → 抓窗口 → PNG；**2026-09-26 改用 `PrintWindow` 抓窗口本体**——原 `CopyFromScreen` 抓的是屏幕，`SetForegroundWindow` 被前台锁定规则拒绝时抓到的是桌面/控制台；抓空回退屏幕方式；并置 `HWND_TOPMOST`。另：`dist\` 根 exe 是 x86 启动器，x64 上自举后父进程退出[ExitCode=0]，脚本会自动跟进 `dist\x64\` 子进程，手动跑建议直接 `-Exe dist\x64\SysRecoverUI.exe`）+ `tools/ui/diff.ps1`（逐像素比对，`Tol=30`）；交互态（模式切换后）用 `PostMessage(WM_LBUTTONDOWN/WM_LBUTTONUP)` 点击后截图。差异率收敛 8.57% → 6.66%（关闭分区行区域）；剩余集中在文字抗锯齿边缘（GDI vs WPF 渲染后端固有差异）与"老图有分区数据 / 本机非管理员读不到"的分区行。**脚本必须保持纯 ASCII**（PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 解析，中文注释会直接把解析器打挂；同理 `param()` 必须是脚本第一条语句）。**注意：GUI 现已带 `requireAdministrator` 清单（PIT-018），非管理员会话下这两个脚本启动 GUI 会弹 UAC 并阻塞到超时** —— 要么在管理员 PowerShell 里跑，要么先人工点一次；纯外观改动（不碰 `SysRecoverUI.{rc,manifest}` 与链接规则）时可直接用「界面源码未变 ⇒ 界面未变」论证，免掉截图回归。

## 16. 参考（只认官方，不抄博客）

- wimlib C API：https://wimlib.net/apidoc/
- BCD 命令行：Microsoft Learn `bcdedit` / `adding-boot-entries`
- Win32：`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`（winioctl.h）
- GRUB4DOS：官方 release（含 grldr/grldr.mbr）

---

## 17. 工程纪律与约定（2026-09-26 借鉴自 DreamGrain 电子教室）

> 参考：同作者另一项目「DreamGrain 电子教室」的 AGENTS.md（其工程纪律）。只抄"**机器可校验 / 能防错**"的那几条，不抄它项目特有的东西。

### 17.1 开工序（每个会话 / 换人）
1. 读 `docs/11-接手指南（读我优先）`（现状 / 下一步 / 文档地图）。
2. 跑 **`make check`** —— 其中 `tools/check-docs.py` 会核**"指针"一致性**：
   `version.h` ↔ `AGENTS`/`docs` 的 `contract_version`；救援脚本 `get_task` 读的键 **⊆** Windows 侧（`task.cpp`）写的键；`dist/version.json` ↔ `version.h`。
   **不一致 = 有人忘了同步**（这条是机器可校验的）。
3. 再动手。

### 17.2 编译 / 打包纪律（硬）
- 改完**必须编译通过**，并**重新 `make package`** —— **只有 `dist/` 是交付物**；光改源码不打包等于没改（PIT-077"构建失败还提交"就是没守住这条）。
- 改**救援层脚本**（`bootfiles/alpine/init`、`bootfiles/zjrestore-lite.sh`）后必须 `python tools/build-debian-rescue.py` 重建 initramfs（脚本是打进 initramfs 的）。
- 改**译文**（`tools/i18n-en.py`）后必须 `python tools/i18n-wrap.py --gen-lang` 重新生成 `lang/en.lang` —— **直接改 `lang/en.lang` 会被 `check-i18n` 的 C5 拦下**（只有 `dist/` 是交付物，`lang/en.lang` 要进 `dist/lang/`）。
- 新增/改了 UI 中文字面量后：`python tools/i18n-wrap.py --apply` 回填 `Tr()` → `python tools/i18n-wrap.py --skeleton` 补齐新键 → 填英文 → `--gen-lang`。**顺序不能反**（`--skeleton` 只保留已存在的 value）。
- 提交前顺序：`make check`（单测 + 一致性）→ `make package`。

### 17.3 品牌与术语约束（改界面/文案前必查）
| 项 | 值 | 能不能改 |
|---|---|---|
| 产品中文名 | **九转还原** | 改要全仓同步（见下） |
| 英文名 | **SysRecover** | 用户已定：不改 |
| exe 名 | `SysRecover.exe` / `SysRecoverUI.exe` | 不改 |
| exe 图标 | **只有 `SysRecoverUI.exe` 带品牌图标**（`resources/SysRecover.ico`）；CLI `SysRecover.exe` **不带** | 2026-09-26 用户裁定（区分入口）；**别给 CLI 补图标** |
| 技术标识 | `ZJRESTORE` / `_zjresy*.log` / `zjrestore-lite.sh` / 内核参数 `zjre=1` | **不改**（跨层契约，改要双端同步发版） |
| 链接文案 | GUI 右下角「**讨论**」→ 无忧论坛 `tid=453597` | 只改 `src/gui/main_form.cpp::kSiteUrl` 一处 |
| 维护者 | **DreamGrain**（总项目名） | — |

- 改中文名前先 `grep` 全仓（皮肤 `skin/*.xml`、`main_win.cpp` 窗口标题、各对话框标题、README/docs），改完**再 grep 一次确认无残留**；**注意别误改作者/维护者署名**。

### 17.4 AI 工具资源纪律
- 并发搜索工具（`glob`/`grep`/底层 rg）**一次最多 2~4 个**，严禁成批几十上百（会瞬间占满 CPU/内存、整机卡死）。
- 能用 `Read` 直接读已知路径就**不搜索**；一次搜索能解决的不拆成多次；发之前先想清"到底要什么"。
- 少跑 PowerShell；命令写短（长命令 + 中文易致 bash JSON `Unterminated string` 解析失败）。

### 17.5 删除暂存区（不直接删）
- 判定"应删"的文件/目录**移入 `docs\老旧文档暂存\`**（只进不出；清空须用户裁定），保留可回溯。明细见该目录 `清单.md`。

### 17.6 崩溃处理（2026-09-26 新增）
- `src/common/crash.{h,cpp}`：`SetUnhandledExceptionFilter` 捕获**未处理异常** → 写
  `<exeDir>\logs\crash\crash-<时间>-<pid>.{dmp,txt}` + `last.txt`（dump 用系统 `dbghelp.dll` 的
  `MiniDumpWriteDump`，**动态加载、零链接依赖**）。
- 文本里含：异常码/地址、访问违例方向+地址、**调用栈（地址 + 所属模块 + 偏移）**、命令行、版本。
- CLI/GUI 入口（`main()`/`WinMain()` 最开头、位数自举之后）各装一次；`diag --zip` 会把 `logs\` 一起打包，用户发回来即可定位。
- 回归：**`make crash-test`**（故意触发访问违例，验证 dump/txt 真能落盘）。
---

## 18. 国际化纪律（i18n，PLAN §14；2026-09-27 M2+M3 落地）

> 一句话：**键 = 中文源文本**；中文界面不查表，其它语言查 lang/<tag>.lang；漏了也只是显示原文，不会崩。

### 18.1 机制（src/common/i18n.{h,cpp}）
| 项 | 值 |
|---|---|
| 键 | **中文源文本本身**（Tr(L开始备份系统) → key 就是 开始备份系统）。没有 T(id) 消息 ID |
| 语言判定 | InitI18n(forced)：显式 --lang xx → SYSRECOVER_LANG → GetUserDefaultUILanguage()（主语言低 10 位 == 0x04 → zh-CN，否则 en） |
| 回退链 | lang/<tag>.lang（主）→ lang/en.lang（通用）→ **源文本**（永不掉 key、永不崩界面） |
| 词典位置 | <AppDir()>\lang\*.lang（AppDir 在 exe 位于 x86//x64/ 时自动上移一级，与 skin/bootfiles 同规则） |
| 皮肤 XML | **文件不改**：LoadSkinXml() 读 UTF-8 源文件、按词典把引号属性值翻成宽串，返回以 < 开头的内存 XML 给 Duilib（UIDlgBuilder.cpp:17，GetResourceType() 默认 UILIB_FILE 才走这条路） |
| 宽串 | Tr(const wchar_t*) 先查 g_wide 缓存 → W2U8 后查窄表 → 命中再 U2W 存回缓存（节点地址不受 rehash 影响，可长期返回） |

### 18.2 .lang 格式（key=value，UTF-8，可带 BOM）
- 每行一条；# 开头 = 注释；空行跳过；**不做 strip** —— 键值可以带前导/尾随空格（' 备份'、'    已用 '）。
- 分隔符 = **首个「未转义」的 =**（\ 会转义紧随其后的字符）。
- 转义：\n \r \t \\ **\=**。\= 必需 —— 备份失败(rc=%d):  这类词条的**键本身含 =**，不转义会被解析器从中间截断（写入端 	ools/i18n-wrap.py::lang_escape、C++ Unescape/LoadTextLocked、门禁 check-i18n.py::split_entry **三端必须同口径**，单测 i18n_escaping_and_equals_split 守着）。
- **译文只能改 	ools/i18n-en.py（EN 表）**，再 python tools/i18n-wrap.py --gen-lang 生成 lang/en.lang。直接改 .lang 会被门禁 C5 拦下。

### 18.3 工作流
1. 新增/改中文 UI 文案（源码写中文字面量，**不要**自己包 Tr）。
2. python tools/i18n-wrap.py --apply 机械化回填（幂等；只动 WRAP_FILES 白名单 14 个文件）。
3. python tools/i18n-wrap.py --skeleton 把新键补进 	ools/i18n-en.py（**已译值原样保留**）。
4. 填英文 value → python tools/i18n-wrap.py --gen-lang → lang/en.lang。
5. mingw32-make -f Makefile check → mingw32-make -f Makefile package（分开跑，见 §17.2）。
- 改**救援层/契约/日志**的文本**不要**走这套（见 18.4）。

### 18.4 翻译边界（红线）
**翻**：GUI 皮肤属性、GUI/CLI 面向用户的提示、dvice 文案、命令帮助。
**不翻**：Log*/AppendHistory/Progress*（机器可读）、跨层契约（
estore-task.conf / _zjresy*.log / progress.json）、救援层屏幕、**品牌名**（九转还原→SysRecover、SysRecover、一键还原恢复环境→SysRecover Recovery Environment）。
> 门禁 C1 会扫**全部** src/**/*.{cpp,h}：每个含 CJK 的字符串 run，callee 必须是 Tr 或 SKIP_CALLS 白名单。

### 18.5 门禁 	ools/check-i18n.py（已挂 make check）
| 码 | 拦什么 |
|---|---|
| C1 | 源码里该翻却没包 Tr() 的中文字面量 |
| C2 | 键在 lang/en.lang 缺失 / .lang 里有源码已不存在的**死键**（改文案忘了 --gen-lang） |
| C3 | 英文 value 里残留汉字（漏译/半译） |
| C4 | 中英 **printf 占位符序列**不一致（%d %u %s %ls %zu %.1f %04X %3d%%，%% 不计） |
| C5 | lang/en.lang 与 	ools/i18n-en.py 不同步、或译文表有空 value |

### 18.6 常用命令
`sh
python tools/extract-strings.py            # 字符串账本 → build/msg-ledger.tsv（统计口径，≠词条数）
python tools/i18n-wrap.py                  # 演练：列出将做的回填
python tools/i18n-wrap.py --apply          # 落盘回填（幂等）
python tools/i18n-wrap.py --skeleton       # 补新键 / 报进度 N/M
python tools/i18n-wrap.py --gen-lang       # 译文表 → lang/en.lang
python tools/check-i18n.py                 # 单独跑门禁
SysRecover.exe --lang en help              # CLI 指定语言
set SYSRECOVER_LANG=en && SysRecover.exe   # 环境变量指定语言（机房批量）
`
- 当前词条：**344**（en.lang 约 37 KB）。每增一语种 ≈ +37 KB。
