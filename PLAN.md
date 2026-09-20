# SysRecover（一键还原，C++ 重写）- 总计划书

> 文档版本：v2.2（C++ 新项目 + ops 共享层 + Linux 层定稿）/ 最后更新：2026-09-15 / 维护者：知鉴
> 与 AGENTS.md 的关系：本文件是路线图（做什么、何时算完）；AGENTS.md 是操作手册（怎么做）。

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
