#pragma once
// 引导方式决策（纯逻辑，可单测）：**以目标磁盘的分区风格为准**，而不是只看固件类型。
//
// 背景（2026-09-29 用户实测，Win10 x64 + MBR 分区）：UEFI 固件机器上装的是
// **CSM/Legacy 引导的 MBR 系统**（很常见：OEM 预装、克隆盘、把盘从 GPT 转成 MBR、
// 或在 Legacy 模式下装的系统）。这类机器上：
//   * 固件类型 = UEFI（`GetFirmwareType()` 只看平台，不看当前是怎么启动的）；
//   * 但**没有 ESP**，引导链是 legacy `bootmgr`（`\Boot\BCD` + 实模式启动扇区）。
// 只看固件类型 → 误走 UEFI/ESP 分支 → `FindEspPartition` 找不到 → 直接失败：
//   "UEFI 机器未找到 ESP 分区，无法部署引导层"（用户截图 C:\2222.png）。
//
// 正确判据：
//   目标 **MBR** → BIOS/GRUB4DOS 链（bootmgr → 实模式启动扇区 → `grldr.mbr`），
//                  **哪怕固件是 UEFI**（那台机器本来就是 legacy 启动 Windows 的）；
//   目标 **GPT** → UEFI/ESP 链（且要求固件是 UEFI；BIOS 固件 + GPT 由 safety 预拒）；
//   风格未知（罕见）→ 退回固件类型判断。
#include "../disk/disk.h"

namespace sysrecover {

bool ShouldUseUefiBoot(bool firmwareIsUefi, PartitionStyle diskStyle);

}  // namespace sysrecover
