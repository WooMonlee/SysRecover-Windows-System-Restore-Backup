# AGENTS.md - SysRecover（一键还原，C++ 重写）开发指南

> 🆕 **第一次接手 / 不知道从哪看起？先读 [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md)**
> —— 现状（已验证 vs 待验证）、下一步优先级、文档地图、以及"AI 环境丢了怎么续上"。
>
> 🆕 **换机器 / 新环境？先看 [`docs/10-新环境交接说明.md`](docs/10-新环境交接说明.md)**
> —— 里面是：**复制哪些文件夹**、**必须保持一致的绝对路径**（工具链/QEMU 写死在 Makefile 与 tools 里）、
> 新环境开工三步（编译 → `diag` 自检 → QEMU 回归），以及**已踩过的坑**。
> 本文件继续作为**操作手册**（§0 红线 / §7 引导 SOP / §13 坑位册）。

> 面向 AI Agent 和开发者。地位：操作手册（怎么做）。路线图见 PLAN.md，Linux 引导契约见 docs/boot-contract.md（如缺则以本文件 §5 为准）。
> 文档版本：v2.1（C++ 新项目 + Linux 层定稿）/ 最后更新：2026-09-15 / 维护者：知鉴
> 旧 C# 原型（`D:\Prog\_Project\SysRestore\src/`）已冻结，仅作设计蓝本，不移植代码。

---

## 0. 开工前必查（5 条，进任务前必读）

1. PE 红线：读分区/磁盘信息**禁用 WMI**（PE 不可用），只用 Win32 API；设引导（BCD）只在正常 Windows 执行。
2. BCD 红线：严格按 §7 成熟方案（`\grldr.mbr` + `/application bootsector` + GUID 回显判断），严禁自行发明。
3. License 红线：只动态链接 `libwim`（LGPLv3）；**严禁抄 `wimlib-imagex`（GPLv3）源码、grub4dos 源码、Dism++ 主程序**（闭源）。
4. 构建红线：钉版工具链 + Release x64 + 零依赖验证 + 体积门禁（§3）。
5. 契约红线：改 `restore-task.conf` / `progress.json` / `_zjresy*.log` 字段必须双端（Windows + Linux）同步发版。

---

## 1. 项目快照

- **产品**：一键还原（单机版）/ SysRecover，原生 C++，零运行时依赖，目标体积 **< 10 MB**。
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

---

## 3. 工具链与构建发布（钉版；唯一允许的绝对路径是 MinGW 本体）

| 项 | 要求 |
|---|---|
| 编译器 | **MinGW-w64 GCC 14.2.0**（`D:\Prog\ProgIDE\mingw64\bin\g++.exe`，x86_64-w64-mingw32，posix-seh），`_WIN32_WINNT=0x0601`（Win7 兼容） |
| 构建 | `mingw32-make` + 手写 `Makefile`（无 CMake；make 来自 `D:\Prog\ProgIDE\mingw64\bin\mingw32-make.exe`），禁止写死其他绝对路径 |
| 配置 | **Release + x64**（另需 x86 构建时再加）；`-O2`；静态链接优先 **`-static`**（PE 零依赖，MinGW 无 `/MT` 概念）；子系统按 exe 区分（CLI=`-mconsole`，GUI=`-mwindows`）；提权清单用 windres 编入 **GUI 与 CLI 两个 exe**（`src/gui/SysRecoverUI.rc` / `src/cli/SysRecover.rc` + 同名 `.manifest` → `build/*_rc.o`，见 PIT-018） |
| libwim 引入 | 预编译 `wimlib.h + libwim.lib + libwim-15.dll` 放 `third_party/wimlib/`；运行时 DLL 与 EXE 同目录分发；**严禁静态链接 libwim，严禁抄 `wimlib-imagex.c`** |
| Duilib 引入 | **已换库：经典 `Duilib`（MIT/BSD）**，`third_party/duilib-master/`（35 cpp，静态库 `build/libduilib.a`）。原因：`nim_duilib` 运行时强制 Skia（`GlobalManager` 无 GDI 回退），Skia 体积违背 <10MB 目标，弃用（源码留存 `third_party/nim_duilib-main/` 不再编译）；MinGW 移植补丁见 PIT-012；XML 皮肤随包 `dist/skin/`，禁止依赖外部散文件 |
| 构建三命令 | `mingw32-make -f Makefile all` / `mingw32-make -f Makefile clean` / `mingw32-make -f Makefile package`（Makefile 头部写死 MinGW 路径 `D:/Prog/ProgIDE/mingw64`，其余用相对路径） |
| 救援层构建 | `python tools/vmtest/build-alpine-initramfs.py`（从 `tools/vmtest/dl/alpine/*.apk` 组装 `bootfiles/{vmlinuz-zjrestore,initramfs-zjrestore.cpio.gz}`；**纯 Windows/Python，无需 Linux 环境**，见 PIT-044/045/046） |
| 输出物 | `SysRecover.exe` + `libwim-15.dll` + `boot/{vmlinuz,initramfs,restore.sh,grldr,grldr.mbr,menu.lst模板}` + `version.json` + SHA256 |
| 门禁 | 体积检查（见 PLAN.md §1 体积目标 < 10 MB）+ `objdump -p` / `x86_64-w64-mingw32-objdump` 或 Dependencies 零依赖检查 + `diag` 自检通过 |

