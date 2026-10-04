# 17 - 同时支持 WIM / ESD / SNA / TBI 四种镜像格式调研

> 日期：2026-10-03｜状态：调研完成，待拍板｜关联：AGENTS §8（libwim 规范）、§14（许可合规）
> 目标：评估"让九转还原能还原 .wim/.esd/.sna/.tbi 四种既有镜像"的**难度、体积、时间、界面**变化。

## 0. 结论摘要

| 格式 | 引擎 | Linux 救援层还原 | 随包分发许可 | 体积增量 | 开发量 | 还原耗时 | 界面改动 | 结论 |
|---|---|---|---|---|---|---|---|---|
| **WIM** | libwim（LGPL，动态） | ✅ 已有 | ✅ 允许 | 0 | 0 | 基准 | 已有 | **已支持** |
| **ESD**（固态 LZMS WIM） | 同上 | ✅ 已有 | ✅ 允许 | 0 | 0 | 比 wim 慢（固态解压），约 1.2~2× | 已有 | **已支持** |
| **SNA**（Drive Snapshot） | `snapshot.exe`（Win/DOS，闭源商业） | ❌ 无 Linux 引擎；格式不公开、无开源解析器 | ❌ 未经协议不得分发 | 0（若桥接） | 桥接 2~4 周；原生不可行 | = 转换一遍 + 还原一遍 | 文件过滤 + 转换提示 | **不建议原生；可桥接** |
| **TBI**（TeraByte Image） | Image for Linux `imagel`（商业，**需 32 位 glibc**）；Windows 端 TBIMount | ⚠️ 有 Linux 版但受许可/兼容限制 | ❌ 需付费 OEM/Deployment 许可 | 原生 +30~60MB；桥接 0 | 原生 2~3 月 + 许可谈判；桥接 2~4 周 | 原生扇区级（较快）；桥接 = 两遍 | 同上 | **不建议原生；可桥接** |

**一句话**：wim/esd 已经全支持；sna/tbi 没有"免费 + 可分发的 Linux 还原引擎"，卡死点是**许可 + 引擎**（不是界面）。
现实路线只有两条：**① 桥接**——检测到 sna/tbi 时，调用用户自己已安装的原厂工具把镜像挂载成盘，我们用现有引擎把它**捕获成 WIM** 再走正常 Linux 还原（不复带、不分发原厂二进制）；**② 不集成**——提示用户先用原厂工具还原、再用我们备份 WIM。

## 1. 前提与硬约束

- 我们的还原**在 Linux 救援层执行**（AGENTS §2 禁令 1：重启类还原禁用 WinPE/WinRE）。
- 目标体积 < 10MB、零运行时依赖；既有许可全部允许随包分发（LGPL/BSD/独立聚合）。
- "支持"指**能还原既有镜像**；四种格式的**备份创建**不现实（sna/tbi 的写入同样只有原厂引擎），备份仍只做 wim/esd。

## 2. WIM / ESD（现状，无需改动）

- 引擎 `libwim`：Windows 侧动态链接 `libwim-15.dll`（LGPLv3）；救援层静态 `wimlib-imagex`。
- ESD = **固态 LZMS 压缩的 WIM**（子镜像共压缩），wimlib 读写均支持；我们的备份格式下拉里 `.esd` 就是它。
- 兼容性：wimlib 能读微软 `install.esd`、DISM 制作的 WIM/ESD、我们自己的产物；LZX/LZMS/XPRESS 均支持。
- **唯一代价**：ESD 解压比普通 wim 慢（固态解压需按块序解码），属预期，不是障碍。

## 3. SNA（Drive Snapshot）

事实（官方文档）：

- `snapshot.exe` 为 **Windows + DOS** 工具，单文件（约 3MB 安装空间），商业试用版；**"Restoring a system partition will require DOS or Windows PE"**。
- 命令行：`snapshot x:\image.sna D:`（还原到分区）、`--EntireDisk`（整盘）、`--schedule C: image.sna`（计划重启还原）、`--RestorePartitionStructure` 等。
- Linux 侧：**没有官方还原工具**；.sna 格式**未公开**；社区/开源无可靠解析器（无规格 → 自研等于逆向，法律与工程风险都不可控）。

可行方案与代价：

| 方案 | 说明 | 成本 |
|---|---|---|
| a. 自研 Linux 解析/直写 | 无公开规格，需逆向 | **不可行**（法律 + 工作量） |
| b. Windows 桥接（推荐如有需求） | 要求用户已装正版 Drive Snapshot → `snapshot image.sna Z:` 挂载为虚拟盘 → 我们的引擎把 Z: 捕获成 WIM（VSS/快照）→ 正常 Linux 还原 | 开发 2~4 周；依赖用户环境；临时空间=镜像解包大小；耗时=转换+还原两遍 |
| c. 不集成 | 提示用户在原厂/PE 自己还原后，再用我们备份 WIM | 0（推荐短期） |

许可：未经协议**不得**随包分发 `snapshot.exe`（很多 PE 里塞它是侵权做法，不能学）。

## 4. TBI（TeraByte Image）

事实（官方文档）：

