#pragma once
// A 引擎残留检测与分流（0.7，用户 2026-10-09 规格）：
//   程序运行时检测上次遗留的 <盘>:\~new / ~old ——
//     · 隔了较久（≥24h）→ 直接删除（记日志）；
//     · 刚发生的 → 接着处理（完整 ~new 复用免重解压；~old 补做清理）；
//     · 拿不准（交换中断态）→ 问用户（GUI 弹框 / CLI 询问）。
// 决策表见 docs/23 §5；纯逻辑 DecideSwapLeftover 与磁盘 IO 分离。
#include <string>
#include <vector>

namespace sysrecover {

enum class SwapLeftAction {
    Delete,   // 直接删除（过期 / 已证明是交换完成后的垃圾）
    Reuse,    // 近期完整 ~new：还原时复用，免重解压（接着处理）
    Redo,     // 近期不完整 ~new：还原时自动删除重新解压（启动期只记日志）
    AskUndo,  // 拿不准（~old 交换中断态）：问用户 撤销搬回 / 保留不动
};

struct SwapLeftInfo {
    wchar_t drive = 0;                     // 盘符（如 L'C'）
    bool isNew = false;                    // true=~new / false=~old
    bool hasMarker = false;                // ~new/.zj-done 存在
    unsigned long long markerContent = 0;  // 标记里的 content= 字节
    long ageHours = -1;                    // 距最近活动小时数；-1=无法判定（按"刚发生"保守处理）
    bool rootIntact = false;               //（~old）根下有 Windows 目录
    bool siblingNew = false;               //（~old）同盘 ~new 仍在
    bool rescueOk = false;                 //（~old）根下 ZJRESTORE-status.txt result=OK
};

struct SwapLeftVerdict {
    SwapLeftAction action = SwapLeftAction::Redo;
    std::string reason;  // 日志/排障用（ASCII，不进 UI）
};

// 残留超过该小时数视为过期（用户 2026-10-09 定：24 小时）
constexpr long kSwapLeftStaleHours = 24;

// 纯策略（不碰磁盘）。ageHours<0 视为"刚发生" → 不走"过期直接删"。
SwapLeftVerdict DecideSwapLeftover(const SwapLeftInfo& i,
                                   long staleHours = kSwapLeftStaleHours);

// 扫描全部固定盘的 ~new / ~old（属性级检查，毫秒级）
std::vector<SwapLeftInfo> ScanSwapLeftovers();

// 单盘探测 ~new（还原暂存时判定能否复用）：找到返回 true
bool ProbeSwapNew(wchar_t drive, SwapLeftInfo& out);

// 同步删除（带 busy 登记，WaitSwapCleanup 可等）+ 记日志
void DeleteSwapLeftover(const SwapLeftInfo& i);
// 后台线程删除（GUI 启动期用，避免大树删除卡住开窗）
void DeleteSwapLeftoverAsync(const SwapLeftInfo& i);
// 等某盘的后台删除收尾（还原暂存进 swap 分支前调用，防并发踩踏）
void WaitSwapCleanup(wchar_t drive);

// 撤销：把 <盘>:\~old 内容搬回根（尽力而为；冲突留在 ~old 并记日志）。
// 返回 true = 全部搬回且 ~old 已空；false = 有冲突/失败（detail 说明）。
bool UndoSwapLeftover(wchar_t drive, std::string& detail);

}  // namespace sysrecover
