# SysRecover（一键还原，C++ 重写）- 总计划书

> 文档版本：v2.2（C++ 新项目 + ops 共享层 + Linux 层定稿）/ 最后更新：2026-09-15 / 维护者：DreamGrain
> 与 AGENTS.md 的关系：本文件是路线图（做什么、何时算完）；AGENTS.md 是操作手册（怎么做）。

---

## 版本号规则（自 0.1.3 起生效）

**格式**：`主.次.修订`（SemVer 风格）——`主`/`次` **由人指定**，`修订` **每解决一个问题自动 +1**。

| 谁动 | 什么时候 | 怎么动 |
|---|---|---|
| **修订号**（第 3 位）| **每解决一个问题**（一个 bug / 一个明确的缺陷修复）就 +1，多个问题连续修就连续 +1 | `python tools/version.py --bump` |
| **次版本号**（第 2 位）| 由**用户/负责人**指定：一批功能做完、口径变化 | `python tools/version.py --set 0.2.0` |
| **主版本号**（第 1 位）| 由**用户/负责人**指定：架构/契约不兼容的大改动（如跨层契约破坏性变更） | `python tools/version.py --set 1.0.0` |

**唯一来源**：`src/common/version.h` 的 `SYSRECOVER_VERSION`。改它一处，下列各处自动跟随，**不允许各自写死**：

| 位置 | 如何跟随 |
|---|---|
| CLI `version` / `diag` 输出 | 直接用宏（`src/cli/main.cpp`）|
| `_zjresy*.log` 的 `software_version` | 直接用宏（`src/boot/task.cpp`）|
| GUI 标题栏副标题 `vX.Y.Z` | `CTitleLabelUI` 用 `SYSRECOVER_VERSION_W` 初始化（`skin/main.xml` 不再写版本号）|
| `dist/version.json` | `Makefile` 调 `tools/version.py` 生成 |
| 发布 tag | 打 tag 时手工对齐：`vX.Y.Z` |

**约定**：① 每修完一个问题、**提交前**跑 `--bump`（一次提交 = 一次修订号递增，除非该提交纯属文档/重构）；② `SYSRECOVER_CONTRACT_VERSION`（跨层契约版本）**另计**，仅当 `restore-task.conf` / `progress.json` / `_zjresy*.log` 字段变化时才动，且必须双端同步发版；③ 对外发布时必须三处同源：`version.h`、`dist/version.json`、git tag。

**例**：本次（2026-09-20 换机测试）修掉换机蓝屏（BCD 残留旧设备）、`menu.lst` 第二次部署失败、`bootfix\bootmgr` 第二次覆盖失败 → `0.1.0 → 0.1.3`。

---

## 0. 资产继承（从 SysRestore C# 原型复用，不重验）

| 资产 | 状态 | 说明 |
|---|---|---|
| Linux 引导层（vmlinuz + initramfs + restore.sh） | ✅ 既有资产 | 2026-09-17 验证：**Alpine linux-lts 6.6 底座**（自组装 initramfs + 自写 `/init` + 全套存储模块，QEMU 全链通过）；见 PLAN.md §4 |
| GRUB4DOS 方案（grldr.mbr + grldr + menu.lst `kernel/initrd`） | ✅ 直接复用 | 2026-09-05 已验证打通 |
| BCD 五命令 + GUID 回显判定 + NeedsInstall 四项检查 | ✅ 直接复用 | 见 AGENTS.md §7，严禁发明 |
| restore-task / progress.json / `_zjresy*.log` 契约 | ✅ 冻结复用 | 改字段需双端发版 |
| C# 业务逻辑 | 📖 仅作蓝本 | 不移植代码，只参考流程 |

---

## 1. 里程碑总表（唯一真源，Phase 0–6）

| Phase | 目标 | 工期 | 门禁 |
|---|---|---|---|
| 0 准备 | MinGW + Makefile 骨架 + libwim 调通（`third_party/wimlib/`） + Duilib 定库 + grldr 入库 | 0.5 周 | `SysRecover.exe version/diag` 可运行；小目录→WIM 成功；体积基线建立 |
| 0 进展（2026-09-05） | 🚧 Makefile + `src/cli/main.cpp`（version/diag）已编过；`wimlib_global_init→0`；exe **521KB**；`bootfiles/` 五件齐；**Duilib 已定库** | — | ✅ `nim_duilib`（MIT）入库（`third_party/nim_duilib-main/` + zip 留底，SHA 见 `third_party/SBOM.md`）；头文件语法检查 + Core 61 个 cpp 全编过；⬜ 小目录→WIM 待 Phase 2 |
| 1 磁盘库 | DeviceIoControl 枚举/MBR-GPT/盘符映射/管理员判断，禁 WMI | 1 周 | CLI `list` 与 C# 原型分区表类型一致；PE 下 pe-diag 全绿 |
| 1 进展（2026-09-05） | ✅ `src/disk/`（`disk.h/disk.cpp`）+ CLI `list` 已通；本机 Disk 0/MBR/C:[System]/D: 与原型一致；exe 668KB；零警告 | — | ⬜ PE 实测待补（`tools/pe-diag` 未建）；路上踩出 PIT-007（wprintf `%s` 须用 `%ls`） |
| 2 WIM 引擎 | 捕获/应用/校验/append 增量/`--snapshot` 热备/progress 回调 + 取消 | 1 周 | 热备 C 盘 + 百分比 + `verify` 通过；第二次 `append` 增量用例通过 |
| 2 进展（2026-09-05） | ✅ `src/wim/`（直连 libwim）+ CLI 四命令已通；小目录捕获/应用/追加/校验全循环通过；**C 盘热备实测通过**（14GB→6.8GB，fast，100%，verify 通过）；exe 741KB；零警告 | — | 路上踩出 PIT-008（NTFS 标志是裸卷模式）+ PIT-009（热备必须传排除配置，否则 rc=88）；`src/wim/exclude.cpp` 已落地 |
| 3 引导层 | BCD 五命令 + menu.lst 生成 + 任务暂存 + NeedsInstall 四检 | 0.5–1 周 | 正常 Win10 出现实模式扇区条目 + `enum` 回显 GUID + D 盘文件齐；VM 单次进 Linux |
| 3 进展（2026-09-05） | ✅ `src/{common/process,boot/{bcd,grub,task},app/safety}` + CLI `restore` 暂存链路已通；实测 BCD 实模式条目（`device=partition=D: path=\grldr.mbr`）+ `bootsequence` + 任务文件齐全；exe 787KB；零警告 | — | 路上踩出 PIT-010（`CreateProcessW` 裸文件名 `FILE_NOT_FOUND`，必须 `GetSystemDirectoryW` 拼全路径）；验证后已清 `bootsequence` + 删任务文件（防意外重启进 Linux） |
| 4 CLI 全链路 | list/backup/restore/verify/images/diag/version + 确认提示 + 日志双写 + Mutex 单实例 + IShellLink | 0.5 周 | 备份→暂存→重启→Linux apply→回 Win 全链 |
| 4 进展（2026-09-06） | ✅ `src/common/{logger,progress,singleton}` + `src/app/shortcut` + CLI 接线已通；日志双写（`logs/SysRecover-*.log`）、`progress.json`、`shortcut` 建桌面lnk、备份覆盖确认（exit 2）、单实例互斥（持锁实测拒绝 exit 1）全验证；exe 1.15MB；零警告 | — | Linux 实机还原（重启）仍待真机/VM 验证 |
| 5 GUI | Duilib 分区选择/进度/镜像浏览，工作线程 + PostMessage 更新 UI（外观按 `docs/ui-design.md` + `skin/main.xml`，已定稿） | 1.5–2 周 | 功能对齐原型，无阻塞 UI；100%/150%/200% DPI 截图 |
| 5 进展（2026-09-16） | ✅ 经典 Duilib 窗口 + 完整 `skin/main.xml` + 桥接 CLI 逻辑；**本轮**新增：`src/app/ops.{h,cpp}` 共享层（RunBackup/StageRestore）供 CLI+GUI 调用；`WriteRestoreLog` 写目标分区根 `_zjresy*.log`（对齐 C# 契约）+ `image_path=` 新增进 `restore-task.conf`；`ReleaseOpLock` 支持 GUI 长驻进程；`bootfiles/restore.sh` 两处 bug 修复（image_path 回退 + 反斜杠路径转换）；GUI StartRestore 已改为暂存→确认重启流程；CLI CmdBackup/CmdRestore 重连 ops 层；暂存链路 dry-run + 清理已验证通过；**本轮修复**：①备份源改为选中分区盘符（不再硬编码 C:/）；②进度回调 100ms 节流（防消息队列洪泛）；③线程 detach→join（修复窗口关闭后僵尸线程风险）；SysRecoverUI.exe **1.98MB**；零警告 | — | 待补：⬜ PE 零依赖实测；⬜ 真机/VM 重启全链路验证 |
| 6 兼容收尾 | Win7/10/11/PE 矩阵 + 体积 < 10 MB + 文档更新 | 0.5–1 周 | 标准 PE 零依赖运行；BCD 用例只跑正常 OS |

