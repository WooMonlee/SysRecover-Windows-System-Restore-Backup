#pragma once
// 备份/还原共享操作层：CLI 与 GUI 共用单一实现（AGENTS.md §4 模块职责）。
// 语义对齐旧 C# BackupOrchestrator/RestoreOrchestrator（已冻结契约）。
#include <string>

#include "../wim/wim.h"

namespace sysrecover {

struct BackupRequest {
    std::wstring source = L"C:/";  // 盘符根触发热备（snapshot+排除配置）
    std::wstring dest;             // 目标 WIM/ESD 全路径（必填）
    std::string compress = "fast"; // fast | maximum | recovery
    std::wstring name = L"Backup"; // 子镜像名
    bool append = false;           // true=追加到已有 WIM
    bool verify = false;           // true=写完后跑 wimlib_verify_wim
    bool snapshot = false;         // true=强制 VSS 快照（非盘符根也可用，如活动数据库目录）
};

struct RestoreRequest {
    std::wstring image;  // WIM 全路径
    int disk = 0;        // 目标磁盘号
    int part = 0;        // 目标分区号（1-based）
    int index = 1;       // 子镜像（1-based）
    bool repairBoot = true;
};

// 备份。覆盖确认由调用方负责（CLI --yes / GUI 对话框）。
// 返回：0 成功；1 失败（err=原因 UTF-8）。
int RunBackup(const BackupRequest& req, ProgressFn progress, std::string& err);

// 还原。**两种执行方式自动选择**（PIT-064）：
//   * 目标分区**没被占用**时（在 PE 里、或目标是别的分区）→ 就地还原：格式化 +
//     apply + 修引导，**不重启**（needReboot=false）；
//   * 目标被占用（还原正在运行的系统盘）→ 暂存任务 + 重启进 Linux 救援层执行
//     （§2 禁令1），needReboot=true。
// 返回：0 成功；1 失败（err=原因）；4 安全门禁拒绝（err=理由）。
int StageRestore(const RestoreRequest& req, std::string& err,
                 bool* needReboot = nullptr);

}  // namespace sysrecover
