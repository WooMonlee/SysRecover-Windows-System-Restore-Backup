# SysRecover · 开源同类调研：Clonezilla / Rescuezilla / FOG Project

> 调研时间：**2026-09-21**（版本/特性随时会变，对外引用前请按 §6 复核）
> 目的：找出它们比我们**先进**的地方，判断哪些**值得学**、哪些**值得融**、哪些**不适用**。
> 前提：三家**全部是 GPL 系开源** → 对我们的"可闭源商用"目标意味着**不能并代码**（结论见 §5）。

---

## 1. 一句话定位

| 项目 | 本质 | 适合谁 |
|---|---|---|
| **Clonezilla** | 块级磁盘/分区克隆（partclone），业界事实标准，**离线**（U 盘/PXE 启动）| 会命令行的运维、批量场景 |
| **Rescuezilla** | **Clonezilla 的图形前端**（完全互操作），Ubuntu 底座，**离线**（Live USB）| 普通用户、要图形界面的人 |
| **FOG Project** | **网络部署与管理系统**（PXE + 组播 + Web UI + 客户端代理）| 学校/企业机房、批量装机 |

> 我们（SysRecover）：**系统内一键还原**（Windows 里备份/还原，可重启进内置 Linux 救援层），
> 主打"单机、免 U 盘、免关 Secure Boot、还原后能开机"。

---

## 2. 各自的亮点（2026-09 现状）

### Clonezilla Live 3.3.3-37（2026-09 发布）
- **块级成像**：`partclone` 0.3.50（默认 **XXH128** 校验）；`dd`/`ntfsclone` 兜底；**整盘 / 多分区 / 任意文件系统**（ext4/xfs/btrfs/…）。
- **镜像加密**：改用 **gocryptfs**（eCryptFS 已弃用），新增 `ocs-cvtimg-enc` 转换工具。
- **MDRAID**：新增布局保存/恢复（`ocs-save-mdraid-layout` / `ocs-restore-mdraid-layout`）→ 阵列机器还原后可复原阵列布局。
- **网络部署**：**multicast + BitTorrent**（`ocs-live-feed-img`）；**Clonezilla Lite Server** 支持 **PXE 与 HTTP Boot**，且 **HTTP Boot 下支持 Secure Boot**。
- **设备对设备**：`ocs-onthefly`（含"反向连接"网络克隆）；可跳过分区表（`-sspt`）。
- 其他：LUKS2 仓库、MTD/eMMC 启动设备、Debian Sid 底座（内核 7.1）。

### Rescuezilla 2.6.2（2026-05 发布）
- **Clonezilla 的 GUI**，且**完全互操作**（两边镜像可互还原）→ "不锁死用户"的产品哲学。
- 基于 **Ubuntu 26.04 LTS**（新硬件支持好），Live USB 即用，**无需安装**。
- **Image Explorer（beta）**：**挂载镜像、直接提取文件**（基于 `partclone-nbd`）；
  ⚠️ 官方说明：对 gzip/zstd 压缩的大镜像会**很慢**（>50GB 需要等它做索引化访问）。
- 支持 **VM 镜像格式**：VDI / VMDK / VHDx / QCOW2 / raw（经 `qemu-nbd`）。
- 支持 **md RAID、LVM、无分区表** 等高级环境；有**实验性 CLI**。
- 附带一堆救援工具（分区、反删除、文件浏览器、浏览器）。

### FOG Project（1.6 线）
- **完全网络化**：PXE/iPXE + TFTP + HTTP 分发，**没有 U 盘/CD**；内核里带大量网卡驱动。
- **组播（UDPCast）**：官方原话"1 台和 20 台的耗时差不多" → 机房装机的核心价值。
- **Web 管理台**：主机 / 主机组 / 任务队列 / 镜像库 / 存储节点（分布式）。
- **Snapins**：部署后在客户端**跑脚本/装软件**；**FOG Client**（Windows 服务）能**改名、入域**、管打印机、空闲关机、用户追踪。
- 资产清单（inventory）、擦盘、坏道扫描、反删除。
- "Capone" 插件：类似硬件的机器"快速镜像"，最少交互。

---

## 3. 与我们对照

