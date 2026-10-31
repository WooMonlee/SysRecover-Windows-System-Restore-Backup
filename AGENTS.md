> ⚠️ **当前版本见 `src/common/version.h`**（唯一来源；`0.9.49`）。公开前准备、发布闸门与 1.0 观察期见 [`docs/26`](docs/26-公开前准备与1.0观察期.md)；**PIT 正文已搬到 [`docs/25`](docs/25-PIT坑位册.md)**，本文件 §13 只留活跃约束索引。

# AGENTS.md - SysRecover（一键还原，C++ 重写）开发指南

> **当前状态（2026-10-28）**：主线 = **0.9.x**（0.9.0 异机还原 → 0.9.4x 分区/引导自足与界面收口）；下一步 = 真机矩阵回归 + 1.0 收口（`docs/26`）。
> **封存线** = **0.6.54**（Linux apply 架构，tag `v0.6.54`）：现状/未闭环/资产见 [`docs/20-项目封存说明（当前线，2026-10-09）`](docs/20-项目封存说明（当前线，2026-10-09）.md)。
> **新线（0.7 起）** = A 引擎（Windows 原生解压 + Linux 仅改名交换）：协议见 [`docs/22-P0原型验证计划（0.7）`](docs/22-P0原型验证计划（0.7）.md)，方案复审见 [`docs/23`](docs/23-0.7方案复审与最小自测清单（专家组）.md)。
> 其它入口：[`docs/21`](docs/21-新软件策划与计划（专家组）.md)（策划）｜[`graveyard/README.md`](graveyard/README.md)（弃码不删，移此）。
> 测试线：单元测试在 `tests/`，`make check` = 单测 + 版本自检 + 文档/i18n/宽度/shell/分层/配置/密钥门禁；`make crash-test` 可跑崩溃落盘回归。**QEMU 冒烟脚本仍封存在 `graveyard/`**（随 docs/21 P3 矩阵再定）。
> **以下内容为操作手册**（§0 红线 / §7 引导 SOP / §13 活跃坑位索引 / §17 纪律 / §18 i18n），对封存线与新线均有效。

> 🆕 **第一次接手 / 不知道从哪看起？先读 [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md)**
> 🆕 **换机器 / 新环境？先看 [`docs/10-新环境交接说明.md`](docs/10-新环境交接说明.md)**（复制哪些目录、必须一致的绝对路径、开工三步、已踩坑）

> 面向 AI Agent 和开发者。地位：操作手册（怎么做）。路线图见 PLAN.md，Linux 引导契约见 docs/boot-contract.md（如缺则以本文件 §5 为准）。
> 最后更新：2026-10-28 / 维护者：DreamGrain
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
| `export-drivers [--dest <目录>]` | 只导出**本机启动关键**驱动（存储/输入类），供异机还原；不是 1.8GB 全量 |
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