版本号：SemVer `主.次.修订` —— **唯一来源 `src/common/version.h`**（`SYSRECOVER_VERSION`），
`version` 命令、`_zjresy*.log` 的 `software_version`、GUI 标题栏副标题、`dist/version.json` 全部由它派生。
**规则见 `PLAN.md`「版本号规则」**：主/次版本**由用户指定**（`python tools/version.py --set X.Y.Z`），
**修订号每解决一个问题 +1**（提交前跑 `python tools/version.py --bump`）。发布时与 git tag `vX.Y.Z` 对齐。

> **架构：目前只出 x64**（2026-09-20 用户实测：32 位 Win7 上 exe 直接起不来，系统层面拒绝、程序内无法提示）。
> 要出 x86 需三样：① 另装 **i686-w64-mingw32** 工具链；② 官方 **32 位 `libwim-15.dll`**
> （wimlib 同时发布 `windows-i686` / `windows-x86_64` 包）；③ Makefile 加 `ARCH=x86` 分支。
> **救援层与宿主位数无关**（Linux 侧照旧）；但 **UEFI 引导资产是 x64**（`shimx64.efi` + 64 位内核），
> 32 位 UEFI（IA32）需另找 `shimia32.efi` + 32 位内核 —— 极少见，32 位机器基本都走 **BIOS + GRUB4DOS** ✓。

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
| `restore-task.conf` | 软件目录 | 还原任务参数（key=value：action/pt_type/image_part_guid/image_rel_path/image_path/image_index/target_guid/target_offset/target_size/target_disk_serial/repair_boot/partition_count） |
| `restore-task.json` | 软件目录 | 同上 JSON 形态 |
| `progress.json` | logs 目录 | 实时进度（Phase/Percent/Status/Detail/UpdatedAt/Pid，供 AI/外部工具读） |
| `_zjresy*.log` | **目标分区根**（C:） | 主发现契约（由 `WriteRestoreLog` 写）：action=restore / log_time / software_version / software_path / target_disk_name / target_disk_serial / target_disk_size / target_part_offset / target_part_size / target_fs / target_vol_label / image_path / image_index / repair_boot / pt_type |
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

