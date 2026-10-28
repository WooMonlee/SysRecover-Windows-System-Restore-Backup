#pragma once
// 磁盘 MBR 引导代码写入（2026-10-27，用户反馈 2：极简 WinPE 里没有 bootsect.exe）。
//
// 只写**前 440 字节**（引导代码区），保留 446..509 的分区表与 510..511 的
// 0x55AA 签名；磁盘上已有非零引导代码时**不覆盖**（可能是 GRUB/第三方引导，
// 覆盖等于把别人的多系统引导废掉）。
//
// 为什么放在 disk 层：`disk/partinit`（分区向导）与 `boot` 层都要用，而
// `disk` 不能反向依赖 `boot`（AGENTS §4 依赖 DAG）。
#include <string>

namespace sysrecover {

// 确保磁盘前 440 字节是随包微软引导代码（exeDir\bootfiles\mbr-boot-code.bin）。
//   · 已有非零代码 + 0x55AA → 保留并返回 true（log 记 keep）；
//   · 全零 → 写入并回读校验（0x55AA + 前 440 字节逐字节）；
//   · 读不到磁盘 / 资产缺失 / 写不进 → 返回 false（log 记原因，调用方降级为告警）。
// 4Kn 盘同样按**字节偏移**写：MBR 永远是 LBA0 的前 512 字节。
bool EnsureDiskMbrCode(unsigned diskIndex, const std::wstring& exeDir,
                       std::string& log);

}  // namespace sysrecover
