# docs/15 · 启动故障分析（还原后空启动菜单 & 未找到 ESP）

> 状态：**部分已证 / 等待机器侧取证**（2026-09-29 更新：镜像侧与实验室复现已完成，见 §2.3、§5.1；§7 已定稿为可执行改造方案，**尚未动代码**）。
> 本文用于**防止忘记**当前的搜索结论与分析，取证后据此推进修复。
> 关联版本：`0.4.1`（`a89b228`）；本轮的代码行号基于当前 HEAD（`0.4.2`）。

## 0. 速览（先看这一段）

| 项 | 结论 | 出处 |
|---|---|---|
| 故障 A 的**镜像侧**嫌疑（`BCD-Template` 缺失/损坏） | **已排除**：模板在、可解析、`bcdboot` 拿它跑成功 | §2.3-A |
| 救援层 `mkntfs` 破坏 BCD 引用 | **已排除**：格式化后条目仍解析 | §2.3-B |
| ESP 空间不足 | **我们真会踩**：`bcdboot` rc=112/123 被**静默吞掉**（确定缺陷） | §2.3-B / §7-P1/P2 |
| 9 种静止 BCD 状态 | 屏幕都是**确定性**的，且**造不出**"选择操作系统 + 中间空白" | §10.4 |
| 故障 A 真因 | **仍未定**，需故障机 `bcd.txt`（H2 首选、H5 待验） | §5.1 / §6-A |
| 故障 B（DiskGenius 重建后找不到 ESP） | **已定位**：`isEsp` 只认精确 ESP 类型 GUID → fail-closed | §3 / §7-P4 |
| 改法 | **P1–P7 已定稿**（含行号、文案、回归），**尚未动代码** | §7 |
| 间歇性"有时能进、有时不能" | 静止状态不可能解释 → 变化输入 V1 坏盘 / V2 热捕获 / V3 NVRAM / V4 引导期改写 ESP | §11 |
| "重启会清 ESP 垃圾？" | **不会**（只可能被"改写"，不是被清理） | §11.1 |
| 故障 A 的真因 | **判定为镜像侧**（用户确认：故障镜像 → 高故障率、好镜像 → 零故障、同机换镜像即好）。实验室 11 种状态**都无法复现**"空菜单" → **停止深挖**，只修我们自己的确证缺陷 | §11.4 |

**立即可做的两件事**：① 你按 §6-A 取证（1 分钟）；② 确认失败现场镜像的 SHA256 是否等于我们分析的 `C:\20260415Win10备份5509.esd`（§11.2）。

---

## 1. 两个故障

### 故障 A · 还原后开机只剩"选择操作系统"两行字，中间全空

- 镜像：`20260415Win10备份5509.esd`（**WIT / wimlib 内核在 VM 里做的**，不在本机）。
  同目录的 `20260911Win10不忘初心LTSC2021_19044.7725.esd` 是**别人的、非本产品做的**，用它还原正常。
- 现象：用我们的工具还原 → 重启 → EFI 转圈后立刻出现
  **「选择操作系统」/「更改默认值或选择其他选项」两行字，中间没有任何条目**，回车也无变化，反复如此。
- **进入 PE 再做一次引导修复，还是同样现象。**
- **同型号机器上"很多台有问题、有些没问题"。**
- UEFI 机型。

**这条现象已经能推出两个硬结论**（不依赖取证）：

1. 屏幕文字是 **Windows Boot Manager（`bootmgfw.efi`）的中文字串** → **ESP 上的 `bootmgfw.efi` 存在且能跑、ESP 的 `BCD` 能读出来**（否则是固件菜单或 `bootmgr is missing` 之类）。问题**只在 `BCD` 的内容**：`{bootmgr}` 的 **`displayorder` 里没有可解析的条目**。
2. **"PE 里再修复也没用"** → PE 那次修复**同样失败了**（或者用的不是 `bcdboot`）。UEFI 下 `bootrec /rebuildbcd` 之类对 ESP 基本无效；`bcdboot` 若失败，最典型原因是读不到源镜像里的
   `%WINDIR%\System32\Config\BCD-Template`（见 §4-2）——**而 PE 读的是还原后那台机器上的模板，也就是 WIT 镜像里的那份**，所以"两次用同一个坏源头，两次都失败"，现象完全一致。

### 故障 B · DiskGenius 重建分区后跑我们的还原 → "找不到引导盘"

- 场景：用 DiskGenius **删掉 GPT 磁盘前 2 个分区 + C 盘**，再用 DiskGenius Pro 6 **重建前 2 个分区**，把故障镜像写进 C 盘，然后跑我们的还原 → **真正无法启动，找不到引导盘**。
- 换回好镜像后正常。
- 同型号机器，多数中招，少数正常。

