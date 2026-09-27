// 还原安全检查实现。
#include "../common/i18n.h"
#include "safety.h"

#include <windows.h>  // GetLogicalDrives / GetDriveTypeW（BitLocker 全盘扫描）

#include <cstdlib>
#include <cwctype>

#include "../common/process.h"

namespace sysrecover {
namespace {

// BitLocker 判定（best-effort）：manage-bde 不可用→放行+调用方记 warn（fail-open）。
//
// ⚠️ 语言无关（PLAN §14 M1.2 审计，2026-09-27）：manage-bde 的**行标签随系统语言变**
// （en "Percentage Encrypted" / zh "加密" / de 等又不同），旧版只认 en+zh 标签 →
// 其他语言系统上**恒漏报**（静默 fail-open，看不出来）。改为只认**数值**：
// 扫描输出里所有 `<数字>%` 记号取最大值，> 0.5 即视为已加密 —— 标签可以翻译，
// `100.0%` / `0.0%` 不会。语义同时修正：旧版把 "Protection Off" 当成"未启用"，
// 但那其实常常是**加密但已挂起保护**的卷（仍在加密）→ 现在按"是否加密"判，
// 该提醒就提醒（fail-closer：宁可多提醒一次，也不漏掉"恢复密钥在被还原的 C 盘上"的场景）。
bool IsBitLockerEncrypted(wchar_t letter) {
    std::wstring args = L"-status ";
    args += letter;
    args += L":";
    std::string out;
    if (RunProcess(SysToolPath(L"manage-bde.exe"), args, out) != 0)
        return false;  // 工具失败则放行（避免误杀），调用方可记 warn

    double best = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] < '0' || out[i] > '9')
            continue;
        size_t j = i;
        while (j < out.size() && ((out[j] >= '0' && out[j] <= '9') || out[j] == '.'))
            ++j;
        size_t k = j;
        while (k < out.size() && (out[k] == ' ' || out[k] == '\t'))
            ++k;
        if (k < out.size() && out[k] == '%') {  // 数字后（隔空格可）紧跟 % = 百分比记号
            double v = atof(out.c_str() + i);
            if (v > best) best = v;
            i = k;
        }
    }
    return best > 0.5;
}

}  // namespace

// 全系统 BitLocker 扫描（用户规格 2026-09-23）—— 见 safety.h。
std::vector<std::wstring> BitLockerVolumes() {
    std::vector<std::wstring> out;
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE)
            continue;
        if (IsBitLockerEncrypted(c)) {
            std::wstring v(1, c);
            v += L":";
            out.push_back(v);
        }
    }
    return out;
}

std::string CheckRestoreTarget(const PartitionInfo& p,
                               const std::wstring& imagePath,
                               ErrAdvice* adv) {
    if (p.isEsp)
        return Tr("拒绝：目标是 ESP 分区");
    if (p.isMsr)
        return Tr("拒绝：目标是 MSR 分区");
    if (p.isRecovery)
        return Tr("拒绝：目标是恢复分区");
    if (!p.letter.empty() && imagePath.size() > 1 && imagePath[1] == L':') {
        if (towupper(p.letter[0]) == towupper(imagePath[0])) {
            if (adv) *adv = ADV_IMAGE_IN_TARGET;
            return Tr("拒绝：镜像在目标分区内，请先移走");
        }
    }
    // BitLocker 不再在这里硬拒绝（用户规格 2026-09-23）：改为"全系统扫描 + 提醒
    // 用户可选继续/退出"，见 BitLockerVolumes() 以及 GUI/CLI 的提示。
    // GPT 目标：只有 UEFI 固件能引导（走 ESP 上的 bootmgfw + BCD）；BIOS 固件下
    // GRUB4DOS 那套链（grldr.mbr = 16 位实模式）在 GPT 上也没法用 → 仍拒绝。
    if (!p.guid.empty() && !IsUefiFirmware())
        return Tr("拒绝：GPT 分区需要 UEFI 引导，当前机器是 BIOS 固件（请用 MBR 系统盘）");
    return "";
}

// 目标物理磁盘健康警告（PIT-086，用户 2026-09-26 需求）—— 见 safety.h。
std::string CheckDiskHealth(int diskIndex) {
    DiskHealth h = QueryDiskHealth((uint32_t)diskIndex);
    if (!h.smartKnown)
        return "";  // 取不到 SMART（NVMe/RAID/USB 桥）→ 静默放行
    if (!h.caution)
        return "";
    // 组织成"给用户看的一句话"：先说结论，再列具体指标。
    std::string s = Tr("警告：目标磁盘（磁盘") + std::to_string(diskIndex) +
                    Tr("）健康状态异常 —— 该盘可能已出现坏道，还原后系统仍可能无法启动。");
    auto add = [&](const char* what, uint64_t v) {
        if (v)
            s += "\n  - " + std::string(what) + Tr("：") + std::to_string(v);
    };
    if (h.failing)
        s += Tr("\n  - 驱动器自报：即将故障（SMART 阈值已超）");
    add(Tr("待定扇区（读失败待重映射）"), h.pendingSectors);
    add(Tr("无法纠正扇区"), h.uncorrectableSectors);
    add(Tr("重映射扇区"), h.reallocatedSectors);
    if (h.isNvme) {
        if (h.criticalWarning) {
            s += Tr("\n  - NVMe 关键警告位图 = ") + std::to_string(h.criticalWarning) +
                 Tr("（bit0 备用空间不足 / bit1 温度 / bit2 可靠性降级 / bit4 备份失败）");
        }
        add(Tr("介质与数据完整性错误（Media Errors）"), h.mediaErrors);
        add(Tr("错误日志条目数"), h.errorLogCount);
    }
    s += Tr("\n建议：先更换/检修硬盘，或用镜像恢复到另一块好盘。仍要继续会覆盖目标分区。");
    return s;
}

}  // namespace sysrecover
