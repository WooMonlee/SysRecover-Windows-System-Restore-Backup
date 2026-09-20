# SysRecover · 引导设计（BIOS / UEFI / Secure Boot）

> 文档版本：v1.0（2026-09-20）｜关联：《01-需求与定位》《04-跨层契约》
> 细节与踩坑见 `AGENTS.md` §7 与坑位册 **PIT-058/059/060/061/062/063/065/066**。
> 一句话：**引导层只负责"把内核和 initramfs 拉起来"**，怎么执行还原由契约决定。

---

## 1. 三条链路（按环境自动选择）

### ① BIOS / MBR
```
MBR → bootmgr → BCD「实模式启动扇区」条目(device=<目标盘>: path=\grldr.mbr)
    → \grldr → \menu.lst → kernel + initrd → 救援
```
- `grldr` / `grldr.mbr` / `menu.lst` **必须在分区根目录**（GRUB4DOS 硬限制，PIT-058）；
  三个都标隐藏+系统；内核/initramfs 进 `<目标盘>\ZJRESTORE\boot\`。
- BCD 五条命令（`/application bootsector` 不可省，PIT-001/002）。
- 单次启动用 `{bootmgr} bootsequence`。

### ② UEFI / GPT（Secure Boot 关闭）
```
固件 → Boot####（NVRAM 固件启动项，FilePathList → 内核 .efi）
     → 内核（EFI stub）+ initrd（命令行走 OptionalData）
```
- 自己用 `SetFirmwareEnvironmentVariable` 写 `Boot####` + `BootOrder`（同 Rufus/efibootmgr），
  **不经 bootmgr**（bootapp 只收 subsystem=16，加载 subsystem=10 内核会 `0xc000007b`）。
- `OptionalData` 就是内核命令行（**必须 UTF-16LE**，内核 `efi_convert_cmdline()` 按 `char16*` 读）。
- 条目挂在 `BootOrder` **末尾**（默认仍进 Windows，开机启动菜单里能选到）；单次启动用 `BootNext`。

### ③ UEFI / GPT（Secure Boot 开启）—— 本产品当前实现
```
固件 → shimx64.efi（微软签名，固件信任）
     → grubx64.efi（Canonical 签名的 Ubuntu GRUB；shim 内嵌同一把证书 → 无需 MOK 注册）
     → grub.cfg（普通 linux/initrd）
     → vmlinuz-zjrestore（Canonical 签名的 Ubuntu 内核）
     + initramfs-zjrestore.cpio.gz（我们的；**initrd 按 UEFI 规则不做校验**）
```
- **零注册、零成本、不绕过**：链上每个可执行文件都有合法签名，与 Ubuntu 正常开机完全一样。
- 为什么必须"借"签名：`nointegritychecks` 这类"关掉完整性校验"的启动项**自 Secure Boot 引入
  以来就被策略保护**（PIT-065 实测：`该值受安全引导策略保护`）；自签内核又被 GRUB 的
  `shim_lock` 强制校验（PIT-066 变体 C 实测：`error: bad shim lock signature`）。
- 谁签名由谁提供：`bootfiles/sb/{shimx64.efi,grub-ubuntu.efi}` 来自 Ubuntu 官方包（未修改）。

## 2. 救援层（Linux）设计

| 项 | 选择 | 原因 |
|---|---|---|
| 用户态 | Alpine 静态 busybox + musl + ntfs-3g + util-linux(blkid) + wimlib | 与内核无关，体积小 |
| 内核 | **Canonical 签名的 Ubuntu 6.8**（SB 需要签名） | 满足 Secure Boot 链 |
| 模块 | Ubuntu 全量基础模块（999 个，`.ko.gz`） | 驱动覆盖率 = 发行版水平 |
| PID1 | **自写** `bootfiles/alpine/init`（无 rcS/inittab、无交互提示） | 启动干净、可控 |
| 执行 | `bootfiles/zjrestore-lite.sh` | 格式化 + apply + 修引导 + 重启 |

**屏上文字一律 ASCII**：内核内置字体无中文字形（中文显示成方框）。**必须有 fbcon**：若内核
`CONFIG_SYSFB_SIMPLEFB=y` 顶掉了内建 efifb，要加载 `simpledrm`，否则 UEFI 下黑屏像死机（PIT-061）。
进度条与阶段提示见《04-跨层契约》。

## 3. 部署位置与清理

- 救援文件部署到**目标分区**（通常 C:）：`grldr`/`grldr.mbr`/`menu.lst` 在根目录，
  其余进 `<目标>\ZJRESTORE\{boot,scripts,bootfix,logs}`；**数据盘根目录保持干净**（PIT-059）。
- 还原成功 → 目标分区被格式化 → 目标盘上的救援文件自然消失（常驻的 ESP 模块除外）。
- **UEFI+SB 的 ESP 模块常驻**：`<ESP>\EFI\ZJRESTORE\`（约 50MB：内核 14 + initramfs 33 + GRUB/shim 4），
  开机启动菜单里可随时进救援；只有「删除启动还原」才清。
- **恢复旧文件**：部署前先清空自己的目录，避免不同引导方式的历史文件把 ESP 塞满（PIT-063 附带修复）。

## 4. 安全与门禁

- GPT 目标**仅 UEFI 固件放行**；BIOS+GPT 拒绝。
- 四元素校验（GUID/序列号/偏移/大小）fail-closed，见《05-磁盘枚举与安全红线》。
- 改 BCD 前先 `bcdedit /export` 备份。
