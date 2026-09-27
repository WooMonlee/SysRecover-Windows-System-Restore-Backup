#pragma once
// 还原安全检查（fail-closed）。四项见 AGENTS.md §11。
#include <string>
#include <vector>

#include "../disk/disk.h"
#include "advice.h"  // ErrAdvice（拒绝原因 → 建议码，i18n 解耦见 PLAN §14 M1）

namespace sysrecover {

// 返回空串=通过，否则为拒绝原因（UTF-8 中文）。
// adv（可选）：仅"镜像在目标分区内"给 ADV_IMAGE_IN_TARGET，其余拒绝（ESP/MSR/
// 恢复分区/GPT-BIOS）没有对应的"下一步怎么办" → 保持 ADV_NONE。
std::string CheckRestoreTarget(const PartitionInfo& p,
                               const std::wstring& imagePath,
                               ErrAdvice* adv = nullptr);

// 扫一遍系统里所有盘符，返回**启用了 BitLocker** 的卷（形如 L"C:"）。
// 用途（用户规格 2026-09-23）：还原前提醒用户 —— 加密卷若没有密码/恢复密钥，
// 还原后数据将无法恢复；由用户选择**继续或退出**（不再硬拒绝）。
std::vector<std::wstring> BitLockerVolumes();

// 目标**物理磁盘**的健康警告（PIT-086，用户 2026-09-26 需求）：还原要格式化目标分区，
// 若目标盘已出现坏道（SMART 待定/无法纠正扇区、重映射过多、自报即将故障）→ 还原完
// 系统照样起不来，不如先换盘。返回空串 = 健康 **或取不到 SMART**（fail-open，不阻断）。
// 形如："警告：目标磁盘（磁盘0）健康状态异常 …\n  - 待定扇区（读失败待重映射）：8\n建议：…"。
std::string CheckDiskHealth(int diskIndex);

}  // namespace sysrecover