**合计约 5–6 周**（单人全职）。每 Phase 独立可回滚；Phase 3 未过不进 Phase 4。

每个 Phase 必须五字段：目标 | 输入（前置 + 现成来源） | 输出（文件/PR/产物） | 验收（可执行命令 + 期望输出） | 回滚（`bcdedit /delete {GUID}` + 删 `D:\grldr.mbr/menu.lst/ZJRESTORE` 回 Windows）。

---

## 2. 已冻结结论（C# 原型验证，可直接复用，不重验）

- MBR 链路：`bootmgr → grldr.mbr → grldr → menu.lst(kernel/initrd) → restore.sh` ✅ 2026-09-05（Win10，MBR/BIOS）。
- `bcdedit /enum {GUID}` 判定法：存在回显 GUID（约 200B），不存在约 15B，两者退出码都 0。✅ 2026-09-05
- wimlib 热备：`--snapshot` 等价 VSS 快照。✅ 原型验证
- 进度/日志契约字段表。✅ 冻结

## 3. 待验证假设（C++ 新风险，需 Phase 闭环）

- H1：C++ 直接调 `wimlib_add_image` 的 `--snapshot` 热备与 tuned exe 等价？→ P2 验收。
- H2：~~Duilib 定库~~ ✅ 已定 `nim_duilib`（MIT，2026-09-05，MinGW 编译验证通过）；Win7/PE 下渲染正常待 P5 验收。
- H3：MinGW `-static` + 动态 libwim 后仍 < 10MB？→ P6 验收。

---

## 4. Linux 端架构（Alpine 底座，2026-09-17 全链验证通过）

### 架构定稿（2026-09-17 QEMU 全链验证通过）

使用 **Alpine linux-lts 6.6 底座**（自组装 initramfs + 自写 `/init`），**不用 BG-Rescue**（其内核无任何 SCSI HBA 驱动，VMware LSI/服务器 RAID 认不到盘，见 AGENTS.md PIT-043/044）。

**启动链路**（已验证）：
```
BIOS → MBR → bootmgr → BCD → grldr.mbr → grldr → menu.lst
→ kernel vmlinuz-zjrestore + initrd initramfs-zjrestore.cpio.gz
→ /init（自写 PID1：挂 proc/sys/dev → modprobe 全套存储驱动 → 扫描分区）
→ 找 _zjresy*.log（文件检测，不依赖卷标）+ restore-task.conf
→ zjrestore-lite.sh（格式化 + wimlib apply + PBR 引导修复）
→ reboot 回 Windows
```

**为什么换 Alpine**：BG-Rescue 9.0.0 的内核（5.15.12-64bit bg@rescue）`.config` 里 54 个 SCSI
HBA 驱动全部 `is not set`、被编成模块的驱动数 `=m` 为 **0**，initramfs 也不含任何 `.ko` →
在 VMware LSI Logic SCSI / 服务器 RAID 卡上**磁盘完全不可见**。Alpine 的 `linux-lts` 把
`mpt3sas`/`mptspi`/`mptsas`/`megaraid_sas`/`aacraid`/`hpsa`/`arcmsr`/`vmw_pvscsi`/`virtio_blk`/
`virtio_scsi`/`ahci`/`nvme`/`usb-storage` 等做成模块，按需 `modprobe`。

**组装方式（纯 Windows/Python，无需 Linux 环境）**：
`python tools/vmtest/build-alpine-initramfs.py` 从 `tools/vmtest/dl/alpine/*.apk`（Alpine 包是
tar.gz，可直接解包）组装：
- 内核：`linux-lts` 的 `boot/vmlinuz-lts` → `bootfiles/vmlinuz-zjrestore`
- 用户态：`busybox-static` + `ntfs-3g`/`ntfs-3g-progs`（含 `mkntfs`）+ `wimlib` + `util-linux blkid`
  （+ `libblkid`/`libuuid`/`libeconf`）+ `musl`
- 模块：裁剪后的 443 个 `.ko.gz`（`kernel/{drivers/{ata,scsi,message/fusion,nvme,virtio,block,md,usb,cdrom},block,fs/{ntfs3,fuse,fat,exfat,nls},crypto,lib}` + `modules.dep`）
- 自写 `bootfiles/alpine/init`（PID1）+ `bootfiles/zjrestore-lite.sh`
- 要点见 AGENTS.md PIT-044~048（权限位、apk 硬链接、busybox blkid 遮蔽、`/proc`/`/sys` 目录、PBR 修复）

### zjrestore-lite.sh 执行顺序（失败即停并写日志）

1. 读 `/tmp/_zjresy*.log` + `/tmp/restore-task.conf`（由 `/init` 扫描后拷贝；conf 缺失时回退日志字段）。
2. 解析四元素（offset/size/serial/path）+ 镜像路径/索引。
3. 目标分区定位：**用日志所在分区设备**（`/init` 作为 `$1` 传入）；回退按 offset 匹配（blkid 返回扇区，须 ×512）。
4. 扫描其它分区找镜像文件 + `bootfix.wim`（按 fs 类型选 `ntfs3`/`ntfs-3g`/`vfat`/`exfat` 挂载）。
5. `dd` 备份目标 PBR → `mkntfs -f --partition-start <真实起始LBA>` 格式化 → 写回微软引导代码段（内置 `ntfs-boot-code.bin`）+ 校验 BPB 隐藏扇区数（PIT-049/050）。
6. `wimlib-imagex apply "$IMG" "$IMAGE_INDEX" "$TARGET_DEV"`（**无 `--ntfs` 选项**，见 PIT-024）；进度重排为单行进度条。
7. **引导补全**：apply `bootfix.wim`，把 `\bootmgr` + `\Boot\BCD`（Windows 暂存阶段生成的可移植 BCD）写进目标分区（PIT-051）。
8. 写日志（`/tmp` + 尽量持久化到非目标分区根 `zjrestore-debug.log`）+ `sync` + `reboot -f`。

### 产品侧约束

