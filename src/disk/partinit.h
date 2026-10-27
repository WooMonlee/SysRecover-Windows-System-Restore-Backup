#pragma once
// 新硬盘初始化（2026-10-20，用户规格）：PE 里遇到"没有任何分区的固定硬盘"时，
// 引导用户按方案创建分区（GPT/UEFI 或 MBR/Legacy），再由还原流程写入系统。
//
// 原则（用户拍板）：
//  · **只推荐、不禁止** —— 两种方案始终可选，除非技术上不可能：
//      - 磁盘 >2TB → 只能 GPT（MBR 单分区上限 2TB）；
//      - Win7 x86 / XP 类镜像 → 只能 MBR（这些系统不支持 UEFI 启动）。
//  · 当前 PE 以什么方式启动、固件是否支持 UEFI 可检测；**BIOS 里另一模式是否被
//    禁用无法检测** → 只给提示（Legacy 会话选 GPT：还原后需在 BIOS 改 UEFI）。
//  · 默认系统盘：Win7 60GB / Win10+ 100GB（可改）；ESP 300MB、MSR 16MB。
//  · 分区后：MBR 必须补写磁盘引导代码 + 活动分区（否则还原完黑屏）；
//    GPT 写保护性 MBR + ESP(FAT32) + MSR。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "disk.h"

namespace sysrecover {

enum class DiskScheme { Gpt = 0, Mbr = 1 };

// 可初始化的"空盘"（固定盘、没有任何分区；可能是 RAW 或已初始化但无分区）。
struct EmptyDiskInfo {
    uint32_t index = 0;
    std::wstring model;
    std::wstring serial;
    uint64_t sizeBytes = 0;
    bool initialized = false;              // true=已有空分区表；false=RAW
    PartitionStyle style = PartitionStyle::Unknown;
};

struct PartInitPlan {
    uint32_t diskIndex = 0;
    DiskScheme scheme = DiskScheme::Gpt;
    unsigned long long espMb = 300;
    unsigned long long msrMb = 16;
    unsigned long long systemGb = 100;
    bool win7Family = false;
    // 是否创建"其余全部"数据盘（2026-10-21 用户规格：60GB 小盘只做系统盘；
    // 剩余空间 <5GB 时也不建数据盘）。
    bool createData = true;
};

// 小容量磁盘的推荐值（纯逻辑）：usableGb = 扣掉分区表/余量后的可用 GB；
// systemGb = 推荐系统盘；createData = 是否还够建数据盘。
// 规则（用户 2026-10-21）：
//   · Win10/11：可用 ≥105 → 系统盘 100 + 数据；65~104 → 60 + 数据；
//     <65 → 整盘做系统盘（不建数据盘）。
//   · Win7：可用 ≥65 → 60 + 数据；35~64 → 30 + 数据；<35 → 整盘做系统盘。
struct PartSizeHint {
    unsigned long long usableGb = 0;
    unsigned long long systemGb = 0;
    bool createData = false;
};
PartSizeHint SuggestPartitionSizes(bool win7Family, DiskScheme scheme,
                                   uint64_t diskSize);

// 检测"没有任何分区"的固定盘（排除可移动盘；已分区但无分区的也算）。
std::vector<EmptyDiskInfo> FindEmptyDisks();

// 纯逻辑：推荐顺序 + 两种方案是否可选。
// uefiSession=当前 PE 是否以 UEFI 方式启动；uefiCapable=固件是否支持 UEFI。
void RankPartSchemes(bool uefiSession, bool uefiCapable, bool win7Family,
                     uint64_t diskSize, DiskScheme& first, DiskScheme& second,
                     bool& mbrAllowed, bool& gptAllowed);

// 纯逻辑：硬限制/容量校验（返回 false + err 给用户看）。
bool ValidatePartInitPlan(const PartInitPlan& p, uint64_t diskSize,
                          std::wstring& err);

// 执行：分配盘符 → 逐步骤 diskpart → bootsect（MBR）→ 回读校验。
// 返回 0 成功；非 0 失败（err=UTF-16 说明，log=UTF-8 过程）。
// 磁盘脚本本身由 partscript.h 的 BuildDiskpartSteps() 生成（纯逻辑、可单测）。
//
// progress（可空，2026-10-27 用户规格：分区向导必须逐步报告）按阶段回调，
// **每一步 diskpart 之前都报一次**（一条大脚本几十秒无输出 = 冻住的进度条）：
//   partinit-letters(5) → partinit-script(12)
//   → partinit-clean → partinit-convert → [partinit-esp → partinit-msr]
//   → partinit-system → [partinit-data]   （12..82 均分，每步开始前上报）
//   → partinit-bootsect(86) → partinit-verify(92) → partinit-done(100)
// 阶段名保持纯 ASCII，由 GUI 侧 StageCn() 翻成中文（日志/契约不翻）。
// 回调返回 void：diskpart/bootsect 一旦开始就无法安全中断，这里只做"看得见"。
using PartInitProgressFn = std::function<void(int, const std::string&)>;
int ApplyPartInitPlan(const PartInitPlan& p, std::wstring& err,
                      std::string& log,
                      const PartInitProgressFn& progress = nullptr);

}  // namespace sysrecover
