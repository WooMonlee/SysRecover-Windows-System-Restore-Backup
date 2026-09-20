#pragma once
// 引导补全（BIOS/MBR）：Linux 侧没有 bcdboot，所以在 Windows 暂存阶段就把
// 「BIOS 启动文件」准备好，随还原任务一起下发，Linux 侧 apply 完主镜像后再
// 写进目标分区。见 AGENTS.md §7 与 PLAN.md §4。
//
// 产出目录（recoveryDir\bootfix\）：
//   \bootmgr
//   \Boot\*（含 BCD —— device/osdevice 已改成可移植的 "boot"）
#include <string>

namespace sysrecover {

// 生成引导补全文件。返回 false 表示失败（调用方记日志，不致命）。
// recoveryDir：恢复目录（D:\ZJRESTORE）。
bool PrepareBootFixFiles(const std::wstring& recoveryDir, std::string& log);

}  // namespace sysrecover