- **不改卷标**，用 `_zjresy*.log` 文件检测定位目标分区。
- 恢复分区根放：`vmlinuz-zjrestore` + `initramfs-zjrestore.cpio.gz`（含 `/init` + 工具 + 模块）+ `zjrestore-lite.sh` + `_zjresy*.log` + `restore-task.conf`。
- `menu.lst`：`kernel /ZJRESTORE/boot/vmlinuz-zjrestore` + `initrd /ZJRESTORE/boot/initramfs-zjrestore.cpio.gz`。
- Windows C++ 代码部署：grldr + grldr.mbr + menu.lst + vmlinuz + initramfs + zjrestore-lite.sh → `D:\ZJRESTORE\`。

---

## 5. 日志规范（与 AGENTS.md §10 一致）

- 程序目录 `logs/restore-YYYYMMDD.log` 为主；PE 只读介质 fallback `%ProgramData%`。
- 目标分区 `_zjresy*.log`（引导层写入）。
- `progress.json` 字段：Phase/Percent/Status/Detail/UpdatedAt/Pid。

---

## 6. MBR/GPT 差异（统一写软件目录，细节见 AGENTS.md §5–§7）

- MBR/BIOS：BCD `bootsector` → `D:\grldr.mbr`（本计划主线，已验证）。
- GPT/UEFI：ESP 修复分支另立小节（沿用原型逻辑，Phase 3 覆盖）。
- **UEFI + Secure Boot（2026-09-19 三线对比，结论见 AGENTS.md PIT-060/062/063/065/066）**：
  - ✅ **默认线（已实测）**：`固件启动项 → shimx64.efi（微软签名）→ grubx64.efi（Canonical
    签名的 Ubuntu GRUB）→ grub.cfg（普通 `linux`/`initrd`）→ Canonical 签名的 Ubuntu 内核
    + 我们的 initramfs`。**零注册、零成本、不绕过**（每个可执行文件都有合法签名）。
    构建：`tools/build-ubuntu-rescue.py`。
  - 🅿️ **备选线（代码在仓库，默认不编译、不随包）**：`shim → 我们签名的 UKI`（需一次性
    MOK 注册）—— `src/boot/uefi.cpp` 的 `ZJ_ENABLE_MOK_PATH` 宏；资产 `bootfiles/sb/
    {shimx64.efi, mmx64.efi, fbx64.efi, zj-mok.cer}`、`tools/build-uki.py`、`keys/zj-mok.*`。
    **启用方式：把该宏改成 1、在 Makefile 的 `package` 里恢复 UKI 构建与资产拷贝即可。**
    保留原因：万一 Canonical/微软的签名链策略收紧，可快速切回。
  - ❌ **已证伪**：`bootapp + efiloader`（`nointegritychecks` 被 Secure Boot 策略保护，
    PIT-065）；`shim → 我们未签名的内核`（GRUB 强制 `shim_lock` 校验，PIT-066 变体 C）。
  - 体积：`dist` ≈ **53.6 MB**（内核 14.2 + initramfs 32.5 + GRUB/shim 3.6 + 程序）。

---

## 7. 文件清单（新项目）

- Windows 侧：`Makefile`（mingw32-make，无 CMake）、`src/{common,disk,wim,boot,app,cli}`、Phase 5 加 `src/gui/`、`third_party/wimlib/`（`wimlib.h` + `libwim-15.dll`，来自旧项目 `tools\ref\` 与官方 1.14.5 包）、`bootfiles/`（grldr/grldr.mbr/menu.lst 模板/vmlinuz+initramfs 引用）、`docs/ui-design.md` + `skin/main.xml`（外观已定稿）、`tools/pe-diag/`、`dist/`。
- Linux 侧：`vmlinuz-zjrestore`（Alpine linux-lts 6.6 内核）、`initramfs-zjrestore.cpio.gz`（自组装：busybox + ntfs-3g/mkntfs + wimlib + blkid + 443 个存储/文件系统模块 + 自写 `/init` + `zjrestore-lite.sh`）、`restore.sh`、`menu.lst` 模板、`grub.cfg`（UEFI）。
- 禁止项：`wimlib-imagex` 源码、grub4dos 源码、Dism++ 代码一律不入库。

---

## 8. 执行步骤（每 Phase 通用）

1. 开工先读 AGENTS.md §0 五条必查。
2. 按 Phase 表完成 输出物 + 验收命令。
3. 体积与依赖检查（`x86_64-w64-mingw32-objdump -p` 或 Dependencies 工具）。
4. PE/Win7 矩阵抽查（P1/P6 必跑，P3 只在正常 OS 跑 BCD）。
5. 更新本文 Phase 状态（`⬜→🚧→✅ + 日期/环境`）；超 90 天或工具链变更自动降为 `⏳待复验`。

---

## 9. 参考

- AGENTS.md（操作手册，§7 BCD SOP、§8 libwim、§13 坑位册为准）。
- wimlib C API：https://wimlib.net/apidoc/
- Microsoft Learn：`bcdedit`、`adding-boot-entries`、winioctl。
- GRUB4DOS 官方 release。

---

## 10. 产品边界与待决事项（2026-09-19）

- **产品关系**：本计划书只覆盖 **SysRecover 单机版**（面向电脑维修/系统维护/IT 管理员）。
  早期的 `WooMonlee/OnekeyRestore`（C# + 原生 VHD 引导 + VHDX 差分链、多点秒还原、面向学校机房）
  是**另一个产品**，分开推进；两者的顶层分层思想同源，但「还原机制」根本不同，文档与代码不混用。
- **待决：本产品自身代码是否开源**。合规上无阻碍（见 AGENTS.md §14：未静态链接 GPL、未修改
  第三方源码 → 不受 copyleft 约束）；开源与否属商业决定。
- **待决：Windows 7 支持策略**。当前 exe 与 `libwim-15.dll` 依赖 UCRT（Win7 无）；
  要零安装支持 Win7 需把 Windows SDK 的 `Redist\ucrt\DLLs\x64\`（约 1.5MB）随包带上。
- **待补：服务器 RAID 驱动子集**（`vmd`/`megaraid`/老 `mpt*`/`isci` 等，在 Ubuntu
  `linux-modules-extra` 里），补入后可覆盖服务器/阵列卡场景。
- **32 位支持：已定方案（2026-09-24 用户拍板，`0.3`）**。目标机器 **CPU/主板都是 64 位**，只是系统可能是
  Win7 x64 / **Win7 x86**；**不考虑真正的 32 位 CPU 老机器（已过时）** → **救援层（x86_64）不动**，
  只把 **Windows 侧 exe 出成 x86**（32/64 位 Windows 通吃）。详见 `docs/08-32位支持评估（待实施）.md` §0。
  **代价**：备份压缩慢（`fast`≈0~10%，`maximum`≈10~20%，`recovery`≈20~35%），**还原 0%**；其余无损失。
  **想零损失 → 方案 C**（x86+x64 两套 + 启动器，+4MB）。前置条件：i686 工具链 + 官方 32 位 `libwim-15.dll`
  + Makefile `ARCH=x86` + **x86 版 UCRT**（Win7 无 UCRT）。Win7 x86 只能 BIOS/MBR → 走已验证的 GRUB4DOS 路径。
- **待补：服务器 RAID 真机验收**（驱动已入包 26 个、QEMU 加载通过 ✓，缺真机阵列卡场景）。
- **主体工作已完成**：BIOS/MBR、UEFI/GPT、UEFI+Secure Boot（零注册）、Win7 宿主（含跨版本还原）
  均已实测通过；后续以**兼容性测试与打磨**为主。
- **待实测：忙时关闭「终止并退出」**（2026-09-21，`0.1.4`，PIT-071）—— 以前任务执行中点右上角
  叉叉**完全没有反应**；现在应立刻弹选择框（默认「继续等待」），选「终止并退出」应 1 秒内退出，
  且只删未写完的 `<目标>.tmp`（原有的同名镜像不受影响）。验收要点见 `docs/11` §3 / `docs/07` §2。
- **待决：Secure Boot 双签 shim**（`Microsoft Corporation UEFI CA 2011` 已于 2026-06-26/27 到期）。
  现状与备选 B/C 的完整核实记录见 **§11**。

---

## 11. Secure Boot 证书过渡（CA2011 → CA2023）：现状与备选（2026-09-20 记录）

**现状 A（保持不动，无需动作）**：我们的 Secure Boot 链为
`固件 → shimx64.efi（微软签）→ grubx64.efi（Canonical 签）→ vmlinuz（Canonical 签）+ 我们的 initramfs`。
实测（直接解析 PE 证书表）：`bootfiles/sb/shimx64.efi` **只有一条签名** —— 签发者
`Microsoft Windows UEFI Driver Publisher`、颁证者 `Microsoft Corporation UEFI CA 2011`，
全文件搜不到任何 CA2023 字样 → **CA2011 单签**（链上其余文件均为 Canonical 签名，与微软 CA 无关）。

**背景**：CA2011 已于 **2026-06-26/27 到期**，但**到期不影响启动** —— 固件按 `db`/`dbx` 成员判定信任，
**不检查有效期**，故信任 CA2011 的固件上一切照旧。真正的风险只有两个：
1. **2026 年新出厂、固件 `db` 仅含 CA2023** 的机器 → 我们的 CA2011 单签 shim **起不来**；
2. 微软自 2026-06 起只能用 CA2023 签新 shim → 未来的 shim 安全修复无法覆盖只信任 CA2011 的固件。

**已核实的可下载情况**（国内源，均**不需要梯子**）：

| 发行版 | 版本 | 签名 | 来源 |
|---|---|---|---|
| Ubuntu | `shim-signed 1.59+15.8-0ubuntu2` | **CA2011 单签** | 清华/中科大/官方 archive.ubuntu.com 三处一致 —— Canonical 尚未发布双签版 |
| Fedora 43 | `shim-x64-15.8-3` | **CA2011 单签** | 清华 |
| **AlmaLinux 10**（RHEL 10 重建）| `shim-x64 16.1-4` | **CA2011 + CA2023 双签** ✓ | 阿里云 `almalinux/10/BaseOS/x86_64/os/Packages/` |

**关键约束**：shim 只信任**它自己发行版**内嵌的证书（Alma 的 shim 里是 AlmaLinux 证书，不含 Canonical 的）
→ **不能只替换 shim 这一个文件**，必须把 GRUB 与内核一并换成同一发行版的签名件。

### 备选 B：整链换 AlmaLinux（新老固件通吃）
- **内容**：`shimx64.efi` + `grubx64.efi` + `vmlinuz` + 内核模块 `.ko` 全部改用 AlmaLinux 的签名件；
  **initramfs 仍是我们自己的**（UEFI 规则：initrd 从不校验）。
- **好处**：仅信任 CA2011 的老固件 + 只信 CA2023 的新固件 **都能启动**。
- **代价**：① `tools/build-ubuntu-rescue.py` 需按 Alma 的内核模块打包方式再适配一次（量级≈当初
  Alpine→Ubuntu 那次，数小时）；② 链上文件来源变为 Alma，SBOM（AGENTS.md §14）需同步更新。
- **触发条件**：把「2026 新硬件（仅 CA2023 固件）兼容」列为硬指标时启动。
- **资源**：`https://mirrors.aliyun.com/almalinux/10/BaseOS/x86_64/os/Packages/`（`shim-x64-16.1-4` 已实测双签 ✓）。

