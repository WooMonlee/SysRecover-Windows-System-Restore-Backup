#include "bootpath.h"

namespace sysrecover {

bool ShouldUseUefiBoot(bool firmwareIsUefi, PartitionStyle diskStyle) {
    if (diskStyle == PartitionStyle::GPT)
        return true;  // GPT 只能 UEFI 引导（BIOS 会话 + GPT 暂存由 CheckStagedBootPath 拦截）
    if (diskStyle == PartitionStyle::MBR)
        return false;  // MBR 走 BIOS/GRUB4DOS 链 —— 固件即使是 UEFI 也一样
    return firmwareIsUefi;  // 风格未知 → 退回固件类型
}

StagedBootPath CheckStagedBootPath(bool sessionIsUefi, PartitionStyle diskStyle) {
    // 会话 BIOS + GPT 目标：重启后走 BIOS 链，GPT 没有可用 BIOS 引导 → 必坏。
    if (!sessionIsUefi && diskStyle == PartitionStyle::GPT)
        return StagedBootPath::BiosSessionGptTarget;
    // 会话 UEFI + MBR 目标：重启后走 UEFI 链，16 位 GRUB4DOS UEFI 无法执行 → 必坏。
    if (sessionIsUefi && diskStyle == PartitionStyle::MBR)
        return StagedBootPath::UefiSessionMbrTarget;
    return StagedBootPath::Ok;  // GPT+UEFI / MBR+BIOS / Unknown → 可以暂存
}

}  // namespace sysrecover
