#pragma once
// 新硬盘初始化的 diskpart 脚本生成（纯逻辑，2026-10-27）。
//
// 为什么单独一个文件：这是纯字符串拼接、不碰任何 Windows API，放进
// `make check` 的单元测试里就能守住"每一步都带了 select disk / exit、
// 顺序不能乱、MBR 必须有 active"这些只能用真机运维才发现的问题。
// 执行部分（写文件 / RunProcess / bootsect / 回读校验）在 partinit.cpp。
#include <cstdint>
#include <string>
#include <vector>

namespace sysrecover {

// 一步 = 一次独立的 diskpart 调用（用户 2026-10-27 规格：分区向导每一步都要
// 在状态栏显示正在做什么）。拆成多次调用是为了让 GUI 能在每一步之前报活 ——
// 一条大脚本中途没有任何可观测状态，几十秒里进度条像卡死。
struct PartScriptStep {
    std::string stage;   // 纯 ASCII 阶段名（partinit-clean / -convert / ...）
    std::string script;  // 完整 diskpart 脚本（含 select disk 与 exit）
};

// 生成分步脚本。字母传 0 表示该分区不分配盘符（磁盘满/无空闲符时）。
// GPT：clean → convert gpt → ESP(FAT32) → MSR → 系统盘(NTFS) → [数据盘]
// MBR：clean → convert mbr → 系统盘(NTFS, active) → [数据盘]
// 返回顺序即执行顺序；MBR 方案另外要求调用方跑 `bootsect /nt60 X: /mbr`。
std::vector<PartScriptStep> BuildDiskpartSteps(
    bool gpt, uint32_t diskIndex, unsigned long long espMb,
    unsigned long long msrMb, unsigned long long systemGb, bool createData,
    wchar_t espLetter, wchar_t sysLetter, wchar_t dataLetter);

// 把分步脚本拼回"一条命令搞定"的形态（只用于日志留档 / 人工排错时复制粘贴）。
std::string JoinDiskpartSteps(const std::vector<PartScriptStep>& steps,
                              uint32_t diskIndex);

}  // namespace sysrecover