### 备选 C：等 Canonical 发布双签 shim（最小改动、零代码）
- 一旦 `shim-signed` 出现**双签**版本（同时含 CA2011 + CA2023），只需替换 `bootfiles/sb/shimx64.efi`
  一个文件，**代码零改动**（`ZJ_SB_MODE=grub` 链不变）。
- **风险**：Canonical 公文称 2026 Q4 起更新将要求 CA2023；若他们只发 **2023-only** 版，
  会**打死仅信任 CA2011 的老固件** → 必须确认是**双签**才能采用。
- **验收方法（已有）**：解析 PE 证书表，期望两条签名（`#1 → CA2011`、`#2 → Microsoft UEFI CA 2023`）。
- **监测点**：`pool/main/s/shim-signed/`（国内源与官方同步），出现新版本即验证。

> 未采纳的加固思路 **D**：安装时读固件 `db`（`GetFirmwareEnvironmentVariable("db")` + 解析
> `EFI_SIGNATURE_LIST`）探测其信任哪张 CA，按结果决定部署哪条链 —— 产品上最稳，但有额外开发量。

> **测试环境提醒**：QEMU/VMware 虚拟机的 NVRAM 在**创建时**快照 → 老 VM 的 `db` 只含 CA2011，
> 「新硬件只信 CA2023」的失败场景**在现有 VM 上复现不出来**；要复现需新建 VM / 用新版 OVMF 固件模板，
> 或手动把 CA2023 灌进 VM 的 `db`。

### §11.1 换链前的核实（2026-09-23 实测）

**AlmaLinux 10**（原备选 B 的候选）：
- `shim-x64 16.1-4`：**CA2011 + CA2023 双签** ✓（实测含 `Microsoft UEFI CA 2023 signer`）
- `grubx64.efi`（4.2MB）/ `vmlinuz`（6.12.0-211.56.1，15.9MB）：均为 **AlmaLinux 签名** ✓ → 链自洽 ✓
- ⚠️ **但驱动覆盖有风险** ✗：Alma 的 `kernel-modules-extra` 只有 **3.1MB**（Ubuntu 的 `linux-modules-extra` 是 **113MB**）；
  实扫三个包（`kernel-modules-core` 1250 个/27.8MB + `kernel-modules` 1009 个/39.9MB + `extra` 154 个/1.4MB）后，
  **`vmd` / `isci` / `arcmsr` / `pm80xx` / `mvsas` 找不到模块文件** ✗（内建？还是被上游内核移除？待定）；
  **`ntfs3` 也没有** ✗（但我们随包带 `ntfs-3g`(FUSE)，NTFS 照样能挂 ✓）
- 内核 6.12 的 vmlinuz **没有内嵌 `.config`** ✗（PIT-044 那招在 RHEL 系内核上不适用）

**Debian**（新发现，**可能是更优选择** ✓✓）：
- `shim-signed 1.51+16.1-2` 的 `shimx64.efi.signed`（1063KB）：**也是双签** ✓✓
  （CA2011 ×3 + `Microsoft UEFI CA 2023` ×4 + `Microsoft UEFI CA 2023 signer` ×1，内嵌 **Debian** 证书）
- Debian 与 Ubuntu **同源** → 内核模块覆盖**应当同样广** ✓（Alma/RHEL 明显更"克制" ✗）
- → **备选 B 的发行版候选：Debian 优于 AlmaLinux**（待验覆盖后定）

**结论 / 下一步**：换链前先做一次「**候选发行版驱动覆盖对比**」——
各下 1 个内核/模块包、数模块并核对关键 HBA 名单（Ubuntu 现状 / Debian / Alma 三家），**再决定换谁**；
选定后再动代码（构建脚本 + SB 资产 + 回归测试）。

**✅ 三家对比结论（2026-09-23 实测完成）**：

| | 现在的 Ubuntu 链 | **Debian 13 (trixie)** ⭐**推荐** | AlmaLinux 10 |
|---|---|---|---|
| shim 双签（CA2011+CA2023）| ❌ 单签 | ✅ **双签**（`shim-signed 1.51+16.1-2` 实测）| ✅ 双签（`shim-x64 16.1-4`）|
| 内核 | 6.8 / **14.2MB** | **6.12.107 / 11.6MB**（最小）| 6.12.0 / 15.9MB |
| 模块总量 | —（我们只取存储闭包 34MB）| **4225 个 / 89.3MB** | 2413 个 / 69.1MB |
| **关键驱动 31 项**（`vmd`/`isci`/`arcmsr`/`pm80xx`/`mvsas`/`megaraid_sas`/`mpt3sas`/`mptspi`/`mptsas`/`hpsa`/`aacraid`/`smartpqi`/`virtio_*`/`nvme`/`ahci`/`ata_piix`/`usb-storage`/`uas`/`sd_mod`/`vfat`/`exfat`/`dm-*`/`raid0-10`/`qla2xxx`/`lpfc`）| 现状可用 ✓ | **31/31 ✓✓** | **缺 5 项** ✗（vmd/isci/arcmsr/pm80xx/mvsas）|
| `ntfs3` | ✗（靠随包 `ntfs-3g` 兜底）| ✅ 有 | ✗ |

→ **决定：备选 B 的发行版选 Debian** ✓（双签 ✓ + 驱动最全 ✓ + 内核最小 → 我们只取存储闭包，
**体积估计与现状持平甚至略降**）。AlmaLinux 因驱动覆盖偏窄而**不采用** ✗。

**下一步（实施 B）**：① 写 `tools/build-debian-rescue.py`（复用现有骨架，换内核/模块来源；
Debian 的模块也是 `.ko.xz`）② 先验"能组装出可启动 initramfs"（QEMU 认盘）③ 再验 Secure Boot 链
（QEMU+OVMF+SB 开）④ 最后替换 `bootfiles/`（shim/GRUB/内核/initramfs）+ 更新 SBOM/文档 + 回归。

**✅ 实施结果（2026-09-23 完成）**：
- ① `tools/build-debian-rescue.py` ✓（路径白名单 + 依赖闭包；**2026-09-24 加 EXCLUDE 裁剪后 494 模块**，initramfs 20.5MB + 内核 11.6MB；见 PIT-079）
- ② QEMU 启动 ✓（`Linux 6.12.107+deb13-amd64` → `SR: block devs: sda sda1 sr0` 认盘成功）
- ③ SB 资产已换 Debian ✓（shim 双签 + Debian 签名 GRUB）；QEMU 链验证 shim→GRUB→内核→我们的 initramfs ✓；
      **用户 VMware（Secure Boot 开）实测还原成功** ✓（注意：其固件同时信任 CA2011，故"CA2023-only 新硬件"场景仍需将来在新固件上验证 🚧）
