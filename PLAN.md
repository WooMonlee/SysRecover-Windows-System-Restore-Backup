# SysRecover（一键还原，C++ 重写）- 总计划书

> 文档版本：v2.2（C++ 新项目 + ops 共享层 + Linux 层定稿）/ 最后更新：2026-09-15 / 维护者：知鉴
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
- **待决：32 位支持**（2026-09-20 评估完成，用户指示"等几天再说"）。详见
  `docs/08-32位支持评估（待实施）.md`：目前只出 x64，而 x64 exe 在 32 位系统上被系统直接拒绝
  （程序内无法提示）。三条路：只出 x86 / x86 主体 + x64 备份（+1.7MB）/ 两个完整版 + 启动器（+3.8MB）；
  **体积不是问题**（救援层占 93%），**速度只在"备份+重压缩"吃亏**（`fast`≈0~10%，`recovery`≈20~35%），
  还原**完全不受影响**。前置条件：i686 工具链 + 官方 32 位 `libwim-15.dll` + Makefile `ARCH=x86` 分支。
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
- ① `tools/build-debian-rescue.py` ✓（按路径白名单裁剪 + 依赖闭包；**779 模块**；initramfs 34.6MB + 内核 11.6MB）
- ② QEMU 启动 ✓（`Linux 6.12.107+deb13-amd64` → `SR: block devs: sda sda1 sr0` 认盘成功）
- ③ SB 资产已换 Debian ✓（shim 双签 + Debian 签名 GRUB）；QEMU 链验证 shim→GRUB→内核→我们的 initramfs ✓；
      **用户 VMware（Secure Boot 开）实测还原成功** ✓（注意：其固件同时信任 CA2011，故"CA2023-only 新硬件"场景仍需将来在新固件上验证 🚧）
- ④ SBOM/文档已同步 ✓；`dist` **54.3MB**（比 Ubuntu 版更小）；`ZJ_SB_MODE` 默认 `grub` 不变
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
| 3 | **`make check` + 单元测试**：`tools/version.py`、`sysinfo`（注册表解析/中文映射）、日志解析、`exclude` 清单 | **回归安全**（完工后最划算的投入）| 半天 |
| 4 | **Secure Boot 备选 B：整链换 AlmaLinux**（见 §11）→ 解决 2026 新硬件兼容 | 兼容性 | 数小时 |
| 5 | `ARCH=x86` 构建骨架（32 位路线；前置：i686 工具链 + 官方 32 位 `libwim-15.dll`）| 覆盖面 | 1~2 天 |
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
| **P9** | **文件级提取** | ⚠️ **修正**：子镜像浏览/选择**已有**（GUI 下拉、`images`、`restore --index N`）；缺的只是**文件级** | ✅ **已完成（`0.1.8`）**：`WimEngine::ExtractPaths()`（`wimlib_extract_paths`，支持通配符）+ CLI `extract --file <镜像> [--index N] --path "\Windows\..." [--path ...] --dest <目录>`。**实测通过**：从测试镜像取出 `\Windows\win.ini` 与 `\Windows\System32\drivers\etc\hosts`，目录层级保留 ✓。GUI 树留以后 | 半天 |
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
3. **P9 文件级提取**（与 P10 同一套 wim API，一起做省一次上下文）
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
- **P3 的 GUI 按钮改为「网站」链接** ✅ `0.1.15`：界面**右下角**加一个透明底的「网站」链接 →
  初期指向无忧论坛帖子 `https://bbs.wuyou.net/forum.php?mod=viewthread&tid=453579`；
  成熟后换成自家站点 + **网站收日志的报错上报**。URL 只在 `src/gui/main_form.cpp` 的 `kSiteUrl` 一处。
