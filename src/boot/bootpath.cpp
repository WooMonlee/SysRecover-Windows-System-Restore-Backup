#include "bootpath.h"

namespace sysrecover {

bool ShouldUseUefiBoot(bool firmwareIsUefi, PartitionStyle diskStyle) {
    if (diskStyle == PartitionStyle::GPT)
        return true;  // GPT 只能 UEFI 引导（BIOS+GPT 已被 safety.cpp 预拒）
    if (diskStyle == PartitionStyle::MBR)
        return false;  // MBR 走 BIOS/GRUB4DOS 链 —— 固件即使是 UEFI 也一样
    return firmwareIsUefi;  // 风格未知 → 退回固件类型
}

}  // namespace sysrecover
