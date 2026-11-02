#pragma once
// `bcdedit /enum` 输出解析（纯文本逻辑，无 Win32 依赖 → 可单测）。
//
// 为什么需要它（docs/15 启动故障分析）：`bcdboot` 失败时**不会**报错到我们脸上
// （返回码曾被丢弃），而"写没写成"唯一的可靠判据是**产物**：ESP 上的 BCD 里
// 有没有可引导的条目。这里放判定用的纯逻辑，真正的 IO 在
// `boot/uefi.cpp::VerifyEspBcd()`。
#include <string>
#include <vector>

namespace sysrecover {

// enum 文本是否表明"这个 BCD 里有一个能用 Windows 启动项"：
//   ① {bootmgr} 的 displayorder 存在，且至少跟一个 {GUID}；
//   ② 文本里存在 Windows Boot Loader（path 含 `winload.`）。
// 不成立时把**原因**写进 why（英文，进日志/详情，不面向最终用户）。
bool BcdEnumShowsOsEntry(const std::string& enumText, std::string* why);

// **加强版**产物断言（PIT-208，2026-11-02 客户机"还原后反复重启"）：
// 从 {bootmgr} 的 displayorder/default 出发（可跟嵌套的链），找出**真正会被
// 启动的那个 Windows Boot Loader**，看它的 device/osdevice 是不是指向本次
// 还原的目标分区。
//   candidates = 目标分区的全部"卷标识"（小写不敏感，子串匹配）：
//       "C:" / "\\?\Volume{guid}\" / "\Device\HarddiskVolumeN"
//   · 命中任一 → true；
//   · 有条目但一个都不命中 → false（why 里带实际 device 值）；
//   · candidates 为空 或 BCD 里没有 device 行（字段名本地化等）→ true + 记 note
//     （判定不了就不 fail-closed，避免把正常机器误判成坏引导）。
bool BcdOsEntryTargetsVolume(const std::string& enumText,
                             const std::vector<std::string>& candidates,
                             std::string* why);

// UEFI 固件下，**默认进入**的那个 winload 必须是 `winload.efi`。
// 若 BCD 是 `bcdboot /f BIOS`（或从 BIOS 机器带过来的）建的，路径会是
// `winload.exe` —— UEFI 的 bootmgr 加载它必然失败，表现为开机立刻重启
// （0xc000000e），数据面却"还原 100% 成功"。{bootmgr} 的 default 认不出来时
// 退化为"任一可达加载器是 .efi 即通过"。
bool BcdOsEntryIsUefiLoader(const std::string& enumText, std::string* why);

}  // namespace sysrecover
