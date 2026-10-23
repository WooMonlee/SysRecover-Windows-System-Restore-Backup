#pragma once
// 开机「快捷启动键」识别（品牌/机型 → 按键），供 GUI/CLI 提示文案使用。
// 表移植自本仓 BiosKC 项目（同一作者）：同款子串匹配、同款表顺序优先级。
// 纯注册表读取（HARDWARE\DESCRIPTION\System\BIOS，**禁用 WMI**，PE 可用）。
#include <string>

namespace sysrecover {

// 纯逻辑查表：四个标识串任意大小写、可为空；按「主板厂商 → 主板型号 →
// 整机厂商 → 整机型号」顺序扫，表内顺序即优先级（同 BiosKC 的 match()）。
// 返回键文案（如 L"F9" / L"F12 or Fn+F12"）；返回空串 = 表里没把握。
// vendor（可空）回填识别到的品牌名。
std::wstring LookupBootMenuKey(const std::wstring& baseBoardManufacturer,
                               const std::wstring& baseBoardProduct,
                               const std::wstring& systemManufacturer,
                               const std::wstring& systemProduct,
                               std::wstring* vendor = nullptr);

// 读本机标识并查表（结果缓存）。vendor（可空）回填品牌名。
std::wstring LocalBootMenuKey(std::wstring* vendor = nullptr);

// 展示用文案：识别到就返回该键（多键 " or " 按当前语言换成 " 或 "）；
// 识别不到返回 Tr(L"快捷启动键")（中性措辞，绝不臆测 F12）。结果缓存。
std::wstring BootMenuKeyHint();

}  // namespace sysrecover
