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

const char* StyleName(PartitionStyle s);

// 当前固件是 UEFI 还是 BIOS（Win8+ 用 GetFirmwareType；Win7 回退用固件变量探测）。
bool IsUefiFirmware();

// 是否运行在 WinPE 里（标准判据：HKLM\SYSTEM\CurrentControlSet\Control\MiniNT 存在）。
// 用途（用户 2026-09-23 规格）：PE 下放宽"就地还原"的**卷锁**判定 —— PE 里目标分区
// 通常没被真正使用，但 FSCTL_LOCK_VOLUME 仍可能因杂七杂八的句柄失败，导致误判成
// "必须重启"。PE 里只要目标不是正在运行的系统盘，就应该就地还原、不重启也不提示。
bool IsWinPE();

// 找 ESP（EFI System Partition）。注意：正常 Windows 下 ESP 无盘符（需临时挂载）。
bool FindEspPartition(PartitionInfo& out);

}  // namespace sysrecover