**已拿到的唯一日志**（用户从该机器拷回 `C:\logs\`，2026-09-28 16:14）直接命中这一条：

```
16:14:22 已载入镜像：D:\Software\ESD_ISO_WIM\U盘启动5509重做系统.esd
16:14:26 target disk health warning: 磁盘0 SMART 即将故障（驱动器自报：即将故障）
16:14:27 space precheck: content 8.9 GB, need ~10.1 GB, target 100.0 GB
16:14:27 restore stage image=... disk=0 part=1
16:14:27 restore mode: staged reboot (目标是正在运行的系统盘（必须重启后脱机还原）)
16:14:27 restore task written to D:\Software\ESD_ISO_WIM\九转大还原
16:14:27 [ERROR] UEFI 机器未找到 ESP 分区，无法部署引导层
16:14:27 GUI status: UEFI 机器未找到 ESP 分区，无法部署引导层
```

`progress.json`：`{"phase":"restore","percent":100,"status":"failed",...}`（顺带：**失败时 `percent` 还是 100，是个小瑕疵**）。

> ⚠️ 这份日志**证明了"未找到 ESP"这个失败模式真实发生**，但**还不能证明**就是 DiskGenius 分区类型的问题 —— 要 §6-B 的分区表取证才能定死。
> 另注：故障 B 的镜像文件名在日志里是 `U盘启动5509重做系统.esd`，与 §1-A 说的 `20260415Win10备份5509.esd` 不同名（同为 5509），**两者是不是同一个文件待确认**。

---

## 2. 已拿到的证据（本机只读，未碰盘）

### 2.1 `C:\logs\SysRecover-20260928.log`
见上。关键事实：

- 顺序是 `BcdExport` → 写 `_zjresy*.log` → 写 `restore-task.conf` → **然后才**找不到 ESP 失败。
  即：**任务/契约已经落到目标分区与软件目录，引导层却没装上**（引导没装 → 不会重启进救援层 → 任务不会被执行，但盘上留了残留契约文件）。
- 该机**磁盘 0 SMART 报"即将故障"**（PIT-086 的告警正常弹出并被跳过继续）。

### 2.2 `C:\logs\bcd-backup`（28672 B，`bcdedit /export` 的系统 BCD 快照）

用 `bcdedit /store C:\logs\bcd-backup /enum all` 读出来的要点：

| 项 | 值 | 说明 |
|---|---|---|
| `{fwbootmgr} displayorder` | `{bootmgr}` | 固件层正常 |
| `{bootmgr}` | `path \EFI\MICROSOFT\BOOT\BOOTMGFW.EFI`、`default {default}`、**`displayorder {default}`**、`timeout 5` | **系统 BCD 本身是好的** |
| `{default}` | `Windows 11.25H2 Pro China x64`、`path \Windows\system32\winload.efi`、**`bootmenupolicy Legacy`** | 见下 |
| WinRE / resume / memdiag | 均在 | — |

**两个观察**：

1. **这份快照是"健康"的** → 说明**故障 B 那台机器暂存时的系统 BCD 没问题**，失败点确实在 ESP 分区本身（找不到），不在 BCD。
2. **`{default}` 是 `bootmenupolicy Legacy`** —— Legacy 策略 = **无论有没有条目都弹菜单**（Standard 才是"只有一个系统时隐藏菜单"）。
   这解释了**为什么会看到"选择操作系统"这个界面**：那台机器本来就会弹菜单。
   ⚠️ 这个 Legacy 不是我们设的（全仓 `git grep bootmenupolicy` 只有 `bootfix.cpp` 动 `timeout`，没有任何地方写 `bootmenupolicy`），是该机镜像/安装方式自带的。
   **但要注意**：`device unknown` / `osdevice unknown` 是**离线 store 的正常显示**（在线 enum 显示 `partition=\Device\HarddiskVolume1`），**不是故障**，别被它误导。

### 2.3 2026-09-29 本轮证据（镜像侧 + 实验室复现，全部本机只读/用一次性 VHD）

**A. 镜像侧（推翻 H1）**

| 检查 | 故障镜像 `20260415Win10备份5509.esd` | 3 个"无故障"对照镜像 |
|---|---|---|
| 子镜像 | `Windows 10.22H2 Pro for Workstations x64 10.0.19045.3208`，内容 7.57 GB，源 `C: (VMware Virtual NVMe)` | 各自 1~4 个子镜像 |
| `\Windows\System32\Config\BCD-Template` | **存在，28672 B，`bcdedit /store … /enum all` 解析完全正常**（含 `{bootmgr}`/`{default}`/`{memdiag}`…） | 存在（28672 B） |
| `\Windows\Boot\EFI\bootmgfw.efi` | **存在**（1 588 608 B） | 存在 |
| `\Windows\System32\{winload.efi,ntoskrnl.exe,Drivers\stornvme.sys}` | 全部存在 | 全部存在 |
| `\EFI`、`\Boot\BCD`、`\bootmgr`、`\Recovery\WindowsRE\Winre.wim` | **不存在** | **同样不存在**（C 盘单分区捕获本来就没有 → 属正常，不是差异点） |
| 把整份 `\Windows` 解出（20 300 文件 / 6.15 GB）后跑我们同一条命令 | `bcdboot <win> /s X: /f UEFI` → **rc=0**，ESP 生成完整（`bootmgfw.efi`+`BCD`+`Fonts\*`+`Resources\bootres.dll`+`zh-CN`/`en-US` MUI+`\EFI\Boot\bootx64.efi`），`displayorder {default}` 正常 | — |

⇒ **H1（镜像 BCD-Template 缺失/损坏 → bcdboot 失败）不成立**：模板在、能解析、bcdboot 拿它跑成功。

**B. 实验室复现（`D:\lab.vhd` = GPT 300 MB ESP + 19 GB NTFS，QEMU + OVMF 真跑 `bootmgfw.efi`，逐屏 `screendump`）**

| 构造的 ESP/BCD 状态 | 实测屏幕 | 结论 |
|---|---|---|
| 正常 BCD（bcdboot 产物） | 正常进 `winload`（我后来删了 `W:\wf` 才 0xc000000f） | 基线 |
| 用 Windows **重新格式化目标分区**（等价救援层 `mkntfs`）后 | BCD 里 `{default}` **仍解析到同一分区** | **"mkntfs 换掉卷标识 → 条目失效" 不成立** |
| ESP 只剩 ~3 MB | `bcdboot` **rc=112**，只落 `boot.stl`+`bootmgfw.efi`、**无 BCD** | 满盘失败模式 = "引导文件缺失/0xc0000098"，**不是空菜单** |
| 已有完整 ESP 再跑一次 bcdboot（源不全） | rc=123，**旧 BCD 与 `displayorder` 都被保留** | bcdboot 失败**不会**清掉旧 BCD |
| `displayorder` 删空 | 直接引导 `{default}` → 0xc000000f（**不显示菜单**） | 不是空菜单 |
| 整份 `BCD-Template` 当 BCD | 0xc0000001 | 不是空菜单 |
| 条目对象被删空 | 0xc0000098 | 不是空菜单 |
| 条目设备指向已摘除的卷 | 0xc000000e | 不是空菜单 |
| 强制 `displaybootmenu yes`（单条目） | 文本菜单，条目正常列出 | 菜单本身没坏 |

⇒ **以上 9 种状态都造不出「选择操作系统 / 更改默认值或选择其他选项 / 中间空白」这一屏**。该屏 = bootmgr 图形 OS 页 **列表为空**，而它由 **`{bootmgr}.displayorder` 里"能枚举的条目"** 决定 → 仍须拿到故障机 `bcd.txt` 才能定死（§6-A）。**注意**：其中"ESP 空间不足"这条是**我们自己真会踩**的确定缺陷（`bcdboot` 失败被静默吞掉），无论 A 的真因是什么都要修（§7 P1/P2）。

---

## 3. 代码定位（本仓库，行号基于 `a89b228`；`0.4.2` 的对应行号见 §7 表内注释）

| 位置 | 内容 | 定性 |
|---|---|---|
| `src/disk/disk.cpp:262-264` | `p.isEsp` **只在** `GPT 分区 && PartitionType == kGuidEsp`（精确类型 GUID `c12a7328-…`）时置真 | **故障 B 的直接嫌疑** |
| `src/disk/disk.cpp:331-339` | `FindEspPartition()` 遍历**所有磁盘**找 `isEsp`，找不到返回 false | 同上 |
| `src/app/ops.cpp:602-603` | `if (req.repairBoot) BcdExport(logsDir + "\\bcd-backup")` | 证据来源 ✓ |
| `src/app/ops.cpp:668` | `WriteRestoreLog`（主契约，写目标分区根） | 失败前已写 |
| `src/app/ops.cpp:683/702` | `WriteRestoreTask`（副契约）+ 日志 `restore task written to …` | 失败前已写 |
| `src/app/ops.cpp:724-730` | `if (!FindEspPartition(esp)) { … err = "UEFI 机器未找到 ESP 分区，无法部署引导层"; return 1; }` | **日志里那句就是这里** |
| `src/app/ops.cpp:739` | `InstallUefiBootEntry(espRoot, exeDir, blog)` —— 往 ESP 写 `EFI\ZJRESTORE\`（内核 12 MB + initramfs 20.5 MB ≈ **33 MB**） | 占 ESP 空间 |
| `src/app/ops.cpp:742-751` | `bcdboot <目标>:\Windows /s <ESP>: /f UEFI`，注释写明"用目标分区的 Windows（暂存时它还是旧系统）" | **故障 A 的核心嫌疑** |
| `src/app/ops.cpp:749-750` | `int brc = RunProcess(...); LogInfo("bcdboot (UEFI) rc=" + …)` —— **只打日志，不判断、不 fail-closed、不重试、不验证** | **确定的代码缺陷** |
| `src/app/ops.cpp:755-761` | `InstallUefiBootEntry` 的 `ok` 有检查 ✓（但在 `UnmountEsp` 之后） | 尚可 |
| `src/boot/bootfix.cpp:158` | BIOS 分支给 bootfix 的 BCD 设 `{bootmgr} timeout 0` | 与故障 A 无关（UEFI 不走这条） |

**结论：我们的 UEFI 引导修复"跑没跑成"是不可知的** —— `bcdboot` 返回码被丢弃，产物也没人校验。这是无论 A 是什么根因都该修的点。

---

## 4. 检索结论（带出处）

### 4.1 `bcdboot` 从哪来、做什么（官方）
- **BCDBoot 官方文档**：<https://learn.microsoft.com/zh-cn/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di?view=windows-11>
  - "BCDBoot 创建新的 BCD 存储，并使用 **`%WINDIR%\System32\Config\BCD-Template`** 文件初始化系统分区上的 BCD"
  - `/s` 指定系统分区；**`/s` 时不往 NVRAM 写条目**，依赖 `\efi\boot\bootx64.efi` 默认路径
  - 官方"修复系统分区"流程 = 进 WinPE → `bcdboot C:\Windows`
  - 2026 文档新增：**Win10+ 升级时默认保留其它启动项，`/c` 才是"从头来过"**；`/m` 合并现有项

### 4.2 `BCD-Template` 缺失/损坏 → `BFSVC Error`
- superuser：*"Windows 8: BFSVC Error: Could not open the BCD template store. Status = [c000000f]"*
  <https://superuser.com/questions/713146/windows-8-bfsvc-error-could-not-open-the-bcd-template-store-status-c000000>
  - 症状即 `bcdboot` **直接失败**；解法是从好的系统/安装介质拷回 `BCD-Template`。
- CSDN 同类案例：<https://blog.csdn.net/hfhbutn/article/details/100778805>
  - 记录了完整排错：`bcdboot` 报 c000000f → 检查 `%WINDIR%\System32\Config\BCD*` → 从 `winsxs\*bcdtemplate*` 或同版本系统拷回。
  - **注意 `sfc` 修不了这个文件**（文中明写 Win10 测试不支持）—— 所以"PE 里修复"通常也修不好。

### 4.3 "选择操作系统"界面空白的官方/半官方描述
- **Microsoft Q&A**：*"'Choose an operating system' screen shows no options"*
  <https://learn.microsoft.com/en-us/answers/questions/3999313/choose-an-operating-system-screen-shows-no-options>
  - 用户原话："I get to a blue screen that says choose an operating system, but … don't show up. **Instead the screen is just blank** … the timer and default OS both say **`%1`**."
  - **`%1` 没被替换 = 模板/变量没写进去** → 与"BCD 内容不完整"的判断一致。
- thewindowsclub：*Choose an operating system screen or Dual boot menu missing*
  <https://www.thewindowsclub.com/choose-an-operating-system-screen-missing>
  - 修法就是 `bcdedit /set {bootmgr} displaybootmenu yes` + **`Bcdboot D:\Windows`** 重建条目 —— 再次指向"最终靠 bcdboot"。
- MicrosoftDocs（Azure VM 启动管理器排查）：
  <https://github.com/MicrosoftDocs/SupportArticles-docs/blob/main/support/azure/virtual-machines/windows/troubleshoot-guide-windows-boot-manager-menu.md>
  - 说明"Choose an operating system to start, or press TAB…"这个界面由 `{bootmgr}` 的 **`displaybootmenu` / `timeout`** 控制 → **界面能出现 = `{bootmgr}` 对象本身是好的，缺的是 `displayorder` 里的子项**。

### 4.4 ESP 分区类型 GUID：能启动但 `bcdedit /enum` 挂掉
- superuser 讨论（"One situation where the system will boot but `bcdedit /enum` will fail is that the EFI boot partition has the **wrong partition type**…"）
  → 对应我们 §3 第 1 行：`isEsp` 只认精确 ESP GUID。
  ⚠️ 该条本轮未取到稳定直链，**引用前请复核**（现象本身也与 DiskGenius 重建分区的操作吻合）。

### 4.5 ESP 空间不足 → `bcdboot` 可能"写一半"
- 修复指南类文章（含错误清单"初始化库系统卷时失败 / 尝试复制启动文件时失败"）：
  <https://www.cdz423.com/post/46367.html> —— 明确 **ESP 常见仅 100 MB**，官方建议 **200 MB**。
- 中文引导修复实践：<https://www.cnblogs.com/heiyizixia/p/10734964.html> —— `bcdboot C:\Windows /s S: /f uefi /l zh-cn` 的前提：**启动分区存在 + Windows 目录里启动文件存在**。
- 检索中出现的**经验性说法**（本轮未拿到稳定直链，标为**待复核**）：
  > ESP 剩余空间不足时，`bcdboot` 可能报告成功但**只写出文件集的一部分**，
  > 典型特征是 **`\EFI\Microsoft\Boot\en-US\`（locale/资源目录）缺失**。
  → 若成立，**我们往 ESP 塞的 33 MB 救援模块正是挤压源**（与 AGENTS PIT-066 里"清理旧文件后 ESP 从 31.9 MB free 变 81.3 MB free"的实测吻合）。

### 4.6 相关但**未采纳**的检索噪音
双系统 / GRUB / Ubuntu 相关的"菜单消失"结果（CSDN、51CTO 等）与本故障**无关**（本机是纯 Windows + 我们的 UEFI 直启内核），不作为依据。

---

## 5. 假设与排序

### 故障 A（空启动菜单）——**根因未定，三条候选**

| # | 假设 | 支持它的事实 | 反对/未知 |
|---|---|---|---|
| **H1（首选）** | **WIT 镜像里的 `Windows\System32\Config\BCD-Template` 缺失或损坏** → 我们暂存时的 `bcdboot`、以及用户后来在 PE 里的 `bcdboot` **两次都失败**，ESP BCD 始终是坏/旧的 | 现象两条硬结论（§1）；`bcdboot` 失败的头号原因就是它（§4.2）；`sfc` 也修不了 → "PE 再处理没用"完全吻合；**换别人的镜像就好** | 需要验证镜像里有没有这个文件（§6-A 第 2 项） |
| **H2** | **ESP 空间被我们 33 MB 救援模块挤爆** → `bcdboot` 写一半 | §4.5；PIT-066 实测过 ESP 只剩 31.9 MB | 依赖"写一半"那条经验说法属实（待复核）；且需解释"有些机器没问题"= ESP 容量/剩余空间差异 |
| **H3** | **`bcdboot` rc 被丢弃（`ops.cpp:749-750`）**，失败无提示、无重试、无产物校验 | 代码事实，**确定存在** | 它是"放大器"不是根因 —— 单独修它只能让故障更早暴露，不能让它不发生 |
| H4（弱） | 该机 ESP BCD 在我们暂存**之前**就已经是坏的（与 WIT 镜像无关） | "有些机器没问题"可由机间差异解释 | 解释不了"换好镜像就好"（同一台机）→ 基本可排除 |

**为什么"菜单界面能显示"很关键**：`选择操作系统 / 更改默认值或选择其他选项` 是 `bootmgfw.efi` 的字符串 → 说明 `bootmgfw.efi` 和 BCD 文件都完好，**坏的只是 `displayorder`**。这排除了"ESP 文件全丢""bootmgr 损坏"一类的假设，把范围收窄到 **BCD 内容**。

### 5.1 2026-09-29 复核后的假设排序（据 §2.3）

| # | 假设 | 现状 |
|---|---|---|
| ~~H1~~ | ~~镜像 `BCD-Template` 缺失/损坏~~ | **已排除**（§2.3-A：模板在、可解析、bcdboot 跑成功） |
| **H2** | **ESP 空间被我们 33 MB 载荷挤爆 → 我们的 `bcdboot` 失败且 rc 被吞** → 机器保持/退回"列表为空的 BCD" | **升级为首选**（我们的失败模式已在 §2.3-B 实测确认：rc=112/123 被静默）；待 `esp-free.txt` 佐证 |
| **H3** | `bcdboot` rc 被丢弃 + 产物不校验 | **确定存在**，且是让 H2 变成"用户可见故障"的关键放大器（必须修） |
| **H5（新）** | 故障机 ESP 的 BCD 里 `displayorder` 指向**已不存在的条目/设备**（机器此前已被多次"半成功"重装过） | 与"同机换好镜像就好""PE 再修也没用（仍失败）"吻合；`bcd.txt` 一到即可判 |
| ~~H6~~ | ~~救援层 `mkntfs` 让 BCD 的卷引用失效~~ | **已排除**（§2.3-B：格式化后条目仍解析） |
| H4（弱） | 机器 ESP BCD 在暂存前就坏了 | 保留（作为 H5 的子情形） |

**验收口径**：真因若不是 H2/H3 一类"我们可控的写入失败"，修完 §7 后故障仍会出现 —— 所以 §7 的第 3 条（写后断言）是**唯一能无论根因都拦住该症状**的保险丝，优先级最高。

### 故障 B（未找到 ESP）——**较确定**

- **B1（主）**：DiskGenius 重建的前 2 个分区**没有 ESP 类型 GUID**（`c12a7328-…`），被标成 Basic Data / 或压根没建 MSR+ESP → `isEsp` 恒 false → `FindEspPartition` fail-closed。旁证见 §4.4，与操作方式吻合。
- **B2（次）**：DiskGenius 重建的是**裸 FAT32 分区但没写 `\EFI` 目录**，或整个 ESP 被删掉没重建 → 即使 GUID 对也缺目录。
- **B3（需排除）**：磁盘 0 SMART 已报"即将故障"（日志实证）——**坏盘本身也可能导致分区表读取异常**，不能只怪 DiskGenius。
- **共同后果**：我们 fail-closed 是**正确**的（不该在没有 ESP 的机器上硬写），但**错误信息只说"未找到 ESP"，不告诉用户下一步怎么办**（是分区类型不对？还是分区没了？还是磁盘坏了？）。

---

## 6. 取证清单（**只读，不碰盘**，等你从受影响机器拿）

### 6-A · 故障 A（空启动菜单的机器）

1. 进 PE → `mountvol X: /s`（挂 ESP），然后：
   ```bat
   bcdedit /store X:\EFI\Microsoft\Boot\BCD /enum all > bcd.txt
   dir X:\ /s > esp-tree.txt
   fsutil volume diskfree X: > esp-free.txt
   bcdedit /enum firmware >> esp-free.txt
   ```
   把三个文件拷回来；**再手机拍一张那屏**（确认真是 bootmgr 的图形"选择操作系统"页）。
   - 看点：`{bootmgr}` 的 `displayorder` 是空的、还是指向不存在的 GUID；`displaybootmenu`/`timeout` 是什么；
     `\EFI\Microsoft\Boot\` 下**有没有 `en-US\`（或 `zh-CN\`）locale 目录、`Resources\bootres.dll`、`Fonts\*`**（"写一半"的特征）；ESP 剩余空间（**预测：< ~5 MB**）。
2. ~~把 `20260415Win10备份5509.esd` 拷回来验 `BCD-Template`~~ → **2026-09-29 已完成，见 §2.3-A（模板存在且有效，bcdboot 可用）**。
3. （可选）同一台机上把好镜像 `20260911…LTSC….esd` 也列一遍做对照。
4. （可选）PE 里"再处理"到底点的是什么工具/命令 —— 如果是 `bootrec /rebuildbcd`，那在 UEFI 上本来就无效，等于没修过。

### 6-B · 故障 B（DiskGenius 重建分区的机器）

1. `diskpart` → `list disk` → `list part`（或 DiskGenius 分区属性截图），
   **重点看前两个分区的类型/卷标/文件系统**，以及是否有 MSR、ESP。
2. 挂一下 ESP（若还认得出）→ `dir X:\`，看有没有 `\EFI\Microsoft\Boot\bootmgfw.efi`。
3. `C:\_zjresy*.log`（如果有）—— 我们失败前写的那份契约，能确认当时的目标 offset/size。
4. 顺带：那块盘 SMART 已报"即将故障"，**建议先换盘再排障**，否则取证结果可能被坏盘污染。

---

## 7. 修复方案（**2026-09-29 已实现**；行号 = 实现时的 HEAD）

> 实现状态见 **§12（实现与验证记录）**。P1–P5 + P7 全部落地；UEFI 分支的机器侧回归待做（本机是 BIOS）。
> 原则：① 检查全部落在**动目标分区之前**（暂存阶段，`IsUefiFirmware()` 分支）或**唯一可修的位置**（就地还原成功后）；② UEFI 写入的**结果必须被验证**，不能只看 `bcdboot` 的 rc；③ 报错文案必须能指导下一步动作。

### P1 · `bcdboot` 返回码 fail-closed（三处）

| 位置 | 现状 | 改法 |
|---|---|---|
| `src/app/ops.cpp:786-787`（暂存 / UEFI） | `int brc = RunProcess(...); LogInfo("bcdboot (UEFI) rc=" + …)` —— 只记日志 | `brc != 0` → 收集 `out`（BFSVC 原文，压成单行 ≤ 200 字符）→ `err = Tr("写入 ESP 引导失败（bcdboot rc=%d）：%s")` → `LogError` → `ProgressDone("restore","failed")` → `ReleaseOpLock()` → `return 1` |
| `src/app/ops.cpp:152-158`（就地 / UEFI） | 同样只记日志 | 同上（`)` 此路目标分区**已被格式化**，报错要附"可用 `repair-boot` 修复引导"） |
| `src/app/ops.cpp:163-167`（就地 / BIOS） | 只记日志 | 同上（BIOS 分支文案区分 `/f BIOS`） |

注意：`bcdboot` 在**暂存路径**必须先于"写契约"完成（顺序见 P2-b），否则会重演故障 B 的"契约已写、引导没装"。

### P2 · 空间与顺序（把 `bcdboot` 排在复制 33 MB 载荷之前）

- **a) 预留余量**：`src/boot/uefi.cpp:472-494` 现有检查只留 `kMargin = 1 MB`，且**只算我们自己的载荷**。`bcdboot` 刷新 `\EFI\Microsoft\Boot\` 需要 ≈ 9 MB（`bootmgfw.efi` 1.5 + `bootmgr.efi` 1.5 + `Fonts` 4.5 + `Resources` + MUI + BCD）。改：`need = 我方载荷 + 9 MB`，`kMargin` 提到 **16 MB**；不足时按现有 `Tr("ESP 空间不足…")` 路径明确报错。
- **b) 调整 `src/app/ops.cpp:742-798` 的执行顺序**为：
  `FindEspPartition`（含 P4 回退）→ `MountEsp` → **① 空间预检**（P2-a）→ **② `bcdboot`**（P1）→ **③ 写后断言**（P3）→ **④ `InstallUefiBootEntry`**（自带空间检查；失败即中止）→ `UnmountEsp` → `SetUefiBootNext`。
  理由：先让 `bcdboot` 用最富裕的空间干活，之后才吃掉 33 MB。
- **c) 目标模板预检**：`GetFileAttributesW(<target>:\Windows\System32\Config\BCD-Template)` 不存在或大小 0 → fail-closed。文案：*"目标系统的 `System32\Config\BCD-Template` 缺失/为空，`bcdboot` 无法生成引导（多见于第三方精简/万能镜像）。请换用完整镜像，或先手工修复该文件。"*（本轮已证明我们的故障镜像**有**此文件；此条是为别人的/GHO 镜像兜底。）
- **d) ESP 剩余空间预检文案**要给出出路：*"ESP 剩余 %.1f MB，不足（需 %.1f MB + 16 MB 余量）。请先在 Windows 里点「删除启动还原」清掉 `\EFI\ZJRESTORE\` 旧版残留，或用 `diskpart` 扩大 ESP。"*

### P3 · 写后断言（**唯一能拦住"空菜单"的保险丝**）

新增 `bool VerifyEspBcd(const std::wstring& espRoot, std::string& detail)`（放 `src/boot/uefi.cpp`，声明进 `uefi.h`），在 `bcdboot` 之后调用：

1. `bcdedit /store <espRoot>\EFI\Microsoft\Boot\BCD /enum all`（RTF/编码无关，**只匹配 ASCII 关键字与 GUID**，PIT-003/005）；
2. 断言输出里 **`displayorder` 行存在且其后至少跟一个 `{…}`**；
3. 断言存在 **`Windows Boot Loader` 段**（或含 `\Windows\system32\winload.efi`）；
4. 断言这些**文件真的在**（bcdboot "写一半"的特征集）：`\EFI\Microsoft\Boot\Resources\bootres.dll`、`Fonts\` 下至少一个 `*_boot.ttf`、locale 目录 `zh-CN\` 或 `en-US\`；
5. 任一条不满足 → `false` + `detail` 列出**缺哪一项**；调用方（P1 的位置）fail-closed 并把 `detail` 给用户。

解析逻辑（第 2/3 条）**抽成纯函数**以便单测：`bool BcdEnumHasOsEntry(const std::string& enumOutput, std::string& why)`。

### P4 · 找不到 ESP 的回退与文案（治故障 B，兼容 DiskGenius 重建的分区）

`src/disk/disk.cpp:331-339` `FindEspPartition` 改为三级：

1. 现有：`GPT && isEsp`（`:261-262` 的精确类型 GUID）；
2. **回退**：`fs` 为 `FAT32`/`FAT`/`FAT16` **且** 根下存在 `\EFI\Microsoft\Boot\bootmgfw.efi`（或 `\EFI\BOOT\BOOTX64.EFI`）→ 认定可用（**固件只认路径、不认分区类型 GUID**，所以安全）。命中回退时 `LogWarn`：*"该引导分区类型不是 EFI System（可能是 DiskGenius 重建所致）；功能可用，建议把分区类型改回 `c12a7328-f81f-11d2-ba4b-00a0c93ec93b`。"*
3. 都没有 → 返回 false，**并带出诊断串**（每块盘每个分区：分区号/类型 GUID/文件系统/卷标/是否有 `\EFI`），供 `ops.cpp:761-767` 的报错使用。

配套文案（`ops.cpp:761-767` 现在只有一句"UEFI 机器未找到 ESP 分区"）：

| 情形 | 文案（要点） |
|---|---|
| 有 FAT 分区但无 `\EFI` | "找到 FAT 引导分区但没有 `\EFI` 目录：请在 PE 里对该分区跑 `bcdboot <系统盘>:\Windows /s <该分区>: /f UEFI`" |
| 完全没有 FAT 分区 | "未找到 ESP/FAT 引导分区（DiskGenius 重建分区时可能漏建 ESP，类型需为 EFI System）。当前分区表：…" |
| 磁盘 SMART 异常（`QueryDiskHealth`） | 追加"该盘已报即将故障，建议先换盘"（PIT-086 已有能力） |

### P5 · 新增「修复引导」动作（已坏的机器不用重装）

- **CLI**：`SysRecover.exe repair-boot [--disk N --part M] [--yes]` —— 目标分区默认自动判定（有 `\Windows\System32\winload.exe` 的分区）→ 找 ESP（含 P4 回退）→ `MountEsp` → `bcdboot`（P1）→ `VerifyEspBcd`（P3）→ 打印 `displayorder` 摘要。退出码复用 §9 约定（0/1/3/4）。
- **GUI**：`BootMenuBtn`（安装/删除菜单）流程里顺带做 **P3 校验**：若发现 ESP BCD 缺 `displayorder` → 自动补跑一次 `bcdboot` 并提示结果；状态栏加一条「修复引导」入口（或按钮 tooltip 里说明）。
- 该动作同时是故障 A/B 现场的**一键自助修复**，不依赖重装镜像。

### P6 · 回归（`tools/vmtest/`，不进 `make check`）

- 新增 `uefi-esp-repair-smoke.ps1`（复用本轮 `D:\lab` 的做法，脚本化）：
  1. `diskpart` 建 GPT + 300 MB ESP + NTFS 分区（一次性 VHD）；
  2. 解 `\Windows` 到 NTFS 分区 → `bcdboot` → 断言 `VerifyEspBcd` 通过；
  3. **故意破坏**（删 `displayorder` / 删 `Resources` / 把 ESP 填到只剩 3 MB）→ 断言 `repair-boot` 能报对错或修好；
  4. Basic-Data 类型的 FAT 分区 → 断言 P4 回退能找到（**故障 B 的回归**）。
- 单测（`make check`，纯逻辑）：`BcdEnumHasOsEntry()` 用若干段真实 `bcdedit /enum` 文本做用例（含本轮 9 种状态的输出）。
- ⚠️ 本机是 **BIOS + GPT**，`safety.cpp` 会拒绝端到端还原 → 端到端只能在 QEMU/OVMF（如本轮）或真 UEFI 机上跑。

### P7 · 顺手（本轮实测踩到的两个小坑）

- `extract --dest X:\`（**盘符根**）会因 `CreateDirectoryW("X:\\")` 返回失败而报"无法创建输出目录（父目录需已存在）"（`src/cli/main.cpp:494-498`）→ 把"根目录视为已存在"。
- 失败时 `progress.json` 的 `percent` 仍写 100（§2.1）→ 失败分支写 `percent = 完成度`。

---

## 8. 本轮检索原始链接清单

1. BCDBoot 命令行选项（Microsoft Learn）<https://learn.microsoft.com/zh-cn/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di?view=windows-11>
2. BFSVC: Could not open the BCD template store (superuser) <https://superuser.com/questions/713146/windows-8-bfsvc-error-could-not-open-the-bcd-template-store-status-c000000>
3. BFSVC 解法（含 winsxs 拷回模板、sfc 无效）<https://blog.csdn.net/hfhbutn/article/details/100778805>
4. 'Choose an operating system' screen shows no options（Microsoft Q&A，`%1` 未替换）<https://learn.microsoft.com/en-us/answers/questions/3999313/choose-an-operating-system-screen-shows-no-options>
5. Choose an operating system screen or Dual boot menu missing（thewindowsclub）<https://www.thewindowsclub.com/choose-an-operating-system-screen-missing>
6. Windows Boot Manager 菜单排查（MicrosoftDocs / Azure）<https://github.com/MicrosoftDocs/SupportArticles-docs/blob/main/support/azure/virtual-machines/windows/troubleshoot-guide-windows-boot-manager-menu.md>
7. 修复 EFI/GPT 引导加载程序（含 ESP 100 MB、bcdboot 错误清单）<https://www.cdz423.com/post/46367.html>
8. Windows10 引导修复（`bcdboot … /s S: /f uefi` 实践）<https://www.cnblogs.com/heiyizixia/p/10734964.html>
9. （待复核，无稳定直链）"ESP 分区类型 GUID 错 → 能启动但 `bcdedit /enum` 失败"；"ESP 空间不足 → bcdboot 写一半、locale 目录缺失"

---

## 9. 下一步

1. ~~（你）按 §6-A 取证~~ → **降级为可选/锦上添花**（用户 2026-09-29 已定：故障 A 属镜像侧，停止深挖；见 §11.4）。若哪天真想弄清，就取 `bcd.txt` + 那屏照片即可。
2. **（我，等你点头）** 照 §7 落地 **P1–P5 + P7**（P6 作回归）：`make check` → `make package` → QEMU/OVMF + 受影响机器回归（**故障 B 看报错文案是否指到点上**）。
3. 修复顺序：**P3（写后断言）→ P1（rc fail-closed）→ P2（空间/顺序）→ P4（ESP 回退）→ P5（修复引导动作）→ P7/P6**。P3 先做，因为它对**任何根因**都有效。

---

## 10. 分析过程（本轮可复现步骤，供日后重放）

> 全部在开发机完成，**除一次性 VHD 外不碰任何真实分区**；所有制品放在 `D:\lab\`（可随时删）。

### 10.1 镜像侧（只读）

```powershell
$exe = 'D:\Prog\_Project\SysRecover\dist\x64\SysRecover.exe'