- ④ SBOM/文档已同步 ✓；`dist` **38.5MB**（2026-09-24 模块裁剪后，原 54.3MB）；`ZJ_SB_MODE` 默认 `grub` 不变
- 遗留：`libcrc32c` 加载告警（softdep 未覆盖，与本产品路径无关，待补）；`bootfiles/sb/` 的 mmx64/fbx64
  仅留在仓库（不再随包/不再部署）

---

## 12. 开发待办（已与用户确认，待排期）

> 2026-09-21 记录：项目已基本完工，以下是"继续开发"的候选清单（用户稍后挑）。
> 约定：**每完成一项先 `python tools/version.py --bump` 再提交**（见「版本号规则」）。

| # | 事项 | 价值 | 预估 |
|---|---|---|---|
| 1 | CLI `backup --name` 默认改用系统描述（与 GUI 一致，复用 `src/common/sysinfo`）| 一致性 | 分钟级 |
| 2 | `--append` 追加模式的**中断保护**（现为就地写，中断可能留下部分更新的文件）| 可靠性 | 1~2 小时 |
| 3 | ✅ **`make check` + 单元测试**（2026-09-24 完成）：`tools/version.py`、`sysinfo`（注册表值→描述组合/中文映射）、`exclude`（含 PIT-056 回归守卫）、`task` 契约文本、`zip` CRC32 | **回归安全**（完工后最划算的投入）| ✅ 已完成 |
| 4 | **Secure Boot 备选 B：整链换 AlmaLinux**（见 §11）→ 解决 2026 新硬件兼容 | 兼容性 | 数小时 |
| 5 | **x86 版 Windows 侧（`0.3`）**：i686 工具链 + 官方 32 位 `libwim-15.dll` + `Makefile ARCH=x86` + x86 UCRT + WOW64 复核 | **Win7/Win10 x86 覆盖面** | 1~2 天 |
| 6 | 发布说明 / 下载页文案（明确"仅 64 位"等口径，避免用户误解）| 交付 | 1 小时 |

**已决定不做（2026-09-21 用户拍板，留记录）**：

| 事项 | 结论 |
|---|---|
| 可启动救援 ISO / U 盘 | ❌ 不做 —— 不如"软件自动重启还原"方便（我们的定位就是免介质）|
| 网络还原 / 组播部署 | ❌ 暂不做 —— 以后做**机房管理**那条产品线时再说 |
| 镜像"浏览" | ⚠️ 修正认知：**子镜像浏览/选择本来就有**（GUI 下拉 `Index - Name`、`images` 命令、`restore --index N`）；缺的只是**文件级提取**（在镜像里看/取单个文件），见下表 |

**候选（已核实 → 见 §13 的实施计划）**：原"待挑候选"已逐条核实并排序，见 **§13**。

---

## 13. 实施计划（2026-09-21 核实后定稿，逐项实现）

> 原则：**先核实再动手**（下表"核实"列是读代码确认的结论，不是猜的）。
> 每完成一项：`python tools/version.py --bump` → 提交 → 推送。
> 实施顺序见文末。

### 13.1 要做（按实施顺序）

| # | 事项 | 核实结论 | 做法 | 工作量 |
|---|---|---|---|---|
| **P1** | **还原前空间预检** | ✅ **真问题**：就地还原与救援层都是**先格式化再 apply**，全工程**无任何**目标空间检查（只有 ESP 空间检查，是另一回事）→ 空间不够会"数据没了、系统也没装上" | ✅ **已完成（`0.1.6`）**：新增 `WimEngine::ImageSize()`（解析 WIM 元数据 XML，坑见 AGENTS **PIT-073**：UTF-16LE + 必须减 HARDLINKBYTES）+ `CheckRestoreSpace()` 插在**安全门禁之后、格式化之前**（暂存/就地两条路都覆盖）；估算法 = 内容量 ×1.10 + 300MB | 半天 |
| **P3** | **一键导出诊断包** | ✅ 现在没有任何打包能力，排错一直靠人工收集 | ✅ **已完成（`0.1.9`）**：新增 `src/common/zip.{h,cpp}`（**自写最小 ZIP**：store 模式、无压缩、零依赖 —— 不引 zlib、不依赖 PS5 的 `Compress-Archive`、不假设有 7-Zip）+ `diag --zip [--out x.zip]`，打包 diag 文本 + `logs/` 全部文件 + `restore-task.conf/.json` + `version.json`。**实测**：用 Windows 自带 `Expand-Archive` 解压验证通过（20 个文件全在 ✓，格式合规 ✓）。⚠️ GUI 按钮待补（皮肤是逐像素校准的，另开一次小改动更稳）| 半天 |
| **P4** | **速度 / ETA** | ⚠️ **修正**：CLI 也**没有**（只有已用时间）→ 两边都要加 | 进度回调里已有 `completed_bytes/total_bytes` → 算 MB/s 与剩余时间；CLI 一行刷新 + GUI 状态栏 | 2~3h |
| **P2** | **BitLocker：从"拒绝"到"能办"（v1）** | ✅ 现状：`safety.cpp` 用 `manage-bde -status` 检测到加密就**直接拒绝** | v1 = **带指引的确认**：说明"目标盘启用了 BitLocker，需先挂起保护"，给「帮我挂起并继续」→ 执行 `manage-bde -protectors -disable <L>: -rebootcount 1`；并提示"若中止还原，记得 `-protectors -enable <L>:` 恢复保护"。**v2**（以后）才做完整自动化（含恢复密钥） | 半天 |
| **P5** | **SB：一键重启进固件设置** | ✅ 工程内**没有** `/fw` | `shutdown /r /fw /t 0`（Win10+/UEFI）→ 放在 SB 相关提示、引导安装失败处 | 1h |
| **P6** | **SB：读固件 `db` 判断该用哪条链** | ✅ 可行：`uefi.cpp` 已有 `GetFirmwareEnvironmentVariableW` + 提权；`db` 里证书 CN 是**明文字符串** → 字符串扫描即可 | 安装/暂存前探测 → 显示"本机固件信任：CA2011 / CA2023 / 两者"；若**只信 CA2023** 而我们的链是 CA2011 → **提前明确提示**（别让用户重启后才懵）。与 §11 的 B/C 配合 | 半天 |
| **P9** | **文件级提取** | ⚠️ **修正**：子镜像浏览/选择**已有**（GUI 下拉、`images`、`restore --index N`）；缺的只是**文件级** | ✅ **已完成（`0.1.8`）**：`WimEngine::ExtractPaths()`（`wimlib_extract_paths`，支持通配符）+ CLI `extract --file <镜像> [--index N] --path "\Windows\..." [--path ...] --dest <目录>`。**实测通过**：从测试镜像取出 `\Windows\win.ini` 与 `\Windows\System32\drivers\etc\hosts`，目录层级保留 ✓。GUI 侧 **2026-09-26 用户裁定不做**（见 §13.4） | 半天 |
| **P10** | **镜像信息展示** | ✅ `ImageDesc` 目前只有 `{index,name}` | ✅ **已完成（`0.1.7`）**：`images --file` 现在输出 `序号 \| 名称 \| 实际占用大小 \| 创建日期` + 描述行；GUI 下拉显示 `Index - Name（大小）`。⚠️ 小尾巴：**创建日期还没解析出来**（显示 `-`，`CREATIONTIME` 的 HIGHPART/LOWPART 待查），不影响使用 | 2~3h |
| **P11** | **CLI 中文路径**（做 P10 时发现）| ✅ 真 bug：CLI 的参数来自控制台 argv（**ANSI**），`ToWide` 按 UTF-8 解 → **中文路径被打乱**，`images --file 中文.esd` 直接 "Failed to open a file" ✗（GUI 内部走宽串，不受影响）| ✅ **已完成（`0.1.11`）**：改用 `GetCommandLineW` + `CommandLineToArgvW` 取宽字符参数再转 UTF-8（整条链路自洽）；顺带 `SetConsoleOutputCP(CP_UTF8)`（退出时还原）→ 中文提示不再乱码。**实测**：中文路径的镜像能正常读取 ✓、中文帮助文本可读 ✓ | 1~2h |
| **P7** | **备份/还原历史记录** | ✅ 无 | 追加写 `logs/history.jsonl`（时间/类型/镜像/目标/结果/耗时/版本）+ CLI `history` 读取；GUI 列表以后 | 2~3h |
| **P8** | **失败提示给"下一步"** | ✅ 现在只给错误文本 | 把常见错误（rc=4/5/84/88、空间不足、BitLocker、未装引导）映射成**可操作建议**（含"跨硬件还原建议先 sysprep"）→ CLI + GUI 弹窗正文 | 2~3h |

