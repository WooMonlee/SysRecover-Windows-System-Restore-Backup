#pragma once
// 备份/还原共享操作层：CLI 与 GUI 共用单一实现（AGENTS.md §4 模块职责）。
// 语义对齐旧 C# BackupOrchestrator/RestoreOrchestrator（已冻结契约）。
#include <string>

#include "../disk/disk.h"
#include "../wim/wim.h"
#include "advice.h"  // ErrAdvice / ErrorAdvice（M1 从本文件迁到 advice.h，留在命名空间同处）

namespace sysrecover {

struct BackupRequest {
    std::wstring source = L"C:/";  // 盘符根触发热备（snapshot+排除配置）
    std::wstring dest;             // 目标 WIM/ESD 全路径（必填）
    std::string compress = "fast"; // fast | maximum | recovery
    std::wstring name = L"Backup"; // 子镜像名
    bool append = false;           // true=追加到已有 WIM
    bool verify = false;           // true=写完后跑 wimlib_verify_wim
    bool snapshot = false;         // true=强制 VSS 快照（非盘符根也可用，如活动数据库目录）
    // true=**禁止** VSS 快照（冷备）：盘符根默认走 VSS 热备，但 PE / 离线卷 /
    // 挂载的 VHD 卷上 VSS 不可用（rc=89）→ 显式冷备（CLI --no-snapshot）。
    bool noSnapshot = false;
    int  cpuCap = 0;               // CPU 硬上限百分比（0=不限；1..100，见 common/cpucap.h）
    // true=把 ESP 分区并入主镜像（方案 C，用户 2026-09-30 规格 / 无忧 66 楼）：
    // ESP 作为同一文件里的子镜像（name="ESP"）一起写出；还原时由暂存契约
    // esp_index 定位、救援层恢复到 ESP 分区。无 ESP / 失败不致命（见 RunBackup：
    // 主镜像经 <dest>.stage 原子落盘，ESP 失败不损坏主镜像）。
    bool esp = false;
};

struct RestoreRequest {
    std::wstring image;  // WIM 全路径
    int disk = 0;        // 目标磁盘号
    int part = 0;        // 目标分区号（1-based）
    int index = 1;       // 子镜像（1-based）
    bool repairBoot = true;
    // 还原引擎（0.7，docs/21/22/23）：""/"block" = B 引擎（Linux apply）；
    // "swap" = A 引擎（Windows 原生解压到 <目标>:\~new + Linux 仅改名交换）。
    std::string engine;
    // 备返（用户 2026-10-09 规格）：true = 交换后保留 ~old，往新系统桌面
    // 放管理 GUI 快捷方式 + 注入 RunOnce 让新系统**首启自动运行本程序**
    //（用户 2026-10-17 规格：先搬日志回程序目录，再弹「是否删除旧系统」）；
    // false = 开机 RunOnce 静默删 ~old。
    bool keepOld = false;
};

// 备份。覆盖确认由调用方负责（CLI --yes / GUI 对话框）。
// 返回：0 成功；1 失败（err=原因 UTF-8）。
// adv（可选）：失败时给出**建议码**，供 ErrorAdvice 取"下一步怎么办"
//（M1：不再靠中文关键词猜，见 advice.h）。
int RunBackup(const BackupRequest& req, ProgressFn progress, std::string& err,
              ErrAdvice* adv = nullptr);

// 就地还原不可行的原因码（M1：替代 GUI 里 `why.find("系统盘")` 这种中文子串匹配，
// 否则消息一翻译判定就失效）。文案仍由调用方决定（各自语言各自渲染）。
enum InPlaceReason {
    INPLACE_OK = 0,       // 可就地还原（不重启）
    INPLACE_NO_LETTER,    // 目标无盘符
    INPLACE_SYSTEM_DISK,  // 目标是正在运行的系统盘 → 必须重启脱机还原
    INPLACE_LOCKED,       // 卷被占用（锁不上）→ 必须重启
    INPLACE_OPEN_FAIL,    // 打开目标卷失败
    INPLACE_PE,           // PE 中放行
};

// 目标分区是否可以**就地还原**（不重启）？
// 供调用方（GUI 的确认框文案、CLI 的提示）决定"要不要说重启"——
// StageRestore 内部调用的是同一个函数，两处不会漂移（PIT-072）。
// 判据：目标不是正在运行的系统盘，且能对目标卷加独占锁（FSCTL_LOCK_VOLUME）。
bool CanRestoreInPlace(const PartitionInfo& target, std::string& why,
                       InPlaceReason* reason = nullptr);

// 给常见失败配一句"下一步怎么办"（P8）。CLI 打印、GUI 弹窗都会附上它。
// 实现已迁到 advice.h / advice.cpp（**按建议码取词**，不再匹配消息文本）。

// 还原前**空间预检**（P1）：镜像的**未压缩**内容大小 vs 目标分区大小。
// 必须在"格式化之前"拦下，否则会出现"数据没了、系统也没装上"（先格式化再 apply）。
// 返回：0 = 够（或无法判定 → 不阻断，让流程继续）；1 = 空间不足（err=给用户看的说明）。
int CheckRestoreSpace(const std::wstring& imagePath, int index,
                      const PartitionInfo& target, std::string& err);

// 还原。**两种执行方式自动选择**（PIT-064）：//   * 目标分区**没被占用**时（在 PE 里、或目标是别的分区）→ 就地还原：格式化 +
//     apply + 修引导，**不重启**（needReboot=false）；
//   * 目标被占用（还原正在运行的系统盘）→ 暂存任务 + 重启进 Linux 救援层执行
//     （§2 禁令1），needReboot=true。
// 返回：0 成功；1 失败（err=原因）；4 安全门禁拒绝（err=理由）。
// adv（可选）：失败时给出建议码（空间不足/半截镜像/镜像在目标分区内/权限…）。
int StageRestore(const RestoreRequest& req, std::string& err,
                 bool* needReboot = nullptr, ProgressFn progress = nullptr,
                 ErrAdvice* adv = nullptr);

// 一键重试（P3 / PIT-148，用户 2026-10-14 规格）：从 <exeDir>\restore-task.conf
// 重建 RestoreRequest（按 target_offset/size/serial 匹配当前磁盘表）并重新
// StageRestore。**只用于 A/B 类失败**（未动目标 / 引导未验证，判定见
// common/rescue_decision.h::RescueStepRetryable）—— C 类（mkntfs/apply 已动盘）
// 救援层已自动重试 ×2，此处 fail-closed 不提供。
// keep_old 不在 conf 原设计里（PIT-148 起新增），老 conf 缺键按 false。
// 返回：0 成功（*needReboot 由 StageRestore 决定）；1 失败（err=原因）；
//       2 conf 缺失/action 非 restore（err=说明）；4 找不到目标盘（err=说明）。
int RetryStagedRestore(std::string& err, bool* needReboot = nullptr);

// 安装/更新「一键还原菜单项」（用户 2026-09-29 规格）：
//   把当前选中的 **镜像 + 目标分区** 写成**常驻任务契约**（目标分区根 `_zjresy*.log`
//   + 软件目录 `restore-task.conf`），并部署救援环境 + **常驻**启动项
//   （UEFI 写固件启动项、BIOS 写 BCD 实模式扇区项）。
//   与 `StageRestore` 的区别：**不重启、不设单次启动**（不写 BootNext / bootsequence），
//   留给用户在开机菜单里自行选择；进入救援层后按契约把镜像还原到目标分区。
//   与 `StageRestore` 共用同一套安全检查与契约写入（只是模式不同），不会漂移。
// 返回：0 成功；1 失败；2 参数错（缺镜像 / 目标无盘符）；4 危险目标被拒；5 镜像不可用。
int StageRestoreMenu(const RestoreRequest& req, std::string& err,
                     ErrAdvice* adv = nullptr);

// ── 返回旧系统（PIT-139，2026-10-13，方案 C）─────────────────────────
// A 引擎 keepOld 备返的撤销。**必须重启进 Linux 救援层完成目录反向交换** ——
// 在 Windows 里改名正在运行的系统树（根 -> ~new、~old -> 根）不可回滚，
// 救援层里没有进程占用，纯同卷改名、秒级、可中断续做（见 docs/23 §5）。
// Windows 侧只做：暂存契约(action=returnold) + 无条件部署引导层 + 单次启动 +
// 写待执行标记 + 返回 needReboot=true（调用方负责 shutdown /r）。
// 前置：目标根有完整旧系统 `~old\Windows` 且根有完整新系统 `\Windows`；
//       软件目录不得位于目标盘（否则程序会把自己搬走）。
// 返回：0 成功；1 失败（err=原因）；4 前置不满足（err=说明，此时应让用户
//       改选「删除旧系统」，或先在别处运行本程序）。
int StageReturnOld(const PartitionInfo& target, std::string& err,
                   bool* needReboot = nullptr);

// 目标根是否具备「返回旧系统」的条件（纯只读探测，不写任何东西）。
// 供 GUI/CLI 决定按钮态与确认框文案；与 StageReturnOld 内部同一判据，不会漂移。
bool DetectReturnOldCapability(const PartitionInfo& target, bool* hasOldTree,
                               bool* hasRootTree, std::string& detail);

// 便捷入口（CLI）：按磁盘号/分区号定位后调 StageReturnOld。
// disk/part 都 < 0 时自动找「根有 \Windows 且 ~old\Windows 也在」的第一块固定盘。
int StageReturnOldByDiskPart(int disk, int part, std::string& err,
                             bool* needReboot = nullptr);

// 读取"已安装菜单项"绑定的镜像/目标（供 GUI 显示与"是否与当前选择一致"判断）。
// 数据源：<exeDir>\restore-task.conf（StageRestore/StageRestoreMenu 写的副契约）。
// 返回 false = 没有绑定信息（未安装，或软件目录不是可写位置）。
bool ReadMenuBinding(std::wstring* imagePath, int* imageIndex,
                     unsigned long long* targetOffset,
                     unsigned long long* targetSize);

// 删除"菜单绑定"副契约（<exeDir>\restore-task.conf|json）：「删除菜单 / 清除
// 引导项」时必须一起删 —— 否则 ReadMenuBinding 仍判"已安装"，按钮回不到
// 「安装菜单」（用户 2026-10-05 实测 bug）。
void DeleteMenuBinding();

// ── A 引擎（0.7）空间预检（用户 2026-10-09 规格，docs/22 §1.1）────────
// 需要空闲 ≥ 解压内容量 + pagefile.sys + swapfile.sys + hiberfil.sys
//            + 余量（内容×10% + 2GB）。
// 策略：够 → 直接开始（不问）；临界/不确定 → 上层弹一次让用户确认。
struct SwapSpaceEstimate {
    unsigned long long content = 0;    // 镜像未压缩内容量（调用方传入）
    unsigned long long pagefile = 0;   // 目标盘当前实际大小（读不到按 0）
    unsigned long long swapfile = 0;
    unsigned long long hiberfil = 0;
    unsigned long long slack = 0;      // 内容×10% + 2GB
    unsigned long long required = 0;   // 合计
    unsigned long long freeBytes = 0;  // 目标盘当前空闲
    bool enough = false;               // free >= required
    bool certain = true;               // 空闲值读取成功（false=让用户确认）
};
bool EstimateSwapSpace(const PartitionInfo& target,
                       unsigned long long imageContentBytes,
                       SwapSpaceEstimate& out);

// 本次还原/装菜单该走哪套引导链（PIT-092）：UEFI 固件 **且** 目标盘 GPT →
// UEFI/ESP；否则（含"UEFI 固件 + MBR 盘"）BIOS/GRUB4DOS。GUI 用它给
// "安装菜单成功"的提示选对说法（UEFI=开机按 F12 选固件启动项；
// BIOS=开机菜单里选 SysRecover 条目）—— 与 StageRestoreImpl 内部同一函数，
// 不会漂移（PIT-072 同款做法）。
bool UseUefiBootFor(const PartitionInfo& target);

// 修复引导（docs/15 · P5）：给**已经坏的机器**用 —— 不重装即可修好引导。
//   disk/part <= 0 → 自动找系统盘（含 \Windows\System32\winload.exe 的分区）；
//   否则用指定的 磁盘号/分区号（分区必须有盘符）。
// 流程：定位系统盘 → 找 ESP（GUID → mountvol → FAT 回退）→ 模板/空间预检 →
//       `bcdboot <系统盘>:\Windows /s <ESP>: /f UEFI`（BIOS 走 /f BIOS）→ 产物断言。
// 返回：0 成功；1 失败（msg 为 UTF-8，含分区表摘要等诊断）。
int RepairBoot(int disk, int part, std::string& msg);

}  // namespace sysrecover
