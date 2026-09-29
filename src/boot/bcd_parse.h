#pragma once
// `bcdedit /enum` 输出解析（纯文本逻辑，无 Win32 依赖 → 可单测）。
//
// 为什么需要它（docs/15 启动故障分析）：`bcdboot` 失败时**不会**报错到我们脸上
// （返回码曾被丢弃），而"写没写成"唯一的可靠判据是**产物**：ESP 上的 BCD 里
// 有没有可引导的条目。这里放判定用的纯逻辑，真正的 IO 在
// `boot/uefi.cpp::VerifyEspBcd()`。
#include <string>

namespace sysrecover {

// enum 文本是否表明"这个 BCD 里有一个能用 Windows 启动项"：
//   ① {bootmgr} 的 displayorder 存在，且至少跟一个 {GUID}；
//   ② 文本里存在 Windows Boot Loader（path 含 `winload.`）。
// 不成立时把**原因**写进 why（英文，进日志/详情，不面向最终用户）。
bool BcdEnumShowsOsEntry(const std::string& enumText, std::string* why);

}  // namespace sysrecover