### 13.2 调整或不做（记录下来，避免以后重复讨论）

| 事项 | 结论 | 原因 |
|---|---|---|
| 可启动救援 ISO / U 盘 | ❌ **不做** | 用户：不如"软件自动重启还原"方便（定位就是免介质）|
| 网络还原 / 组播 | ❌ **暂不做** | 留给以后**机房管理**那条产品线 |
| **ESP 纳入镜像** | ⚠️ **调整：不做** | 核实后现状**更干净**：UEFI 在暂存阶段用 `bcdboot` 补 ESP（已实测通过），比"原样搬源机 ESP"更可靠（源机 ESP 可能带厂商残留/多余条目）|
| 速度/ETA | ⚠️ **修正后要做** | 原以为"CLI 已有"→ 核实：**CLI 也没有**（见 P4）|
| 镜像"浏览" | ⚠️ **认知修正** | 子镜像浏览/选择**本来就有**；只有**文件级提取**算缺（P9）|
| 跨硬件还原：驱动注入 | ❌ **暂不做** | 工程量大；先做"建议先 sysprep"的**提示**（并入 P8）|
| 定时备份 / 企业 MOK | ⏸️ **留作将来** | §12 保留条目 |
| 兼容性矩阵 | ✅ **转测试**（不是开发）| 并入 `docs/07` 待测清单：Hyper-V / VirtualBox / Win8.1 / Server 版 / 4Kn 扇区 |

### 13.3 实施顺序（2026-09-21 重排：按"代码相关性 + 价值/风险"分组，减少来回切换）

1. ✅ **P1** 空间预检（安全；已完成 `0.1.6`）
2. **P10 镜像信息展示**（顺手把 P1 的 wim API 暴露出来 → **P1 的数值可被验证**；改动小）
3. **P9 文件级提取**（与 P10 同一套 wim API，一起做省一次上下文）—— ✅ CLI 侧已完成（`0.1.8`）；**GUI 侧 2026-09-26 用户裁定不做**（见 §13.4）
4. **P3 诊断包**（排错效率，独立）
5. **P4 速度/ETA**（体验，便宜）
6. **P2 BitLocker**（兼容，需谨慎）
7. **P8 失败提示给"下一步"**（紧跟 P2：把 BitLocker/空间/坏镜像等错误都配上可操作建议）
8. **P5 SB 一键重启进固件设置** + **P6 SB 读 `db` 判断该用哪条链**（一组）
9. **P7 备份/还原历史记录**（收尾）

> 每项完成后：`python tools/version.py --bump` → 提交 → 推送；并在本表把该项标 ✅ + 版本号。

**进度（2026-09-23 自主推进）**：
`P1` ✅`0.1.6` → `P10` ✅`0.1.7` → `P9` ✅`0.1.8` → `P3` ✅`0.1.9` → `P4` ✅`0.1.10` →
`P11` ✅`0.1.11`（做 P10 时发现的中文路径 bug）→ `P8` ✅`0.1.12` → `P6` ✅`0.1.13` → `P7` ✅`0.1.14`。
**仍未做**：`P2` 的"帮我挂起 BitLocker"按钮（**建议文案已并入 P8** ✓，自动挂起涉及安全状态 + 界面改动 → 留待用户在场时定）；`P5` 一键重启进固件设置（纯界面按钮 → 同上）；`P3` 的 GUI 导出按钮（皮肤逐像素校准，单独小改）。

**用户拍板（2026-09-23，代替原 P2 / P5 / P3-GUI）**：

- **P2 改为「全系统 BitLocker 扫描 + 提醒」** ✅ `0.1.15`：只要系统里**存在** BitLocker 加密卷就提醒
  「没有密码/恢复密钥则还原后数据无法恢复」，用户选**继续/退出**（默认**退出**，安全项）；**不再硬拒绝**，
  也**不做**自动挂起按钮。实现：`safety.cpp::BitLockerVolumes()`（全盘符扫描）+ GUI 确认框 + CLI 警告行。
- **P5 砍掉** ❌：一键重启进固件设置 —— 用户判断"会的人不需要，不会的人进了也一脸懵逼"，
  改为**等我们自己兼容 CA2023**（§11 的备选 B/C）。
- **P3 的 GUI 按钮改为「讨论」链接** ✅ `0.1.15`：界面**右下角**加一个透明底的「讨论」链接 →
  初期指向无忧论坛帖子 `https://bbs.wuyou.net/forum.php?mod=viewthread&tid=453597`；
  成熟后换成自家站点 + **网站收日志的报错上报**。URL 只在 `src/gui/main_form.cpp` 的 `kSiteUrl` 一处。

### 13.4 第二轮外部评审收拢（2026-09-26，多角色专家组 → 已实施 P1~P7，`0.3.13`）

> 8 个角色（安装部署 / 运维 / 测试 / 安全 / 兼容 / UX / 代码 / 文档）独立评审后**交叉印证**，
> 收拢成 7 项（按"确认度 + 价值/成本"排序）。技术细节见 AGENTS **PIT-085**。

| # | 事项 | 做法 | 验证 |
|---|---|---|---|
| P1 | **网络镜像还原防呆** | 还原系统盘要重启进救援层，而救援层**没有网络** → 镜像在 UNC/映射盘上必然失败（且可能已格式化目标）→ `ops.cpp::IsNetworkPath()` 在**暂存前**拒绝（明确提示"先复制到本地分区"）| ✅ 实测：`\\localhost\D$\...test.wim` → `staged restore rejected: image on network path`，**未暂存** |
| P2 | **契约版本握手** | `restore-task.conf` / `_zjresy*.log` 补写 `contract_version`；`zjrestore-lite.sh` 用 `ZJ_CONTRACT` 核对，**不匹配立即退出、不碰目标分区** | ✅ 向后兼容（缺键视为 1）+ 不匹配（conf=2）均实测 |
| P3 | **CLI 自描述** | `<命令> --help` / `help <命令>` / 无参总览 + 退出码含义 | ✅ 实测 |
| P4 | **`dist/README.txt`** | 用户向使用说明（UTF-8 BOM + CRLF，双击记事本可读），`make package` 拷进 dist | ✅ 已在 dist |
| P5 | **日志自动轮转** | `logger.cpp::PruneLogs()`：`SysRecover-*.log` 留 14 天、`crash-*` 留最近 30 个 | ✅ 实测（60/30 天前日志被清） |
| P6 | **应用/窗口图标** | `tools/make-icon.py` → `resources/SysRecover.ico`（6 尺寸）；**仅 GUI** exe 嵌 `1 ICON` + `WM_SETICON`，CLI 不带图标（2026-09-26 用户裁定，见 §13.4 P6） | ✅ 实测（从 GUI exe 提取到 32×32 图标；CLI 应无 RT_GROUP_ICON） |
| P7 | **`make smoke`** | 一键跑 bios / uefi / uefi-ubuntu 三条 QEMU 冒烟（**不进 `make check`**，耗时数分钟） | — |

**顺带修**：CLI 退出码映射让 `2=参数错` 透传（原 `return rc==4?4:(rc==5?5:1)`）。

**用户裁定（2026-09-26）**：

| 事项 | 结论 |
|---|---|
| 备份默认格式 `.esd` → `.wim` | ❌ **不做** —— 用户群体对"备份快"比"体积小"更敏感；改默认值影响老习惯 |
| 安装包 / 卸载程序 | ❌ **不做** —— 维持绿色解压即用 |
| 自动更新检查 | ❌ **不做** —— 不引入服务器 / 更新源 |
| 高 DPI 清单（`dpiAware`）| ⏸️ 仍搁置 —— 会把界面从 DPI 虚拟化切到真实 DPI，皮肤逐像素校准需单独回归 |

**待讨论后定（2026-09-26 用户："讨论后再做"）**：

