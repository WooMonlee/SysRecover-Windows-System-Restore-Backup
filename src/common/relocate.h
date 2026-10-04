#pragma once
// 运行目录搬迁（用户 2026-10-03 规格）—— 把日志统一到「程序目录\logs」一个位置：
//   · 程序在**光盘/写保护介质**上：无法写日志 → 复制整包到本地固定分区再运行；
//   · 程序在**U盘**上（即便可写）：GUI 询问、CLI 自动复制（GUI 保留"留在原处"选项）；
//   · 目的地 = **装了 Windows 的分区之外的第一顺序可写固定分区**（系统在 C: 就选 D:，
//     在 D: 就选 C:；含本机离线 Windows 分区，PE 下同样避开未来还原目标）；
//     没有别的可写分区时退回系统分区本身（GUI 文案会说明日志保不住）。
// 复制跳过 `logs\`（目的地已有日志只增不减）；新副本由原进程启动后原进程退出
// （GUI 不等待、互斥由副本接手；CLI 等待并透传退出码）。
#include <string>
#include <vector>

namespace sysrecover {

struct RelocatePlan {
    bool needed = false;          // 是否应搬迁（CD / U盘 / 目录不可写）
    std::wstring why;             // "cdrom" / "removable" / "readonly"
    std::wstring destDrive;       // 目标盘根（"D:\\"）；空 = 没有可用目的地
    std::wstring destRoot;        // 目标目录（"D:\\ZJRESTORE"）；空 = 没有可用目的地
    bool onlySystemDrive = false; // 目的地是系统分区（单分区机器；还原系统盘时保不住）
    unsigned long long needBytes = 0;
};

// 纯逻辑（单测）：候选取"系统分区之外的第一顺序"；都在系统分区上则用系统分区。
// 大小写不敏感；返回 0 = 无可用。
wchar_t SelectRelocateDrive(const std::vector<wchar_t>& candidates,
                            wchar_t systemLetter);

// 检测（只读探测，不写盘）：是否需要搬迁、有没有可搬的目的地。
RelocatePlan CheckRelocate();

// 复制整包（跳过 logs\）并启动新副本。
//   wait=true（CLI）：等子进程结束，退出码写入 *exitCode；
//   wait=false（GUI）：立刻返回，调用方应随后退出（单实例互斥由子进程接手）。
// 成功返回 true（调用方应立即退出）。err = 可展示给用户的失败原因（UTF-8）。
bool DoRelocate(const RelocatePlan& plan, bool wait, int* exitCode,
                std::string* err);

// 整包大小（递归，跳过 logs\）—— 用于目标盘剩余空间检查。
unsigned long long AppTreeSize();

// 日志/数据根（PIT-105）：软件装在**系统盘**上时 → <数据盘>\ZJRESTORE
// （还原/重装系统盘会格式化系统盘，日志必须放别的盘才留得住）；
// 其他位置（数据盘/PE 的 X:/U盘）→ 软件目录。
// 优先选不含 Windows 的数据盘（PE 下避免离线 C:），盘不存在/不可写时退回软件目录。
// 返回的目录已尽力建好；调用方只需再建 `\logs`。
std::wstring LogBaseDir();

}  // namespace sysrecover
