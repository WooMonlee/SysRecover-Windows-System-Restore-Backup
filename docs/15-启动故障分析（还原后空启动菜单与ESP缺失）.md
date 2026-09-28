# docs/15 · 启动故障分析（还原后空启动菜单 & 未找到 ESP）

> 状态：**待取证**（2026-09-28 记录，取证回来后本文第 6、7 节定稿）。
> 本文用于**防止忘记**当前的搜索结论与分析，取证后据此推进修复。
> 关联版本：`0.4.1`（`a89b228`）。

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

---

## 3. 代码定位（本仓库，行号基于 `a89b228`）

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
   dir X:\ > esp-tree.txt
   fsutil volume diskfree X: > esp-free.txt
   ```
   把 `bcd.txt` / `esp-tree.txt` / `esp-free.txt` 拷回来。
   - 看点：`{bootmgr}` 的 `displayorder` 是空的、还是指向不存在的 GUID；`displaybootmenu`/`timeout` 是什么；
     `\EFI\Microsoft\Boot\` 下**有没有 `en-US\`（或 `zh-CN\`）locale 目录**（"写一半"的特征）；ESP 剩余空间。
2. 把 `20260415Win10备份5509.esd` 拷回来，我用 `libwim` 只读列出：
   `\Windows\System32\Config\BCD-Template` 是否存在、多大；
   顺带看 `\Windows\System32\Boot\`、`\Windows\boot\efi\`。
   （**只读列目录，不写任何东西。**）
3. （可选）同一台机上把好镜像 `20260911…LTSC….esd` 也列一遍做对照。
4. （可选）PE 里"再处理"到底点的是什么工具/命令 —— 如果是 `bootrec /rebuildbcd`，那在 UEFI 上本来就无效，等于没修过。

### 6-B · 故障 B（DiskGenius 重建分区的机器）

1. `diskpart` → `list disk` → `list part`（或 DiskGenius 分区属性截图），
   **重点看前两个分区的类型/卷标/文件系统**，以及是否有 MSR、ESP。
2. 挂一下 ESP（若还认得出）→ `dir X:\`，看有没有 `\EFI\Microsoft\Boot\bootmgfw.efi`。
3. `C:\_zjresy*.log`（如果有）—— 我们失败前写的那份契约，能确认当时的目标 offset/size。
4. 顺带：那块盘 SMART 已报"即将故障"，**建议先换盘再排障**，否则取证结果可能被坏盘污染。

---

## 7. 拟定修复项（**待取证后定稿，未动代码**）

| # | 改动 | 针对 | 备注 |
|---|---|---|---|
| **P1** | `ops.cpp:749` 的 `bcdboot` **返回码 fail-closed**：`brc != 0` → 中止暂存并把 `out` 里的 BFSVC 错误原文给用户 | H3 | 最小改动、收益确定 |
| **P2** | 暂存前**预检**：① 目标分区 `Windows\System32\Config\BCD-Template` 存在且 > 0；② 挂 ESP 后算**剩余空间**（我们的 33 MB + bcdboot 需要的余量），不够就明确报错（并提示先清 `\EFI\ZJRESTORE` 旧版本残留） | H1 / H2 | 只在 `IsUefiFirmware()` 分支做 |
| **P3** | **写完 ESP BCD 后校验**：`bcdedit /store <ESP>\EFI\Microsoft\Boot\BCD /enum all` → 断言 `displayorder` 非空且含至少一个 osloader；不满足 → fail-closed | H1/H2 的兜底 | **能直接拦住"空菜单"这个症状**，不管根因是什么 |
| **P4** | `FindEspPartition` 失败时**回退检测**：找「FAT32 分区 && 含 `\EFI\Microsoft\Boot\bootmgfw.efi`」的分区；找到但 GUID 不对 → 提示"该分区不是 ESP 类型，需改分区类型（`set id=c12a7328-…`）"；真找不到 → 提示"ESP 分区缺失/磁盘健康异常" | 故障 B | 报错文案要能指导下一步 |
| P5（次要） | `progress.json` 失败时 `percent` 仍为 100 | — | 顺手修 |
| P6（待定） | 若 H1 成立：**`BCD-Template` 损坏时的兜底来源**（随包带一份已知好的模板？还是提示用户先修镜像？） | H1 | **代价与合规都要再想**，取证后再定 |

**原则**：这些检查全部落在**暂存阶段**（还没动目标分区之前），符合项目"fail-closed、先自检再动手"的既有红线（§11）。

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

1. 你按 §6 取证 → 拷回 `C:\logs\`（或任意目录，告诉我路径）。
2. 我据此把 §5 假设定死、§7 方案定稿，再动代码。
3. 代码改动落地后：`make check` → `make package` → 你在受影响机器回归（**故障 A 看 `bcd.txt` 的 `displayorder`、故障 B 看报错文案是否指到点上**）。