| 事项 | 现状 / 费用与前提 |
|---|---|
| **代码签名**（消 SmartScreen"未知发布者"）| **要花钱**（2026-08 微软官方口径）：Azure Artifact Signing（原 Trusted Signing）**$9.99/月**（5000 次签名），但**个人开发者仅限美国/加拿大**（中国个人不可用，只能组织身份或传统证书）；传统 **OV** ≈¥1000~3000/年（2023 起强制硬件令牌/云 HSM）、**EV** ≈$300~700/年（**立刻**获得 SmartScreen 信誉）。**免费的都达不到目的**：自签名免费但照样报"未知发布者"（用户还得手动装证书）；**SignPath Foundation 免费但要求项目开源**（本项目开源与否仍是待决事项）。签名只影响观感、不影响还原功能 |

**已裁定（2026-09-26 下午，用户）**：

| 事项 | 结论 |
|---|---|
| **坏盘 / 文件系统损坏检测** | ✅ **已实施** `0.3.15`（PIT-086）：`QueryDiskHealth()` 双路读健康 —— **ATA/SATA** 走 `IOCTL_ATA_PASS_THROUGH` 的 SMART READ DATA + RETURN STATUS（属性 5/197/198 + 驱动器自报故障），**NVMe** 走 `IOCTL_STORAGE_QUERY_PROPERTY` 的 SMART/Health 日志页（Critical Warning / Media Errors，协议数据必须放 `query->AdditionalParameters`=**偏移 8**，否则 `ERROR_INVALID_PARAMETER(87)`）。展示三处：GUI `AskDiskHealthWarning` 弹框（**默认取消**=安全项，静默模式跳过）、CLI `restore` 前打印、`diag` 每盘一行（`smart=ata/nvme/unavailable`）。取不到 SMART（USB 桥/RAID/Win7 无 NVMe 属性）→ **fail-open 放行**。实测：SATA 盘 `smart=ata realloc=0 pending=0 uncorrect=0`、NVMe 盘 `smart=nvme crit=0x00 media=0 used=1% temp=46C` |
| **ReFS 提示** | 📄 **只写文档**（用户 2026-09-26："无法操作需写入我们的介绍文档中，就像我们不支持 Win7 x86 以下的操作系统一样"）—— 进介绍文档的支持矩阵/限制章节，**不加运行时拦截**（运行时守卫另议、可选实现） |
| **GUI 文件级提取** | ❌ **不做**（用户 2026-09-26："界面太复杂、功能太多后对初学者不友好"）—— **CLI `extract` 保留**（P9，`0.1.8` 已实测，供懂命令行的人/脚本用），GUI 不加对应界面与入口（调研与方案留档：入口三方案 vs 浏览两形态的对比见 `docs/13` §4 与本表历史版本） |

**评审中"不成立 / 低价值"（留档，避免重复讨论）**：CLI 退出码语义"混乱"（既有行为，仅按需加固）、
若干文档措辞/格式项（待文档统一整理时一并处理）。

---

## 14. i18n 全球化（多语言）实施计划（2026-09-27 用户批准："全部按推荐值操作"）

> 目标：**界面与 CLI 跟随系统语言**（英文系统显示英文、中文系统显示中文），
> 并按"**可适用于全世界使用的产品**"做地基改造（不只是翻译）。用户 2026-09-27 批准全部推荐值并要求
> **GUI + CLI 一起上**。本节是唯一计划来源；执行纪律见 AGENTS §17。

### 14.1 锁定决策（用户 2026-09-27 批准，全部按推荐值）

| 项 | 决定 | 理由 |
|---|---|---|
| 范围 | **GUI + CLI 一起上**（不分先后） | 用户："还有可能给其他国家的用户使用" |
| 首发语种 | **en、zh-CN**（人工校对的权威版）+ **ja/ko/de/fr/es/ru**（机翻基线，标注 community） | 主市场保质量，其余先铺开 |
| 载体 | **外置 `lang/*.lang`（`key=value`，UTF-8）**，随包放 dist；**不进 exe、不用 JSON** | 社区可补翻、免重编译；仓库零依赖、无 JSON reader |
| 语言检测 | `GetUserDefaultUILanguage() & 0x3FF == 0x04`（zh* → 中文，其余英文）；`--lang` / `SYSRECOVER_LANG` 覆盖 | Win7/PE 均有效；跟系统为默认、可手动覆盖 |
| 回退链 | 缺 key → en → 显示 key 本身 + 记 warn | 永不因漏翻而崩 |
| RTL（阿拉伯/希伯来） | **声明不支持** | Duilib 无镜像，+2~3 天且要动布局引擎；对齐 Dism++/Rufus 通行做法 |
| 测试策略 | **zh/en 像素级基线**；其余语言 = **文字宽度门禁 + 抽测截图** | 免测试矩阵线性爆炸 |
| **不翻译** | 日志 / 诊断包 / 契约文件（`restore-task.conf`、`_zjresy*.log`、`progress.json`）/ 救援层屏幕（内核无 CJK 字形，PIT-061）/ 品牌名（九转还原、SysRecover） | 机器接口 + 字体硬限制；改契约字段要双端发版（红线①） |
| 翻译质量 | en/zh 人工终校；其余机翻 + 标注 community | 成本可控 |
| 版本 | 建议 **0.4.0**（重大特性，次版本号由用户指定） | SemVer 规则 |

### 14.2 体积测算（2026-09-27 实测 + 推算）

**现状实测**：`dist` = **47.00 MB / 61 文件** —— bootfiles **35.06 MB（75%，i18n 完全无关）**、
根目录 x86 整套 ~6.2 MB、`x64/` 5.70 MB、skin/README 等 ~0.02 MB。
**待翻译文本实测**（账本工具 `tools/extract-strings.py`，2026-09-27；早期"按行统计"的 420 条 / 16.1 KB
把注释也算进去了，**作废**）：C++ 字面量 **348 条** + 皮肤 XML **33 条** = **381 条 / UTF-8 13.5 KB /
平均 36 字节**；另 4 条纯日志、1341 条注释中文按约定不翻；中文出现在字符串/注释之外（raw）**0 处**。

| 项 | en+zh | **8 语种** | 依据 |
|---|---|---|---|
| `lang/*.lang` | ~50 KB | **~210 KB** | 381 条 ×（key ~28B + 值 ~36B + CRLF）≈ 25 KB/语种 ×8 |
| 4 个 exe 净变化 | ~+110 KB | ~+110 KB | 中文串移出（−~20 KB/对）+ 内置**英文兜底表**（+35 KB）+ 加载器（+5 KB）×4 |
| README | +2 KB | +2 KB | 其余语言社区补 |
| 字体 / 救援层 / 契约 / 日志 | 0 | 0 | 用系统自带字体（不内嵌）；机器接口不翻译 |
| **合计** | ≈ +0.16 MB | **≈ +0.31 MB（最坏 0.42 MB）** | 相对 47 MB = **+0.66%（<1%）** |
| exe 体积门禁 | — | 零压力 | GUI 2.66→~2.69 MB，CLI 1.61→~1.63 MB（门禁 <10 MB） |
| 斜率 | — | **每增一语言 ≈ +28 KB** | 20 语种也才 +0.56 MB |

**结论：体积不是约束**（75% 的体积是内核/initramfs），真正的成本在 M1（解耦）与 M3（抗文本扩容）。

### 14.3 分期（M1 → M4，合计 **11~14 人日**；只做 en/zh = 到 M2 为止 ≈ 6~8 人日）

