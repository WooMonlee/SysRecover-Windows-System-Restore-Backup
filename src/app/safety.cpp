// 还原安全检查实现。
#include "safety.h"

#include <cstdlib>
#include <cwctype>

#include "../common/process.h"

namespace sysrecover {
namespace {

// BitLocker 判定（best-effort）：manage-bde 不可用→放行+调用方记 warn。
// 先检查 Protection Status 行（"Off"/"关闭"→直接放行），
// 再检查 Percentage Encrypted（>0.5% 才拒绝）。
bool IsBitLockerEncrypted(wchar_t letter) {
    std::wstring args = L"-status ";
    args += letter;
    args += L":";
    std::string out;
    if (RunProcess(SysToolPath(L"manage-bde.exe"), args, out) != 0)
        return false;  // 工具失败则放行（避免误杀），调用方可记 warn

    // 先看 Protection Status：Off/关闭 → 未启用，直接放行
    if (out.find("Protection Off") != std::string::npos ||
        out.find("Protection off") != std::string::npos ||
        out.find("\xe4\xbf\x9d\xe6\x8a\xa4\xe5\x85\xb3\xe9\x97\xad") != std::string::npos)  // "保护关闭"
        return false;

    // 再看 Percentage Encrypted
    size_t pos = out.find("Percentage Encrypted");
    if (pos == std::string::npos)
        pos = out.find("\xe5\x8a\xa0\xe5\xaf\x86");  // "加密" UTF-8
    if (pos == std::string::npos)
        return false;
    // 取其后第一个数字
    size_t i = pos;
    while (i < out.size() &&
           !(out[i] >= '0' && out[i] <= '9'))
        ++i;
    double v = 0;
    if (i < out.size())
        v = atof(out.c_str() + i);
    return v > 0.5;
}

}  // namespace

std::string CheckRestoreTarget(const PartitionInfo& p,
                               const std::wstring& imagePath) {
    if (p.isEsp)
        return "拒绝：目标是 ESP 分区";
    if (p.isMsr)
        return "拒绝：目标是 MSR 分区";
    if (p.isRecovery)
        return "拒绝：目标是恢复分区";
    if (!p.letter.empty() && imagePath.size() > 1 && imagePath[1] == L':') {
        if (towupper(p.letter[0]) == towupper(imagePath[0]))
            return "拒绝：镜像在目标分区内，请先移走";
    }
    if (!p.letter.empty() && IsBitLockerEncrypted(p.letter[0]))
        return "拒绝：目标分区已启用 BitLocker";
    // GPT 目标：只有 UEFI 固件能引导（走 ESP 上的 bootmgfw + BCD）；BIOS 固件下
    // GRUB4DOS 那套链（grldr.mbr = 16 位实模式）在 GPT 上也没法用 → 仍拒绝。
    if (!p.guid.empty() && !IsUefiFirmware())
        return "拒绝：GPT 分区需要 UEFI 引导，当前机器是 BIOS 固件（请用 MBR 系统盘）";
    return "";
}

}  // namespace sysrecover