| 维度 | **SysRecover** | Clonezilla | Rescuezilla | FOG |
|---|---|---|---|---|
| 成像层级 | **文件级**（WIM/ESD，wimlib）| 块级（partclone）| 块级 | 块级 |
| **备份运行中的系统** | ✅ **VSS 热备，不重启** | ❌ 必须离线 | ❌ 离线 | ❌ 离线 |
| **从 Windows 里一键还原** | ✅（暂存+重启 / 就地不重启）| ❌ 要 U 盘/PXE 启动 | ❌ | ❌ 要 PXE |
| 多镜像 / 去重 | ✅ WIM 多子镜像 + 去重 | ❌ 每次一份完整镜像 | ❌ | 一镜像一份 |
| 追加/增量 | ✅ `--append` | ❌ | ❌ | ❌ |
| **网络批量（组播）** | ❌ **完全没有** | ✅ multicast + BT | ❌ | ✅✅ UDPCast + 任务队列 |
| PXE / HTTP 网络启动 | ❌ | ✅（Lite Server + HTTP Boot + SB）| ❌ | ✅✅ 核心能力 |
| 整盘 / 任意文件系统 | ❌ 只做 NTFS 系统分区 | ✅✅ | ✅ | ✅ |
| **镜像内容浏览 / 单文件提取** | ⚠️ 只有子镜像列表（wimlib 其实支持，我们没做）| ⚠️ 靠挂载脚本 | ✅✅ Image Explorer（GUI）| ❌ |
| 镜像加密 | ❌ | ✅ gocryptfs | ❌ | ❌ |
| MDRAID / LVM 布局 | ⚠️ 驱动有，但不管理布局 | ✅ 布局保存/恢复 | ✅ | ✅ |
| **还原后修 Windows 引导** | ✅✅ 自动（PBR + BCD + 便携 BCD）| ❌ 要自己修 | ❌ | ⚠️ 靠脚本 |
| 交付体积 | ✅ Windows 侧 3.77MB + 内置救援 | ISO ~350MB+ | ISO ~1GB+ | 服务器 |
| 单机无人值守 | ✅ CLI + 静默 + 退出码 | ✅ 命令行 | ⚠️ CLI 实验性 | ⚠️ 要服务器 |
| 许可 | 自有（可闭源）| GPL | GPL | GPL |

**一句话总结差距**：它们强在**"机器已经起不来时也能救"（离线介质 / 网络启动）**和**"一次装一批"（组播）**；
我们强在**"机器还能用时，不用 U 盘、不用关 Secure Boot、不用人工修引导"**和**单机静默**。

---

## 4. 值得借鉴的（按性价比排序）

### ★★★ 立刻值得做
1. **可启动救援 ISO / U 盘** —— 我们现在的救援层是"部署到目标盘"的，**依赖原机还能装/还能引导**；
   系统已经挂了就无解。而 `bootfiles/` 里内核+initramfs+脚本已经齐了，**做成可启动 ISO/U 盘成本很低**，
   能覆盖"裸机/系统已毁"的场景（这也是 Clonezilla/Rescuezilla 的主场）。
2. **镜像内容浏览 / 单文件提取** —— Rescuezilla 的 Image Explorer 是它最受欢迎的功能之一；
   **wimlib 原生支持**列目录与提取单文件 → 我们加个 `browse`/`extract`（CLI + GUI 树）即可，
   客户价值高（"还原前先看看里面有什么 / 只捞一个文件出来"）。

### ★★ 视需求做
3. **网络取镜像 → 组播部署**：救援层加 `wget/curl`（busybox 自带）先支持"从网络下载镜像"，
   再考虑组播（需引入 `udpcast` 之类；工程量与许可都要评估）。**这是 FOG 的核心价值**，但对"单机版"不是刚需。
4. **镜像加密**：Clonezilla 转向 gocryptfs 的思路可借鉴（wimlib 本身不加密，需在外层做）。
5. **整盘完整性**：备份时把 **ESP 的引导文件**也纳入镜像（我们现在靠 `bcdboot` 重建 ESP）→ 裸机还原更"原样"。
6. **MDRAID / LVM 布局保存恢复**：服务器场景有用（我们的救援层已有驱动，缺的是"布局"这一层）。

### ★ 记下来，不急着做
7. **"不锁死用户"的产品哲学**（Rescuezilla 兼容 Clonezilla 镜像）：值得我们思考——
   要不要支持**导入别人的镜像**（如 partclone 格式）？涉及 GPL 组件的调用/分发，需单独评估。
8. **机房管理那一套**（FOG 的主机/组/任务/Snapin/清单）：更像**我们另一条产品线**（OnekeyRestore，面向机房）该学的，不是单机版的重点。

---

## 5. 能不能"融合"？（许可结论，务必先看）

- 三家**都是 GPL 系**（Clonezilla / Rescuezilla / FOG）→ **不能**把它们的代码链接进我们的产品、
  也不能抄进源码（`AGENTS.md` §2 红线 / §14 SBOM）。这会直接毁掉我们"可闭源商用"的立场。
- **可以做的**：
  1. **学思路/学设计**（设计思想不受版权保护）——本文件 §4 就是干这个；
  2. **像 grldr 那样"单独分发"**：把它们的**未修改二进制**作为**独立聚合**随包/随站点提供
     （需附其许可全文与源码获取方式，且不与我们链接）；
  3. **调用其命令行**（进程调用 ≠ 链接）。
- **建议**：**学思路、不并代码**。真需要"整盘 / 多系统 / 任意文件系统 / 组播"时，
  优先考虑"**单独分发并调用 Clonezilla**"的可选"深度模式"，而不是把 GPL 代码并进来。

---

## 6. 复核清单（对外引用前）

| 要确认 | 去哪 |
|---|---|
| Clonezilla 最新版本与特性（multicast/HTTP Boot/SB/加密）| `clonezilla.org` changelog / release notes |
| Rescuezilla 是否已实现"索引化访问"（大镜像挂载提速）| 其 GitHub release / roadmap |
| FOG 的 Windows 11 支持与 Secure Boot PXE 现状 | `docs.fogproject.org` |
| 各家许可与分发义务 | 各自仓库 LICENSE |
| 我们自己的对应能力（避免自夸）| `docs/07`（实测）、`PLAN.md` §12（待办）|
