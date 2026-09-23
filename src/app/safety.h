#pragma once
// 还原安全检查（fail-closed）。四项见 AGENTS.md §11。
#include <string>
#include <vector>

#include "../disk/disk.h"

namespace sysrecover {

// 返回空串=通过，否则为拒绝原因（UTF-8 中文）。
std::string CheckRestoreTarget(const PartitionInfo& p,
                               const std::wstring& imagePath);

// 扫一遍系统里所有盘符，返回**启用了 BitLocker** 的卷（形如 L"C:"）。
// 用途（用户规格 2026-09-23）：还原前提醒用户 —— 加密卷若没有密码/恢复密钥，
// 还原后数据将无法恢复；由用户选择**继续或退出**（不再硬拒绝）。
std::vector<std::wstring> BitLockerVolumes();

}  // namespace sysrecover