| 里程碑 | 内容 | 验收 | 估算 |
|---|---|---|---|
| **M1 前置解耦**（必须先做，防返工） | ① **建议码化**：`ErrorAdvice` 6 处中文关键词匹配中文消息 → `ErrAdvice` 枚举（新建 `src/app/advice.{h,cpp}`），错误产生处直接给码；`main_form.cpp` 的 `why.find("系统盘")` → `InPlaceReason` 枚举。② **系统输出解析审计**：`safety.cpp` BitLocker（现仅认 en/zh → 改数字/结构锚定）、复核 bcdedit 存在性（GUID 回显 ASCII 已安全）、`format.com`/`manage-bde` 有无被解析；无法结构化的一律 fail-open + warn。③ **字符串账本** `tools/extract-strings.py`（C++ 字面量 + 皮肤 XML → msg id 清单，含"疑似漏网中文"报告） | `make check` 绿；**`ErrorAdvice` 每个建议码/每个 rc 都有单测**；审计结论进 PIT | 2~3 天 |
| **M2 引擎 + 回填** | `src/common/i18n.{h,cpp}`（检测/加载/`T(id)`/回退链/占位符）；`lang/en.lang` + `lang/zh-CN.lang`；回填顺序 GUI 皮肤 XML → GUI 代码 → CLI → app 错误文本（**纯日志不纳入**）；默认镜像名后缀"备份"本地化（文件名优先 ASCII）；`make package` 拷 `lang/` | 切语言跑通备份/还原/暂存/diag；**门禁 a（id 存在）+ b（源码无残留中文）PASS** | 4~5 天 |
| **M3 抗扩容 + 门禁 + 字体** | ① 745×410 像素级布局中定宽文本控件松绑（自适应/省略号/缩字号）；② `tools/check-i18n.py` 进 `make check`：**a** `T()` id 必须存在于 en；**b** 源码/皮肤无残留中文（白名单注释）；**c** 逐语言**文字渲染宽度 ≤ 控件宽度**（GDI 测宽，一条门禁管所有语言）；③ 按语言字体回退链（YaHei/Segoe UI/SimSun/System；日 Yu Gothic、韩 Malgun Gothic；注意 PIT-016⑤ 先声明后引用）；④ CLI `SetConsoleOutputCP(CP_UTF8)` + Win7 实测（不行则非 CJK 语言 CLI 退回英文） | 8 语种宽度门禁全绿；zh 沿用 `界面1.png` diff、en 建新基线、ja/ko/de/ru 抽测 | 2~3 天 |
| **M4 语种铺开 + 文档 + 发布** | 6 语种机翻填表 + 抽测（en/zh 终校）；`README.txt`(en) + `README.zh-CN.txt`；AGENTS 新增**国际化纪律**（新字符串必须 `T()`、必须同步 en+zh、门禁会拦）+ PIT 条目；Win7/10/11/PE × en/zh 完整回归 | `make check`/`make package` 绿 + 双语言回归记录 | 2~3 天 |

**M1.2 系统输出解析审计结论（2026-09-27 已执行）**——审计范围 = 产品代码里"解析外部工具文本"的全部点：

| 解析点 | 结论 | 处置 |
|---|---|---|
| `bcdedit /enum {GUID}` 存在性 | **安全**：只看 GUID 是否回显（ASCII，不受代码页影响，PIT-003） | 不改 |
| `bcdedit` 其余调用（create/set/bootsequence…） | **安全**：全部按**退出码**判定 | 不改 |
| `format.com` 就地格式化 | **安全**：只看 rc（`ops.cpp`） | 不改 |
| `manage-bde -status`（BitLocker 扫描） | **不安全**：按 en+zh **行标签**匹配（`Percentage Encrypted`/`加密`/`Protection Off`），其他语言系统**恒漏报**（静默） | ✅ **已改**：`safety.cpp::IsBitLockerEncrypted` 只认 `<数字>%` 数值取最大；顺带把"保护已挂起但仍在加密"从放行改为提醒（fail-closer） |
| SMART 健康检查（PIT-086） | **安全**：走 IOCTL 二进制数据，展示文案由我们自己拼 | 不改 |
| `bcdboot` 及其余工具 | **安全**：只看 rc / 不解析输出 | 不改 |

> **规则（M4 时写进 AGENTS 国际化纪律）**：能用**退出码 / 二进制 API** 就不解析文本；
> 必须解析时**锚定数值或 ASCII 标识符**，绝不能锚定会被翻译的标签。

**明确不做（写进文档范围声明）**：日志/诊断包、契约文件、救援层屏幕、品牌名；RTL 语言；
翻译质量承诺（en/zh 正式、其余 community）。

**主要风险与对策**：文本扩容致布局返工（最高）→ M3 门禁 c + 初版就松绑定宽控件；
错误码重构引入建议错配 → 单测逐码断言；漏翻半中半英 → 门禁 b 拦截；
`lang/` 被删/损坏 → 内置英文兜底 + warn；Win7 控制台 UTF-8 → 实测 + 退化方案。

**工作记录**：
- ✅ **M1 已完成（2026-09-27）**：M1.1 建议码化（新增 `src/app/advice.{h,cpp}`：`ErrAdvice` 枚举 + `AdviceKey`；`ops.h` 加 `InPlaceReason`，GUI 的 `why.find("系统盘")` 改按码判定；CLI/GUI 三处调用点全部传码）+ M1.2 审计（见上表，BitLocker 解析已语言无关化）+ M1.3 账本 `tools/extract-strings.py`（**381 条 / 13.5 KB**，raw=0）。`make check` 绿：**20 用例 / 127 断言**（新增 `advice_*` 三个用例）+ check-docs PASS。
- ✅ **M2 + M3（部分）已完成（2026-09-27）**，交付：
  · 引擎 `src/common/i18n.{h,cpp}`（`InitI18n`/`LangTag`/`IsSourceLang`/`Tr(char*/wchar_t*)`/`LoadSkinXml`/`ClearI18n`/`LoadLangText`）；
  · 机械化回填 `tools/i18n-wrap.py`（词法状态机，14 个白名单文件：**275 处 `Tr()` + 15 处 `_T()`**，日志出口 7 处按纪律跳过；`--apply` 幂等、`--dump-keys`、`--skeleton`、`--gen-lang`）；
  · 译文表 `tools/i18n-en.py` → **`lang/en.lang` 277 条全译**（`make package` 拷进 `dist/lang/`）；
  · 门禁 `tools/check-i18n.py` 已挂进 `make check`（C1 未翻中文 / C2 键覆盖+死键 / C3 英文残留汉字 / C4 printf 占位符序列一致 / C5 `.lang` 与译文表同步）；
  · 单测 4 例（源语言回源、查表+回退、转义与 `=` 分割、皮肤缺失文件）→ `make check` **24 用例 / 148 断言** 绿；
  · 接线：CLI `--lang xx|--lang=xx` + `SYSRECOVER_LANG`，GUI `InitI18n(nullptr)`，3 处 `GetSkinFile()` → `LoadSkinXml()`，`tests/main.cpp` 钉 `InitI18n("zh")`。
  · **实测**：CLI `help` 默认中文 / `--lang en` 英文 / `SYSRECOVER_LANG=en` 英文；GUI 窗口标题 zh=`九转还原`、en=`SysRecover`。

  **与计划的偏差（本节是唯一计划来源，此处记录为准）**：
  1. **键即原文，不用 `T(id)` 消息 ID** —— 中文源文本直接当 key（zh 不查表天然回退、永不掉 key；en 反查）。故 §14.3 的"门禁 a = id 存在于 en"改为"**键**存在于 en.lang"。
  2. **不建 `lang/zh-CN.lang`** —— zh 即源语言（`InitI18n` 直接清表返回），计划里"en+zh 两份词典"降为**只有 `en.lang`**（后续语种各一份）。
  3. **不建内置英文兜底表**（§14.2 那 +35 KB 取消）—— 回退链是 `tag.lang → en.lang → 源文本`，用外置文件而非 exe 内嵌。
  4. **皮肤 XML 文件不动**：33→34 个中文属性在 `LoadSkinXml()` 加载时按词典翻译，Duilib 走内存解析分支（`UIDlgBuilder.cpp:17`，`GetResourceType()` 默认 `UILIB_FILE`）。skin 三件套仍是纯中文源。
  5. **`.lang` 补 `\=` 转义**：分隔符是**首个未转义的 `=`** —— `备份失败(rc=%d): ` 这类词条的**键本身含 `=`**，不转义会被解析器从中间截断（`i18n.cpp::Unescape`/`LoadTextLocked`、`wrap::lang_escape`、`check::split_entry` 三端同口径，单测 `i18n_escaping_and_equals_split` 覆盖）。
  6. **词条数 277 ≠ 账本 381**：账本是"统计口径"（含日志出口、同一文案在多处重复）；**去重后需翻译的独立键 = 277**（290 处包裹点 → 277 个不同键）。
  7. **M3 只做完门禁 a/b/c 的 a+b**（键覆盖、残留中文、加 printf 序列与译文同步）；**M3①（定宽控件松绑）、②c（文字渲染宽度门禁）、③（按语言字体回退链）、④（Win7 控制台 UTF-8 实测）未做**，**M4（6 语种机翻 + 双语 README + 完整回归）未做** → 下一步。
- ⬜ 下一步：M3 余项（宽度门禁 + 字体回退 + Win7 实测）→ M4（ja/ko/de/fr/es/ru + README + 回归）。