- **Image for Linux（`imagel`）** 是 CUI/GUI 的 Linux 程序，可创建/还原/校验 .tbi；同版本 .tbi 在各产品间兼容。
- **64 位 Linux 发行版上运行 IFL 需要安装 32 位库**（即 imagel 是 32 位 glibc 动态程序）——我们的救援层是 Alpine/musl x86_64，要跑它得额外带整套 32 位 glibc 用户态。
- **TBIMount**：Linux 端靠 `tbimount.ko` 内核模块把 .tbi 里的分区挂出来——**模块必须匹配内核**，我们的 Debian 内核加载不了它的 .ko；Windows 端 TBIMount 是驱动（可用）。
- 免费工具：**TBIView** 只能"看/提取文件"；**OSD Tool Suite 标准版"个人免费"**（含 tbidtool/copyp2v 在 Pro 版）；**商业分发**需购买 **OEM Recovery Media / Image Deployment** 等收费许可。
- 格式：**无公开规格**；无开源解析器（archive 组织也只登记了 TBIView 这个闭源 reader）。

可行方案与代价：

| 方案 | 说明 | 成本 |
|---|---|---|
| a. Linux 原生捆绑 `imagel` | 需付费 OEM 许可 + 在 musl 环境里塞 32 位 glibc（+30~60MB initramfs）+ 解析其进度/错误 | 许可谈判 + 2~3 个月；与 <10MB 目标冲突 |
| b. Windows 桥接 | 用户已装 TBI 套件 → TBIMount 把 .tbi 挂成盘 → 我们捕获成 WIM → 正常还原 | 开发 2~4 周；依赖用户环境；两遍耗时 |
| c. 不集成 | 让用户先用原厂还原再备份 WIM | 0（推荐短期） |

## 5. 若做"桥接"，界面/流程要动的地方（两格式共用）

1. 打开文件过滤：`*.wim;*.esd;*.sna;*.tbi`；镜像列表加"格式"列。
2. 选中 sna/tbi 时：隐藏"子镜像"下拉（它们无子镜像）；显示"来源分区/整盘信息"（需要各自挂载器读元数据——桥接方案下由原厂工具提供）。
3. 新增**转换阶段**：状态栏"正在把 <镜像> 转换为 WIM（需原厂工具）…"，进度复用现有回调；临时空间检查（目标盘剩余 ≥ 镜像解包大小）。
4. 前置检查：检测原厂工具是否已安装/已授权；未装 → 明确提示（并给官网链接），fail-closed。
5. 安全边界：
   - 整盘/raw 镜像（`--EntireDisk`、`--raw`）**不支持**——我们只格式化目标分区、不动分区表，必须明确拒绝并解释。
   - sna/tbi 的"分区还原"语义与我们的目标分区选择对齐（按分区大小预检）。
6. 文档/帮助：新增"如何把 SNA/TBI 用原厂工具转成 WIM"的傻瓜步骤（替代集成）。

## 6. 汇总：工作量 / 体积 / 时间 / 界面

| 项 | WIM/ESD | SNA 桥接 | TBI 桥接 | TBI Linux 原生 |
|---|---|---|---|---|
| 开发工期 | 0 | 2~4 周 | 2~4 周 | 2~3 月 + 许可 |
| 包体积增量 | 0 | ≈0 | ≈0 | +30~60MB |
| 单次还原耗时 | 基准 | 转换 + 还原（约 2×） | 转换 + 还原（约 2×） | ≈扇区级直还原（快） |
| 界面改动 | 0 | 中（见 §5） | 中 | 中 |
| 许可风险 | 无 | 低（用户自装） | 低（用户自装） | **高（付费 OEM）** |
| 依赖 | 无 | 用户装 Drive Snapshot | 用户装 TeraByte 套件 | imagel + 32 位 glibc |

## 7. 建议

1. **短期（0 成本，推荐）**：不做 sna/tbi 原生支持；在文档/FAQ 给"用原厂工具还原→再用九转还原备份为 WIM"的转换指南。
2. **中期（若确有客户量）**：做 **Windows 桥接转换**，优先级 TBI（Windows 端 TBIMount 常见）> SNA；**绝不复带原厂二进制**，只做检测 + 调用 + 转 WIM。
3. **不做**：自研 .sna/.tbi 解析（无规格）；Linux 原生捆绑 imagel（许可 + musl/glibc + 体积三重坑）。
4. 若老板要"不管许可先塞进去"→ 明确风险：闭源商业软件随包分发 = 侵权，且与项目"只用可合法分发组件"的红线冲突。

## 8. 来源

- Drive Snapshot：命令行 <http://www.drivesnapshot.de/en/icommandline.htm>；DOS 还原 <http://drivesnapshot.de/en/irestdos.htm>；产品概览 <http://drivesnapshot.de/en/iindex.htm>
- TeraByte：IFL 手册 <https://www.terabyteunlimited.com/downloads/ifl_manual.pdf>；购买/许可（Deployment/OEM）<https://www.terabyteunlimited.com/purchase-image-for-dos-linux/>；TBIMount/下载 <https://www.terabyteunlimited.com/downloads-image-for-linux/>；OSD 套件 <https://www.terabyteunlimited.com/tbosdt/>
- 格式登记：TeraByte Image <http://fileformats.archiveteam.org/wiki/TeraByte_Image>