# ① 列子镜像（版本/内容大小/源分区）
& $exe images --file 'C:\20260415Win10备份5509.esd'

# ② 单个/一组文件"存在性探测"：rc=0 → 存在；rc=1 → 不存在
#    ⚠️ --dest 的**父目录必须已存在**（见 §7-P7）
& $exe extract --file '<镜像>' --index 1 --path '\Windows\System32\Config\BCD-Template' --dest 'D:\lab\probe'

# ③ 把整份 \Windows 解出来做"真跑 bcdboot"的源（20300 文件 / 6.15 GB / 73 s）
& $exe extract --file '<镜像>' --index 1 --path '\Windows' --dest 'W:\wf'   # W: = 实验 VHD 的 NTFS 分区

# ④ 模板能不能被 Windows 认（只读）
bcdedit /store 'D:\lab\probe\Windows\System32\Config\BCD-Template' /enum all
```

顺带核对的 8 个路径（故障镜像 + 3 个对照镜像都跑过）：`\EFI\Microsoft\Boot\bootmgfw.efi`、
`\EFI\Microsoft\Boot\BCD`、`\Boot\BCD`、`\bootmgr`、`\Recovery\WindowsRE\Winre.wim`、
`\Windows\System32\Config\BCD-Template`、`\Windows\Boot\EFI\bootmgfw.efi`、`\Windows\System32\{winload.efi,ntoskrnl.exe,Drivers\stornvme.sys}`。

### 10.2 实验台（一次性 VHD，可销毁）

```bat
:: diskpart：GPT + 300 MB ESP + 19 GB NTFS，动态 VHD
create vdisk file="D:\lab.vhd" maximum=20480 type=expandable
select vdisk file="D:\lab.vhd"
attach vdisk
convert gpt
create partition efi size=300
format quick fs=fat32 label="ESP"
assign letter=X
create partition primary
format quick fs=ntfs label="WIN"
assign letter=W
```

- 挂 ESP 到目录（避免反复分配盘符）：`mountvol D:\lab\esp \\?\Volume{<ESP 卷 GUID>}\`（`volume GUID` 用 `mountvol` 列表取）。
- 我们的工具能识别它：`SysRecover.exe list` → `Disk 2 … GPT / Part 2 FAT32 [ESP]` ✓。
- ⚠️ 本机是 **BIOS 固件**，`SysRecover.exe restore` 对 GPT 目标**按红线拒绝**（`safety.cpp`），所以端到端还原必须在 QEMU/OVMF 或真 UEFI 机上跑；实验室里改用**直接调 `bcdboot`** 复现同一条命令。

### 10.3 逐状态改 BCD + 真启动截图

```bat
bcdedit /store D:\lab\esp\EFI\Microsoft\Boot\BCD /deletevalue {bootmgr} displayorder
bcdedit /store D:\lab\esp\EFI\Microsoft\Boot\BCD /set {default} device partition=Y:      :: 指向另一块 VHD，启动前摘掉
bcdedit /store D:\lab\esp\EFI\Microsoft\Boot\BCD /set {bootmgr} displaybootmenu yes
copy D:\lab\probe\Windows\System32\Config\BCD-Template  D:\lab\esp\EFI\Microsoft\Boot\BCD    :: 模板态
```

截图（`D:\lab\vmshot.py`）：

```
qemu-system-x86_64 -m 1024 -smp 2 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=<msys64>\mingw64\share\qemu\edk2-x86_64-code.fd \
  -drive file=D:\lab.vhd,format=vpc,if=ide,index=0 -display none -no-reboot \
  -monitor tcp:127.0.0.1:5561,server,nowait