**诊断日志与清理（PIT-058/059）**：日志写**软件目录** `<exeDir>\logs\`（conf 的 `software_dir=` 键；软件在目标盘/只读介质上时回退 `<数据盘>\ZJRESTORE`），**成功也保留**供日后排查。
**还原成功后**：目标分区被格式化 → 目标盘上的救援文件 + 引导期日志 `zjrestore-boot.log` **自然消失**；Linux 侧只清旧版本遗留在数据盘根目录的引导文件（不碰软件目录日志/镜像）。**失败时**：引导期日志留在目标根（= 引导阶段没走完的信号），软件目录的日志用于排错。

**条目存在性判定（唯一正确做法）**：`bcdedit /enum {GUID}`，看输出是否**回显该 GUID**（存在约 200 字节，不存在约 15 字节"没有匹配的对象"；**两者退出码都是 0**）。GUID 是 ASCII，不受编码影响。

**PowerShell 提醒**：标识符必须加引号（`bcdedit /enum '{GUID}'`），因 PS 把 `{}` 当脚本块；C++ 的 `CreateProcessW` 直接传参，无此问题。

**安全**：`/bootsequence` 是单次启动，失败重启自动回 Windows；改 BCD 前先 `bcdedit /export` 备份。

### UEFI/GPT 分支（方案：内核 EFI stub + 固件启动项 Boot#### 直启，见 PIT-060）

- 链路：`UEFI → Boot####（NVRAM 固件启动项，FilePathList 直指内核）→ \EFI\ZJRESTORE\vmlinuz-zjrestore.efi (OptionalData=initrd=...) → 救援 init → zjrestore-lite.sh → 重启回 Windows`。**不经 bootmgr**（bootmgr 的 bootapp 只收 subsystem=16，加载我们的 subsystem=10 内核会 `0xc000007b`），**不用 GRUB2**。
- Windows 侧：`IsUefiFirmware()` 判固件；`FindEspPartition()` + `mountvol X: /s` 挂 ESP；`InstallUefiBootEntry()`（`src/boot/uefi.cpp`）把内核+initramfs 放 `<ESP>\EFI\ZJRESTORE\` 并写 `Boot####`（短格式 HardDrive 节点 + FilePath 节点；OptionalData = UTF-16LE 命令行）+ 挂 `BootOrder` 末尾；`bcdboot <目标>:\Windows /s <ESP>: /f UEFI` 修 ESP 引导；`SetUefiBootNext()` 单次启动。
- Linux 侧：`pt_type=gpt` → **跳过 PBR/引导区修复**（ESP 不动）；**成功保留** ESP 上的 `\EFI\ZJRESTORE\`（常驻恢复模块，仅 Windows 侧「删除启动还原」才清）。
- 安全门禁：GPT 目标**仅 UEFI 固件放行**（`safety.cpp`），BIOS+GPT 仍拒绝。
- **Secure Boot 分支**（PIT-062）：`IsSecureBootEnabled()` 为真时改走 `固件 → shimx64.efi（微软签名）→ grubx64.efi(= 我们签名的 UKI) → systemd-stub → 内核`；`IsMokEnrolled()` 判一次性密钥注册是否完成，未完成则暂存前拦住。构建见 `tools/build-uki.py`。
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

级别：INFO/WARN/ERROR。双写判定：程序目录 `logs/` 为主；PE 只读介质才 fallback `%ProgramData%`。原子写入（`.tmp+rename`）。还原/备份日志同时满足：程序目录 `restore-YYYYMMDD.log` + 目标分区 `_zjresy*.log`。

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

---

## 14. License 合规（SBOM，随版本更新）

| 组件 | 版本/来源 | License | 链接/分发方式 |
|---|---|---|---|
| libwim | 记录版本 + wimlib.net | LGPLv3 | 动态链接，随包放 DLL，保留声明，允许用户替换 |
| Duilib 系 | 记录 fork+commit | BSD/MIT | 静态链接可闭源 |
| grldr/grldr.mbr | 记录来源 URL + SHA | GPL | 仅分发二进制，不修改不链接，独立聚合 |
| wimlib-imagex 源码 | — | GPLv3 | **禁止引入** |
| Alpine linux-lts 6.6.142 | 模块（440+ .ko.gz） | GPLv2 | ⚠️ **内核已换成 Ubuntu 的（见下）**；这些模块不再随包 |
| Alpine busybox-static 1.36.1 | `/bin/busybox` | GPLv2 | 仅分发二进制 |
| Alpine musl 1.2.5 | `ld-musl-x86_64.so.1` | MIT | 仅分发二进制 |
| Alpine ntfs-3g/ntfsprogs 2026.2.25 | `ntfs-3g` + `mkntfs` | GPLv2 | 仅分发二进制 |
| Alpine wimlib 1.14.4 | `wimlib-imagex`（动态链接 libwim） | LGPLv3 | **动态链接**，随包放 `libwim.so.15`，允许用户替换 |
| Alpine util-linux 2.40.1 | `blkid` + `libblkid`/`libuuid`/`libeconf` | GPLv2 / LGPL | 仅分发二进制 |
| **Ubuntu 内核** `linux-image-6.8.0-31-generic` | `vmlinuz-zjrestore` | GPLv2 | 仅分发二进制（未修改，**Canonical 签名**），独立聚合（PIT-066） |
| **Ubuntu 内核模块** `linux-modules-6.8.0-31-generic` | 999 个 `.ko.gz` | GPLv2 | 同上（均带 Canonical 签名） |
| **GRUB**（Ubuntu `grub-efi-amd64-signed` 1.215） | `grub-ubuntu.efi` | GPLv3 | 仅分发已签名二进制（未修改），独立聚合；Secure Boot 链（PIT-066） |
| shim（Ubuntu `shim-signed` 1.59） | `shimx64.efi`（+`mmx64.efi` 仅在备选线） | **BSD-2-Clause** | 仅分发已签名二进制（未修改）；Secure Boot 链入口（PIT-066） |
| ~~systemd-stub / UKI / efiloader~~ | 随 MOK 备选线**停止分发**（`ZJ_ENABLE_MOK_PATH=0`） | — | 仅仓库留存，`dist` 不含 |
| osslsigncode 2.9 | 构建期给 UKI 签名（备选线用） | GPLv3 | **仅构建工具，不进产品** |
| ~~BG-Rescue 9.0.0~~ | 已弃用（PIT-044） | — | — |
| Dism++ 主程序 | — | 闭源 | **禁止抄袭** |

> **豁免依据**：本产品自身代码**未静态链接任何 GPL 组件、未修改任何第三方源码**（`libwim-15.dll`
> 动态链接；其余为「单独分发」的聚合）→ 不受 copyleft 衍生作品条款约束。**是否开源为待决事项**。
> 上表全部内容 + 各许可全文已生成 `THIRD_PARTY_LICENSES.txt`，由 `make package` 拷进 `dist/` ✓。
> 构建/测试期工具（MinGW-w64、osslsigncode、QEMU/OVMF、mtools）**不随产品分发**。

---

## 15. UI 外观设计（1:1 对齐老项目 WPF，2026-09-05 定稿；2026-09-13 逐像素校准完成）

老界面（`SysRestore/src/ZjRestore.Gui/MainWindow.xaml`，870×410，无边框圆角）**外观确认无问题**，C++ 版照抄布局，只换实现（WPF XAML → Duilib XML）。详细映射见 `docs/ui-design.md`，骨架见 `skin/main.xml`。

布局速览（上→下）：
1. **顶栏**（`#f7f9fc`）：左=模式 Tab（Radio：`镜像恢复为系统` / `系统备份为镜像`，选中下划线 `#96aaf0`）+ 中=标题`知鉴一键还原 v0.1` + 右=最小化/最大化/关闭（关闭 hover `#e81123`）。
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

