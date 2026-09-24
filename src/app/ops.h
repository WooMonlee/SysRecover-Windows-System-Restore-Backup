#pragma once
// 备份/还原共享操作层：CLI 与 GUI 共用单一实现（AGENTS.md §4 模块职责）。
// 语义对齐旧 C# BackupOrchestrator/RestoreOrchestrator（已冻结契约）。
#include <string>

#include "../disk/disk.h"
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

// 目标分区是否可以**就地还原**（不重启）？
// 供调用方（GUI 的确认框文案、CLI 的提示）决定"要不要说重启"——
// StageRestore 内部调用的是同一个函数，两处不会漂移（PIT-072）。
// 判据：目标不是正在运行的系统盘，且能对目标卷加独占锁（FSCTL_LOCK_VOLUME）。
bool CanRestoreInPlace(const PartitionInfo& target, std::string& why);

// 给常见失败配一句"下一步怎么办"（P8）。CLI 打印、GUI 弹窗都会附上它。
// 匹配不上就返回空串（宁可不给，也不给错的建议）。
std::string ErrorAdvice(int rc, const std::string& err);

// 还原前**空间预检**（P1）：镜像的**未压缩**内容大小 vs 目标分区大小。
// 必须在"格式化之前"拦下，否则会出现"数据没了、系统也没装上"（先格式化再 apply）。
// 返回：0 = 够（或无法判定 → 不阻断，让流程继续）；1 = 空间不足（err=给用户看的说明）。
int CheckRestoreSpace(const std::wstring& imagePath, int index,
                      const PartitionInfo& target, std::string& err);

// 还原。**两种执行方式自动选择**（PIT-064）：
//   * 目标分区**没被占用**时（在 PE 里、或目标是别的分区）→ 就地还原：格式化 +
//     apply + 修引导，**不重启**（needReboot=false）；
//   * 目标被占用（还原正在运行的系统盘）→ 暂存任务 + 重启进 Linux 救援层执行
//     （§2 禁令1），needReboot=true。
// 返回：0 成功；1 失败（err=原因）；4 安全门禁拒绝（err=理由）。
int StageRestore(const RestoreRequest& req, std::string& err,
                 bool* needReboot = nullptr, ProgressFn progress = nullptr);

}  // namespace sysrecover
