#pragma once
// 磁盘/分区枚举（Phase 1）。只用 Win32 API，禁用 WMI（PE 兼容，见 AGENTS.md §6）。
#include <cstdint>
#include <string>
#include <vector>

namespace sysrecover {

enum class PartitionStyle { Unknown, MBR, GPT };

struct PartitionInfo {
    uint32_t diskIndex = 0;
    uint32_t partNumber = 0;  // 1-based；0 表示未编号条目
    std::wstring guid;        // GPT PartitionId，小写无花括号；MBR 为空
    // 卷名 \\?\Volume{...}\（FindFirstVolume 原样返回）；无卷映射时为空。
    // GPT 下通常与 partition GUID 相同，但刚分区/刚格式化时 mountvol 按
    // partition GUID 可能找不到，必须用这个真实卷名（0.9.33 ESP 修复）。
    std::wstring volumeName;
    uint64_t offsetBytes = 0;
    uint64_t sizeBytes = 0;
    std::wstring letter;      // L"C"，无盘符为空
    std::wstring label;       // 卷标
    std::wstring fs;          // NTFS / FAT32 / ...
    uint64_t freeBytes = 0;   // 可用字节；0 = 未知（无盘符时）
    bool isSystem = false;    // 含 %SystemRoot%
    bool isEsp = false;
    bool isMsr = false;
    bool isRecovery = false;
};

struct DiskInfo {
    uint32_t index = 0;
    std::wstring model;
    std::wstring serial;
    uint64_t sizeBytes = 0;
    PartitionStyle style = PartitionStyle::Unknown;
    bool isRemovable = false;
    std::vector<PartitionInfo> parts;
};

// 枚举本地固定磁盘（跳过可移动盘与光驱）。失败的磁盘静默跳过。
std::vector<DiskInfo> EnumerateDisks();

// 磁盘健康（只读、尽力而为）。诚实告知"能不能取到"：
// 走 ATA PASS THROUGH 读 SMART（ATA/SATA 一般可读，含 USB-SATA 桥；NVMe / RAID 虚拟盘
// 取不到）→ 取不到就 smartKnown=false，**不阻断流程**（fail-open，只在日志留一行）。
// 用途（用户 2026-09-26 需求）：还原会**格式化目标分区**，若目标盘已出现坏道/待定扇区，
// 还原完系统照样起不来 —— 不如提前警告，让用户先换盘。
struct DiskHealth {
    bool smartKnown = false;            // 是否真的取到 SMART / 健康日志
    bool isNvme = false;                // true = 数据来自 NVMe 日志页（否则 ATA SMART）
    bool failing = false;               // ATA：驱动器自报"即将故障"（SMART RETURN STATUS）
    bool failingKnown = false;          // ATA：上述结论是否可信（回读 CL/CH 为 0x4F/0xC2 或 0xF4/0x2C）
    unsigned char srCl = 0;             // ATA：SMART RETURN STATUS 回读的 Cylinder Low（排障/取证用）
    unsigned char srCh = 0;             // ATA：SMART RETURN STATUS 回读的 Cylinder High
    uint64_t reallocatedSectors = 0;    // ATA 属性 5   重映射（已用备用扇区顶替）
    uint64_t pendingSectors = 0;        // ATA 属性 197 待定（读失败，等待重映射）
    uint64_t uncorrectableSectors = 0;  // ATA 属性 198 无法纠正
    unsigned criticalWarning = 0;       // NVMe：Critical Warning 位图（0 = 正常）
    uint64_t mediaErrors = 0;           // NVMe：Media and Data Integrity Errors
    uint64_t errorLogCount = 0;         // NVMe：Error Information Log Entries
    int usedPercent = -1;               // NVMe：Percentage Used（磨损；-1 = 未知）
    int tempC = -1;                     // 温度 ℃（-1 = 未知）
    bool caution = false;               // 综合判定：建议先换盘再还原
};

// 查询物理磁盘健康（diskIndex = 0-based，与 DiskInfo::index 一致）。
DiskHealth QueryDiskHealth(uint32_t diskIndex);

const char* StyleName(PartitionStyle s);

// 当前固件是 UEFI 还是 BIOS（Win8+ 用 GetFirmwareType；Win7 回退用固件变量探测）。
bool IsUefiFirmware();

// 本次运行**是否以 UEFI 方式启动**（≠ 固件是否支持 UEFI）：
//   · UEFI 会话有 UEFI 运行时（固件变量可读）→ true；
//   · Legacy/CSM 会话没有 → false（GetFirmwareType 仍可能是 UEFI）。
// 用途（2026-10-20）：新硬盘分区向导/第三步确认框提示"还原后要不要改 BIOS
// 启动方式"；BIOS 里另一模式是否被禁用**无法检测**，只能提示。
bool IsUefiBootSession();

// 是否运行在 WinPE 里（标准判据：HKLM\SYSTEM\CurrentControlSet\Control\MiniNT 存在）。
// 用途（用户 2026-09-23 规格）：PE 下放宽"就地还原"的**卷锁**判定 —— PE 里目标分区
// 通常没被真正使用，但 FSCTL_LOCK_VOLUME 仍可能因杂七杂八的句柄失败，导致误判成
// "必须重启"。PE 里只要目标不是正在运行的系统盘，就应该就地还原、不重启也不提示。
bool IsWinPE();

// 找 ESP（EFI System Partition）。注意：正常 Windows 下 ESP 无盘符（需临时挂载）。
// onlyDiskIndex>=0 时只在该盘上找（Q2，2026-10-18）——多盘机固件 ESP 可能不在
// 还原目标盘上，往错盘的 ESP 写引导是错的；-1 = 任意盘（备份本机 ESP 等）。
bool FindEspPartition(PartitionInfo& out, int onlyDiskIndex = -1);

// 回退（docs/15 故障 B）：分区类型 GUID **不是** ESP 时（DiskGenius 重建分区常把它
// 标成 Basic Data / 或压根没建 MSR+ESP），只要它是 FAT 分区且根下有
//   \EFI\Microsoft\Boot\bootmgfw.efi  或  \EFI\BOOT\BOOTX64.EFI
// 就**照样能引导**（固件/`mountvol` 只认文件路径，不认分区类型 GUID）。
// 只在分区**有盘符**时可判定（没盘符就不带盘符找 —— 那种情况交给 mountvol X: /s）。
// onlyDiskIndex 语义同 FindEspPartition。
bool FindEspPartitionFallback(PartitionInfo& out, int onlyDiskIndex = -1);

// 卷盘符（如 'S'，即 `\\.\S:`）所在的**物理磁盘号** ——
// IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 取 Extents[0].DiskNumber。
// 用途（Q2，2026-10-18）：`mountvol /s` 挂上的固件 ESP 在多盘机上可能不在
// 还原目标盘上，挂完必须校验盘号；取不到（无盘符/句柄失败）→ -1。
int VolumeDiskIndex(wchar_t letter);

// 分区表摘要（**纯 ASCII**：给日志与报错用，避免编码坑 —— 见 docs/15 §7-P4）。
// 形如：`disk0 GPT Msft Virtual Disk: p1 100MB FAT32 [ESP] ; p2 50GB NTFS C: ...`
std::wstring DescribePartitions();

}  // namespace sysrecover
