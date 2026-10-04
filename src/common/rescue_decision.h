#pragma once
// 救援回执 / 待执行标记的**判定逻辑**（纯逻辑，零依赖，可单测；PIT-115 配套）。
//
// 背景（用户 2026-10-05 规格）："救援层是否真的跑过"用两个文件的时间比较来判：
//   · 待执行标记 <日志根>\logs\pending-restore.txt —— 仅"暂存+重启"路径写；
//     （菜单安装不写，避免常驻菜单误报）
//   · 回执      <各分区根>\ZJRESTORE-status.txt —— 救援层跑完写（OK/FAILED）。
// 时间均为本地 "YYYY-MM-DD HH:MM:SS" 字符串（空 = 不存在）；字符串序即时间序。
#include <string>

namespace sysrecover {

struct RescueRunTimes {
    std::string latestStatusTime;  // 最新一条回执（OK/FAILED 都算）
    std::string pendingTime;       // 待执行标记（空 = 无）
    std::string latestFailedTime;  // 最新一条 FAILED 回执（空 = 无）
};

enum class RescueReport {
    None,             // 无事可报
    TaskNotExecuted,  // 标记比所有回执新（或压根没回执）→ 救援层从未执行
    RestoreFailed,    // 有 FAILED 回执（且标记不更新）→ 上次还原失败
};

// 判定规则（与 selfdiag::CheckLastRescueFailure 的提示一一对应）：
// 1) 有标记且（没有回执 或 标记时间 > 最新回执时间）→ TaskNotExecuted
//    （时间相等视为"已执行"，避免同秒竞态误报）；
// 2) 否则有 FAILED 回执 → RestoreFailed；
// 3) 否则 → None（含"只有 OK 回执"与"只有新标记但时间相等"）。
// 稳健性：调用方正常会同时填 latestStatusTime / latestFailedTime；这里取两者
// 的较大值作为"最新回执"，即便只填了 FAILED 也不会误判成"从未执行"
//（单测 rescue_report_decision 的第⑦⑧条就吃这个输入）。
inline RescueReport DecideRescueReport(const RescueRunTimes& t) {
    const std::string& newest = t.latestFailedTime > t.latestStatusTime
                                    ? t.latestFailedTime
                                    : t.latestStatusTime;
    if (!t.pendingTime.empty() && (newest.empty() || t.pendingTime > newest))
        return RescueReport::TaskNotExecuted;
    if (!t.latestFailedTime.empty())
        return RescueReport::RestoreFailed;
    return RescueReport::None;
}

}  // namespace sysrecover