| 第三步右侧（面板坐标，2026-09-17 收窄后实测） | 还原模式 | 备份模式 |
|---|---|---|
| 静默模式 | 勾选框 x420..433 + 文字 x438..494（面板） | 勾选框 x**250**.. + 文字（面板） |
| 格式： | —（隐藏） | 标签 x350..388（**右对齐**）+ 下拉 x388..510 |
| 清除引导项 / 生成启动菜单 | x530..612 / x618..706 | **同样显示**，位置相同 |

- 两模式的「清除引导项 / 生成启动菜单」**都显示**（备份模式不是只剩一个主按钮）。
- 主按钮初值两模式都是**灰色**：`UpdateMainAction()` 条件统一为 `!m_wimPath.empty() && m_selPart >= 0 && !m_busy`，选完第一、二步才转蓝。
- `FormatBox` 的条目是 XML 静态写的（`.esd(慢速小体积)` / `.wim(正常大小)` / `.wim(高速大体积)`），首次显示前没有选中项 → 收起框空白，需 `SelectItem(0)` 补默认值。
- **标签与下拉要贴紧**：`CTextItemUI` 内部文字有固定左缩进 `kItemPadX`（`ui_skin.cpp`，2026-09-17 由 10 调到 6）；`格式：` 用 `align="right"` 让文字右边界顶到下拉左边界（x388），间隔只剩下拉内部的 6px。
- 窗口按钮（两模式相同）：最小化/最大化/关闭在 x618..658 / 659..699 / 700..740，图标均 y23..32；最大化方框实心 9×9。
- 第三步右侧三个控件的 `SetPos` 必须在**两个分支里都写**（切回还原模式要复位），见 `CMainForm::ApplyModeUi()`。
- 第一步备注行的横线（"镜像说明/备份备注"右侧）长度与上方输入框**等宽**：x251..710。

**验收环**：`tools/ui/shot.ps1`（启动 → 抓窗口 → PNG）+ `tools/ui/diff.ps1`（逐像素比对，`Tol=30`）；交互态（模式切换后）用 `PostMessage(WM_LBUTTONDOWN/WM_LBUTTONUP)` 点击后截图。差异率收敛 8.57% → 6.66%（关闭分区行区域）；剩余集中在文字抗锯齿边缘（GDI vs WPF 渲染后端固有差异）与"老图有分区数据 / 本机非管理员读不到"的分区行。**脚本必须保持纯 ASCII**（PowerShell 5.1 对无 BOM 的 `.ps1` 按 ANSI 解析，中文注释会直接把解析器打挂；同理 `param()` 必须是脚本第一条语句）。**注意：GUI 现已带 `requireAdministrator` 清单（PIT-018），非管理员会话下这两个脚本启动 GUI 会弹 UAC 并阻塞到超时** —— 要么在管理员 PowerShell 里跑，要么先人工点一次；纯外观改动（不碰 `SysRecoverUI.{rc,manifest}` 与链接规则）时可直接用「界面源码未变 ⇒ 界面未变」论证，免掉截图回归。

## 16. 参考（只认官方，不抄博客）

- wimlib C API：https://wimlib.net/apidoc/
- BCD 命令行：Microsoft Learn `bcdedit` / `adding-boot-entries`
- Win32：`IOCTL_DISK_GET_DRIVE_LAYOUT_EX`（winioctl.h）
- GRUB4DOS：官方 release（含 grldr/grldr.mbr）