级别：INFO/WARN/ERROR。**日志根**（PIT-150，取代 PIT-104/105 的规则；`LogBaseDir()`，`src/common/relocate.cpp`）四级链：① 已安装的程序目录 `<系统盘>\Program Files\SysRecover\logs`（可写就用）→ ② 程序所在文件夹 → ③ 只读介质时 = 除系统盘外第一个可写固定分区的 `\ZJRESTORE`（**设隐藏**）→ ④ 都不可写 = 退回程序目录 + `LogHomeDegraded()`（日志保不住，启动提示）。**PE 例外**：`X:\`（RAM 盘）与"可能是离线系统盘的程序目录"都不用，日志一律落数据盘。Windows 日志、救援回写、BCD 备份、`history.jsonl`、`crash\` dump、`diag --zip` 全部落在这一个 `logs\` 下（`diag` 会打印 `log_dir=`）。**克隆（备返）后首启**：程序自动运行（RunOnce `ZJSysRecoverFirstBoot`）→ `MigrateLogsToLogHome()` 把 `<数据盘>\ZJRESTORE\logs` 与 `<数据盘>\ZJRESTORE-logs` 搬回日志根（不覆盖同名）→ **然后**才弹「是否删除旧系统」。**启动还会自动生成 `diag.txt`/`list.txt` 并收集部署痕迹到 `logs\collected\`（PIT-107）—— 用户排错只需发 logs 文件夹。**原子写入（`.tmp+rename`）。还原/备份日志同时满足：日志根 `logs\SysRecover-YYYYMMDD.log` + 目标分区 `_zjresy*.log`。

---

## 11. 安全红线（CLI/GUI 共用检查表）

四元素校验（GUID/序列号/偏移/大小）fail-closed；**还原四检查**：ESP 分区 / BitLocker / 恢复分区 / **镜像文件在目标分区内**（命中任一即拒绝，需用户先移走镜像）；改 BCD 前 `/export` 备份；危险操作（格式化目标分区）必须二次确认（`--yes` 才能跳过）；管理员检查。

---

## 12. 测试矩阵

`Win7/10/11/PE × smoke/diag+list+verify / stage（暂存不重启）/ full（真重启进 Linux）`。BCD 用例只在正常 OS 跑，不在 PE 跑。`tools/pe-diag` 全绿为合入门禁。

---

## 13. 坑位登记册（PIT-xxx）

> **PIT 全量正文已搬到 [`docs/25-PIT坑位册.md`](docs/25-PIT坑位册.md)**（187 条，约 370 KB）——
> 本文件只留**仍然活跃的约束**索引（63 条），避免每次会话注入 400 KB 上下文。

> ⚠️ **动手改 `ops.cpp` / `zjrestore-lite.sh` / `bcd.cpp` / `uefi.cpp` / `bootself.cpp` / `main_form.cpp` 之前，
> 先 `grep -n "PIT-" <该文件>`，再到 docs/25 查正文**（代码注释只写编号，全仓 600+ 处）。

> - 新增坑：**追加到 `docs/25` 末尾**（取下一个空号；编号只增不改/不重排/不复用）；
>   若属"今天违反就出事"的铁律，再在本索引 §13.1 加一行。
> - 索引行格式：`- PIT-NNN · ≤30 字结论 · [正文](docs/25-PIT坑位册.md#pit-nnn)`（不放日期/版本/路径）。
> - 编号勘误：`PIT-066` 两条（后者"零注册正解"为现行）；`PIT-136` 原空号，正文已补在 docs/25。
> - 历史作废（保留勿用）：`PIT-022~043`（BG-Rescue 底座）、`PIT-052/061/063/065/066-一`。

### 13.1 铁律（违反即出事，改相关代码前必看）

- PIT-007 · MinGW `wprintf` 的 `%s` 是窄字符串 · [正文](docs/25-PIT坑位册.md#pit-007)
- PIT-008 · `WIMLIB_*_FLAG_NTFS` 是裸卷模式，不是"… · [正文](docs/25-PIT坑位册.md#pit-008)
- PIT-018 · MinGW 下给 exe 嵌提权清单（GUI 启动即弹 UA… · [正文](docs/25-PIT坑位册.md#pit-018)
- PIT-045 · Windows 上打包 initramfs 必须显式带权限位 · [正文](docs/25-PIT坑位册.md#pit-045)
- PIT-082 · 启动器未嵌 `requireAdministrator` →… · [正文](docs/25-PIT坑位册.md#pit-082)
- PIT-092 · UEFI 固件 + MBR 盘（CSM/Legacy 装的… · [正文](docs/25-PIT坑位册.md#pit-092)
- PIT-128 · 目标盘误判隐患 · [正文](docs/25-PIT坑位册.md#pit-128)
- PIT-135 · 客户 20261008 两问题 · [正文](docs/25-PIT坑位册.md#pit-135)

- PIT-187 · AGENTS 体积：PIT 正文写 docs/25，本文件 ≤80KB · [正文](docs/25-PIT坑位册.md#pit-187)
- PIT-188 · 加密镜像只能整包解到临时明文（wimlib 只收路径） · [正文](docs/25-PIT坑位册.md#pit-188)
- PIT-189 · 异机还原只注入启动类驱动；单包失败不再整单失败 · [正文](docs/25-PIT坑位册.md#pit-189)
- PIT-190 · 「删除旧系统」进度窗挡对话框/权限兜底（开机任务） · [正文](docs/25-PIT坑位册.md#pit-190)
- PIT-191 · ESP 空间按"净增量"算，别拿全量报"差 0.1MB" · [正文](docs/25-PIT坑位册.md#pit-191)
- PIT-192 · 新系统缺 `%TEMP%` → wimlib rc=48 误报"镜像缺 ntoskrnl" · [正文](docs/25-PIT坑位册.md#pit-192)
- PIT-193 · 32 位进程读 System32 要走 Sysnative；引导预检不许 fail-closed · [正文](docs/25-PIT坑位册.md#pit-193)

### 13.2 未闭环（正文带 🚧，别当"已经好了"）

- PIT-055 · Alpine 救援层在 VMware/QEMU 虚拟 NVM… · [正文](docs/25-PIT坑位册.md#pit-055)
- PIT-060 · UEFI/GPT 分支 · [正文](docs/25-PIT坑位册.md#pit-060)
- PIT-063 · Secure Boot 两种走法对比 · [正文](docs/25-PIT坑位册.md#pit-063)
- PIT-064 · 就地还原：目标分区没被占用时直接写，不重启（用户规格 · [正文](docs/25-PIT坑位册.md#pit-064)
- PIT-066 · 零注册候选 · [正文](docs/25-PIT坑位册.md#pit-066)
- PIT-126 · 论坛反馈核实（Win11 精简版装菜单失败）+ 两个真 bu… · [正文](docs/25-PIT坑位册.md#pit-126)
- PIT-137 · 黑匣子归并 · [正文](docs/25-PIT坑位册.md#pit-137)
- PIT-139 · 机房测试四问题 · [正文](docs/25-PIT坑位册.md#pit-139)
- PIT-140 · 启动期弹框一律 `owner=nullptr` → 确认框藏… · [正文](docs/25-PIT坑位册.md#pit-140)
- PIT-143 · 演练时有时无假失败 = 契约日志缺失时无条件全量扫 4000… · [正文](docs/25-PIT坑位册.md#pit-143)
- PIT-146 · 救援层能力自检 `caps_selfcheck`（P1 · [正文](docs/25-PIT坑位册.md#pit-146)
- PIT-148 · P3 失败自动回退全链（STAGE 决策 + AUTO-RE… · [正文](docs/25-PIT坑位册.md#pit-148)
- PIT-152 · 「安装菜单」点击后 GUI "(无响应)"（装/删菜单同步跑… · [正文](docs/25-PIT坑位册.md#pit-152)
- PIT-153 · PE/U盘就地还原不再限制 BIOS/UEFI 装法（删 G… · [正文](docs/25-PIT坑位册.md#pit-153)
- PIT-154 · Q1「先落盘后搬家」（SettleHome · [正文](docs/25-PIT坑位册.md#pit-154)
- PIT-155 · Q1 SettleHome 三处加固 · [正文](docs/25-PIT坑位册.md#pit-155)
- PIT-157 · 1019 案「PIT-135 补课」 · [正文](docs/25-PIT坑位册.md#pit-157)
- PIT-158 · P4 用户试用反馈三项（`0.7.74` · [正文](docs/25-PIT坑位册.md#pit-158)
- PIT-159 · 新硬盘初始化向导 + 第三步启动方式提示 + GUI「修复引… · [正文](docs/25-PIT坑位册.md#pit-159)
- PIT-160 · 异机还原（0.9 · [正文](docs/25-PIT坑位册.md#pit-160)
- PIT-161 · 镜像路径输入汉字跑到最前面（UNC 路径 · [正文](docs/25-PIT坑位册.md#pit-161)
- PIT-162 · 网络共享（Unicode 机器名… · [正文](docs/25-PIT坑位册.md#pit-162)
- PIT-163 · 备份保存路径与还原镜像路径分离 + 任务执行中锁定 UI +… · [正文](docs/25-PIT坑位册.md#pit-163)
- PIT-164 · rc=47 真根因 = VSS 快照内 OneDrive 应… · [正文](docs/25-PIT坑位册.md#pit-164)
- PIT-165 · 还原模式按钮改版（清除菜单/安装菜单同款样式 + 两两换位）… · [正文](docs/25-PIT坑位册.md#pit-165)
- PIT-170 · 日常还原临时菜单与「安装菜单」分离 · [正文](docs/25-PIT坑位册.md#pit-170)
- PIT-171 · CLI 参数穷举校验 · [正文](docs/25-PIT坑位册.md#pit-171)
- PIT-173 · 用户四项实测 · [正文](docs/25-PIT坑位册.md#pit-173)
- PIT-175 · 发布入口重构 · [正文](docs/25-PIT坑位册.md#pit-175)
- PIT-176 · 还原确认框按钮被裁 · [正文](docs/25-PIT坑位册.md#pit-176)
- PIT-177 · PE 10586 新分区 GPT 仍报“找不到 ESP” · [正文](docs/25-PIT坑位册.md#pit-177)
- PIT-178 · 停止猜 ESP · [正文](docs/25-PIT坑位册.md#pit-178)
- PIT-179 · ESP-diag 定因 · [正文](docs/25-PIT坑位册.md#pit-179)
- PIT-180 · 用户六项界面改动 · [正文](docs/25-PIT坑位册.md#pit-180)
- PIT-181 · 用户四项需求 · [正文](docs/25-PIT坑位册.md#pit-181)
- PIT-184 · 极简 Win8PE「分区与 BIOS/UEFI 引导全部免… · [正文](docs/25-PIT坑位册.md#pit-184)
- PIT-185 · 被安全软件拦写时给「重试」而不是让用户猜 · [正文](docs/25-PIT坑位册.md#pit-185)
- PIT-186 · 「安装到启动」改三选（扫描/解压/取消）+ 顺手挖出潜伏 b… · [正文](docs/25-PIT坑位册.md#pit-186)

### 13.3 近期（0.9.x，防回归；全量见 docs/25）

- PIT-141 · 「返回旧系统」失败时证据不足 →… · [正文](docs/25-PIT坑位册.md#pit-141)
- PIT-142 · 「返回旧系统」必失败（客户实测 rc=1）根因 · [正文](docs/25-PIT坑位册.md#pit-142)
- PIT-144 · bulk 5 万文件压测 · [正文](docs/25-PIT坑位册.md#pit-144)
- PIT-145 · 统一诊断底座 `zjlog.sh`（P0-B · [正文](docs/25-PIT坑位册.md#pit-145)
- PIT-147 · `2>/dev/null` 全量分类清理（245 处盘点，B… · [正文](docs/25-PIT坑位册.md#pit-147)
- PIT-149 · 「删除旧系统」必闪退（`~old` 一个字节没删、下次开机又… · [正文](docs/25-PIT坑位册.md#pit-149)
- PIT-150 · 日志落地规则改写（四级链 +… · [正文](docs/25-PIT坑位册.md#pit-150)
- PIT-151 · 客户实测「按 ESC 不是启动菜单」→ 开机键文案不再写死… · [正文](docs/25-PIT坑位册.md#pit-151)
- PIT-156 · 还原残留归零 · [正文](docs/25-PIT坑位册.md#pit-156)
- PIT-166 · 0.9.10 质量检查 · [正文](docs/25-PIT坑位册.md#pit-166)
- PIT-167 · 客户 0.9.9 仍 rc=47，真凶换成… · [正文](docs/25-PIT坑位册.md#pit-167)
- PIT-168 · 不再按软件逐个排除 · [正文](docs/25-PIT坑位册.md#pit-168)
- PIT-169 · 跳过文件必须让用户知道 · [正文](docs/25-PIT坑位册.md#pit-169)
- PIT-172 · 所有下拉框用上下键只能选到最下方一项 · [正文](docs/25-PIT坑位册.md#pit-172)
- PIT-174 · 用户三项：①还原确认框矮 ②异机注入阶段 100% 后长时间… · [正文](docs/25-PIT坑位册.md#pit-174)
- PIT-182 · 双击根目录 `SysRecoverCheck.exe` 不弹… · [正文](docs/25-PIT坑位册.md#pit-182)
- PIT-183 · 「镜像检查」确认框标题只显示半句、右边超出外框 · [正文](docs/25-PIT坑位册.md#pit-183)

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

### 17.7 沟通语言
- **与用户交流一律用中文**（维护者中文母语、英文弱）：回复、方案说明、提问、总结都写中文。
- 代码标识符、日志、命令行输出仍按各自既有规范（英文/契约），不受此条影响。

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