# 通过 monitor 发 `screendump x.ppm`（PPM P6 → PNG 转码在脚本里）
```

### 10.4 九个状态的实测矩阵（详表见 §2.3-B）

正常 → 进 winload；格式化目标分区后 → 条目仍解析；ESP 剩 3 MB → `bcdboot` rc=112 且**无 BCD**；
旧 BCD + 失败重跑 → rc=123 但**保留** displayorder；displayorder 删空 → 0xc000000f；模板 BCD → 0xc0000001；
条目删空 → 0xc0000098；设备不存在 → 0xc000000e；`displaybootmenu yes` → 文本菜单正常列出。

**结论**：这 9 种状态都**不能**产生"选择操作系统 + 更改默认值或选择其他选项 + 中间空白"。
即：**静止的 BCD 状态是确定性的** → "有时能进、有时不能"必有别的**变化输入**（见 §11）。

### 10.5 本轮踩到的坑（已记进 §7-P7 与本清单）

| 坑 | 现象 | 规避 |
|---|---|---|
| PowerShell 5.1 解析无 BOM 的 `.ps1` 按 ANSI | 脚本里中文路径全变乱码 → "文件不存在" | 存盘时写 UTF-8 **BOM**（`Set-Content -Encoding UTF8`） |
| `extract --dest X:\` | 报"无法创建输出目录"（`CreateDirectoryW("X:\\")` 失败） | 用已存在的父目录，如 `--dest X:\wf` |
| `bcdedit /set {…} device partition=\Device\HarddiskVolume99` | `The request is not supported` | 造"不可解析设备"要**另挂一块盘再摘掉** |
| `qemu-img`/QEMU 读 VHD | 需显式 `format=vpc` | 直接 `-drive file=…,format=vpc` |
| GUI 进程占用产物 | `make package` 报 `dist\…\SysRecoverUI.exe: Permission denied` | 打包前 `Stop-Process SysRecoverUI` |

---

## 11. 讨论：为什么"有时能进 Windows，有时不能"（间歇性）

### 11.1 先把逻辑说死

- **静止状态是确定性的**：§10.4 证明不同 BCD/ESP 状态 → 固定屏幕。所以"同样的机器，这次能进、下次不能"**不可能**来自同一个静止状态。
- ⇒ 一定存在**每次启动之间会变的东西**。候选只有四类：

| # | 变化的输入 | 为什么能解释"间歇 + 空菜单/失败" | 怎么验证（都不动盘） |
|---|---|---|---|
| **V1（最强）** | **盘本身不稳/坏盘**。他们的现场日志里 **disk0 SMART = "即将故障"**（§2.1） | ESP 与 C 在同一块盘上：偶发读失败 → `bootmgr` 读不全 BCD / 枚举不到条目 → 有时列不出条目（空菜单或直接失败），有时读得全就能进；同一镜像写到不同机器/不同扇区，命中率不同 → 与"镜像故障率高但不恒定"也吻合 | `chkdsk C: /f`、`chkdsk X: /f`；`wmic diskdrive get status`；拿一块**好盘**复现同一镜像 ⬅ **最该先做的对照实验** |
| **V2** | 镜像本身是**热/不一致捕获**（VSS 或不一致快照）。先例：PIT-056（hive 脏 → 日志回放偶然成功/失败） | 首次启动做 hive 恢复/`chkdsk`，结果随上次落盘状态而变 → 有时进得去、有时蓝屏/回滚；"再重启又不行"= 回滚又写了不一致状态 | 解出镜像的 `\Windows\System32\config\{SYSTEM,SOFTWARE}`，读 REGF base block 的 `primary_seq/secondary_seq`（PIT-056 手法）；或对比镜像内文件时间戳/断电痕迹 |
| **V3** | **固件启动项/NVRAM 顺序**在多次启动间变化（我们装过 `Boot####`/`BootNext`，WIT 也会写；某些固件/UEFI 会重排） | 不同的启动项指向不同 `\EFI\…` 路径/不同分区 → 有的能列条目、有的读到另一份 BCD（可能是空的）→ 表现成"时而能进时而不能" | 故障/正常各一次后各跑 `bcdedit /enum firmware`，对比 `BootOrder`/`Boot####` 是否变了 |
| **V4** | 引导期 Windows 自己**会写 ESP**（`\EFI\Microsoft\Boot\BOOTSTAT.DAT`、`BCD.LOG*`/BCD 刷新；Windows Update / 恢复环境也可能重写 `\EFI\Microsoft\Boot\`） | 一次成功启动可能"顺手修好"了 BCD/资源 → 下一次就好了；再之后又被改回/改写 → 又坏 | 两次启动前后 `dir X:\ /s` + `bcdedit /store X:\…\BCD /enum all` 对比（**时间戳 + displayorder 是否变化**） |

> **"重启会不会清掉 ESP 里的垃圾？"** —— **不会**。开机没有任何"清理 ESP"的机制；`\EFI\ZJRESTORE\`、旧版残留、第三方文件都会一直留着。
> 但**引导期 Windows 确实会改写 ESP**（`BOOTSTAT.DAT`、BCD 日志/刷新、更新与恢复环境），所以"重启后表现不同"是**可能**的 —— 那是**改写**，不是"清理"。
> 我们的 33 MB `\EFI\ZJRESTORE\` 也**只在**点「删除启动还原」或重装时才会被清（`RemoveUefiBootEntry`）。

### 11.2 与"镜像决定成败"这个观察的冲突点（必须先排除）

用户观察：**同一台机器，故障镜像 → 出问题；换无故障镜像 → 从此没问题**（且 WIT 与我们的工具都一样）。
这与"盘不稳（V1）"矛盾，因为它指向**镜像内容**；但 §2.3-A 已证明**我们拿到的这份镜像**的 `BCD-Template` 齐全、`bcdboot` 可用。

⚠️ **两个待澄清的事实**（很可能导致"镜像决定"是假象）：
1. **失败现场用的镜像，可能不是我们分析的那一份**：日志里 restore 的镜像是 `…\ESD_ISO_WIM\U盘启动5509重做系统.esd`，而我们分析的是 `C:\20260415Win10备份5509.esd`（同名 5509，**不同文件名/路径**）。
   → 请对**失败现场那份**算 `SHA256` 并发回，和 `C:\20260415Win10备份5509.esd` 比对（`certutil -hashfile <文件> SHA256`）。
2. **WIT 恢复流程**会怎样处理 ESP？如果 WIT 是"整盘写入/自带 ESP 镜像"，它写进 ESP 的 BCD 可能引用的是**做镜像那台 VM 的卷/分区标识** → 到目标机不解解析 → **空菜单**；这样"镜像决定成败"就完全说得通，而且与我们的代码无关（但**我们也能救**：§7-P3/P5）。
   → 请在失败机上确认：ESP 的 `\EFI\Microsoft\Boot\BCD` 是谁写的（时间戳 vs 镜像里文件的时间戳）；以及 WIT 是否有"修复引导/部署 ESP"的选项。

### 11.3 结论（可写进结论页）

1. **确定要修的**：§7 的 P1–P7。即便真因在镜像或硬件，P3（写后断言）+ P1（rc fail-closed）能把"静默失败"变成"明确报错"，P5（`repair-boot`）能让**已经坏的机器一键修好**，P4 让"找不到 ESP"变成可操作的指引。**这是本次讨论唯一确定能落地的收益。**
2. **故障 A 的真因**仍需机器侧数据定死（§6-A）；**间歇性**优先按 §11.1 的 V1/V2/V3/V4 逐一排除，其中 **V1（换好盘复现）成本最低、解释力最强**。
3. **不要**再假设"重启会清 ESP"——文档已证否（§11.1 的引用块）。

### 11.4 2026-09-29 补充证据 + **停止线**（用户裁定）

**新增事实（均为本机只读检查）：**

| 项 | 结论 |
|---|---|
| 失败日志里的镜像 ≠ 我们分析的镜像 | `C:\U盘启动5509重做系统.esd`（**Win10 19045.7417，8.91 GB**，2026-09-11，SHA256 `B20E0CCD…`）与 `C:\20260415Win10备份5509.esd`（19045.3208，1.91 GB，2026-04-15，SHA256 `CE6476B3…`）**是两个不同文件**；用户说明：日志那台机器用的这个镜像是**好的**，那次失败是**机器侧**（ESP 问题） |
| 故障镜像是 **VMware 捕获** | 仅有它含 VMware Tools/驱动：`VMTools`/`vm3dmp*`/`vmci`/`vmhgfs`/`pvscsi`/`vmxnet3ndis6`/`VGAuthService` 等 46 个（+ `hcmon`/`vmrawdsk`/`vmusb`）；且是**精简系统**（服务 568 vs 对照 636） |
| 有无第三方"引导/保护"类软件 | **无**。早先 `DFServ` 命中是**误报**（其实是 Windows 自带 `Services\Dfsc`）；未发现 冰点/影子/云更新/网维/易数/保护卡 等 |
| 镜像 hive 是否"脏"（热捕获） | **都是 clean**（`primary_seq == secondary_seq`：SYSTEM 190/190、SOFTWARE 372/372；对照 116/116、452/452）→ V2 不被支持 |
| 最后一次实验室验证 | 强制 `displaybootmenu yes` + 把唯一条目的设备指向**已摘除**的卷 → bootmgr **仍然列出 "Windows 10"**（点下去才 0xc000000e）→ "设备不存在 ⇒ 条目从列表消失 ⇒ 空菜单"这条**也复现不出来** |

⇒ 本实验室的 **11 种状态（§10.4 的 9 种 + §2.3-B 的两种）都无法造出**"选择操作系统 / 更改默认值或选择其他选项 / 中间空白"。

**停止线（用户 2026-09-29 裁定）：**
> "本来这个问题就确定是镜像的问题……如果能从中发现我们自己身的问题那当然应解决，如果没找到也别乱找。"

- **停止**继续追查故障 A 的镜像侧真因（已足够：镜像侧 = 触发源，用户已用"换好镜像即好"证实）。
- **只做**我们自己的**确证**缺陷：**P1（bcdboot rc 静默）、P2（ESP 空间/顺序/模板预检）、P3（写后断言）、P4（找不到 ESP 的回退与文案）、P5（`repair-boot` 一键修复）、P7（两个小坑）**；P6 作为这些改动的回归。
- 收益定位：**把"静默失败"变"明确报错"，把"已坏的机器"变"一键可修"** —— 不再依赖查出镜像里到底是什么。

---

## 12. 实现与验证记录（2026-09-29）

### 12.1 改了哪些文件

| 文件 | 内容 |
|---|---|
| `src/boot/bcd_parse.{h,cpp}`（**新增**） | 纯逻辑：`BcdEnumShowsOsEntry(enumText, &why)` —— displayorder 非空 + 有 `winload.` 条目 |
| `src/boot/uefi.{h,cpp}` | 新增 `TargetBcdTemplateOk` / `EspHasRoomForBcdboot` / `VerifyEspBcd`；`InstallUefiBootEntry` 的空间余量 **1MB → 16MB**（给 bcdboot 留位） |
| `src/disk/disk.{h,cpp}` | 新增 `FindEspPartitionFallback`（FAT + `\EFI...\bootmgfw.efi`）、`DescribePartitions()`（**纯 ASCII** 分区表摘要） |
| `src/app/ops.{h,cpp}` | 新增 `AcquireEspRoot()`（GUID → mountvol → FAT 回退）；**暂存 UEFI 分支重写**（模板预检 → 空间预检 → **先 bcdboot** → rc fail-closed → `VerifyEspBcd` → 再放 33MB 救援载荷）；**就地 UEFI/BIOS 分支 rc fail-closed + 校验**；新增 `RepairBoot()` |
| `src/cli/main.cpp` | 新增 `repair-boot` 命令 + 帮助 + 总览；**`extract --dest X:\`（盘符根）修复**（P7a） |
| `src/common/progress.{h,cpp}` | 失败收尾不再硬写 `percent=100`，改写"最后一个已知进度"（P7b） |
| `Makefile` / `tests/unit_tests.cpp` | 新源进 `APP_SRC` + `TEST_UNITS`；新增 `bcd_enum_shows_os_entry` 用例（含 7 种真实输出） |
| `tools/i18n-en.py` / `lang/en.lang` | 34 条新/改词条（322 keys 全译） |

### 12.2 已验证（本机，2026-09-29）

| 项 | 结果 |
|---|---|
| `make all`（x64） | ✅ 通过（仅 third_party duilib 既有 warning） |
| `make check` | ✅ **28 cases / 178 checks / 0 failures**；`check-docs` PASS；`check-i18n` **OK（322 keys）**；`check-widths` OK（1 既有 WARN） |
| `--lang en help repair-boot` | ✅ 英文输出正确（说明词条进了词典） |
| `extract --dest W:\`（**分区根**） | ✅ 修复生效：`已提取 1 个路径到 W:\`，文件真落地（此前报"无法创建输出目录"） |
| `repair-boot --disk 9 --part 9`（找不到） | ✅ 报错 + **分区表摘要**，`rc=1`，**未做任何写盘** |
| `repair-boot --disk 2 --part 3`（假 Windows 目录，BIOS 分支） | ✅ **fail-closed**：`修复引导失败（bcdboot rc=193）：尝试复制启动文件失败。`（rc 不再被吞） |

### 12.3 待做（**机器侧回归**，本机是 BIOS+MBR，做不了）

1. **UEFI 分支端到端**：在一台 UEFI 机器（或 UEFI 虚拟机）上验证
   `安装菜单` 与 `repair-boot` 的 **bcdboot→`VerifyEspBcd`** 链路、以及 ESP 只剩 <24MB 时的报错文案。
   - 本机能验证到的是"同一个 bcdboot 在 ESP 上的产物形状"（§2.3-A/B 已量过：bootmgfw + Resources\bootres.dll + Fonts\*_boot.ttf + zh-CN/en-US + BCD 里 `displayorder {default}`）→ `VerifyEspBcd` 的断言与真实产物一致。
2. **故障 B 的回退**：把某个 FAT 分区的类型 GUID 改成 Basic Data（`diskpart set id=... override`）后，`FindEspPartitionFallback` 应能找回（需 UEFI 机器，因为只有 UEFI 才走 ESP 分支）。
3. ~~**`make package` 双架构**：本机 **缺 i686 工具链**~~ → ✅ **2026-09-29 已解决**：装回
   `D:\Prog\ProgIDE\mingw32`（mingw-builds 官方包，与 x64 **同源同版本**：
   `i686-14.2.0-release-posix-dwarf-ucrt-rt_v12-rev0.7z`，88MB，
   <https://github.com/niXman/mingw-builds-binaries/releases/tag/14.2.0-rt_v12-rev0>），
   实测 `i686-posix-dwarf-rev0 14.2.0` / Target `i686-w64-mingw32` / UCRT ✓。
   随后 `make package` **双架构通过**：根目录 = `pei-i386`、`dist\x64` = `pei-x86-64`，
   四个 exe 都带 `requireAdministrator`，`dist\lang\en.lang` 已同步（35142 B / 322 keys）。
   ⚠️ **坑**：跑 `make package` 时必须把 **两个** 工具链的 `bin` 都放进 PATH
   （`D:\Prog\ProgIDE\mingw64\bin` **和** `D:\Prog\ProgIDE\mingw32\bin`）——少了 x86 的，
   `cc1plus` 起不来（缺自身 DLL），**编译"零输出"却报 `Error 1`**，极易误判成源码问题（见 PIT-091）。

### 12.4 未做（明确记录，避免"以后忘了"）
- ~~版本号未 bump~~ → **已 bump 到 `0.4.3`** 并提交（`371875e`）；`make package` 双架构已可跑（装回 i686 工具链后）。
- GUI 入口：**只加了 CLI `repair-boot`**；「安装菜单」/状态栏里的 GUI 一键修复留作后续（CLI 已能被批处理/PE 直接调用，稳定性收益已拿到）。
- 故障 A 的镜像侧真因：按用户 2026-09-29 裁定**停止深挖**（§11.4）。

### 12.5 故障 C：UEFI 固件 + **MBR** 盘被误判成 UEFI 引导（2026-09-29 修）

- **现象**（用户在 Win10 x64 + MBR 分区上实测，旧版 0.4.1；截图 `C:\2222.png`）：
  `暂存失败 / UEFI 机器未找到 ESP 分区，无法部署引导层` —— 明明目标盘是 **MBR**，
  却要求 ESP。0.4.2/0.4.3 只改了文案与 ESP 回退，**判据没变，问题依旧**。
- **根因**：分支判据 `ops.cpp` 的 `IsUefiFirmware()`（`GetFirmwareType()`）只看**平台**是不是
  UEFI，**不看当前是怎么启动的**。"UEFI 平台 + legacy/CSM 引导的 MBR 系统"很常见
  （OEM 预装、克隆盘、GPT→MBR 转换、Legacy 模式下装的系统）——这类机器**没有 ESP**，
  引导链是 legacy `bootmgr` + 实模式 bootsector。
- **修法**（PIT-092）：**按目标磁盘分区风格选链**，不看固件类型。
  - 新增 `src/boot/bootpath.{h,cpp}`：纯逻辑 `ShouldUseUefiBoot(firmwareIsUefi, diskStyle)`
    → **GPT = UEFI/ESP；MBR = BIOS/GRUB4DOS（哪怕固件是 UEFI）；Unknown = 退回固件类型**。
  - `ops.cpp::UseUefiBootFor(target)` 统一替换 4 处判据：就地还原、暂存引导层、"单次启动"
    （`BootNext` vs `bcdsequence`）、`repair-boot`；并新增日志
    `boot path: UEFI/ESP | BIOS/GRUB4DOS (firmware=…, target disk style=…)`。
  - 单测 `boot_path_choice`（6 组组合，含 **UEFI+MBR → BIOS 链**）。`make check` 29 用例 / 184 断言。
- **待做**：真机（UEFI 固件 + MBR 盘）跑一次暂存→重启，确认日志为 `boot path: BIOS/GRUB4DOS`。


---

