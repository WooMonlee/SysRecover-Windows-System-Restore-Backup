#pragma once
// 快捷方式创建（IShellLink COM，无 PowerShell）。
#include <string>

namespace sysrecover {

// 在 folder 目录创建 name.lnk → target targetArgs。
// 成功返回 true（message 含完整路径），失败返回 false。
bool CreateShortcut(const std::wstring& folder, const std::wstring& name,
                    const std::wstring& target, const std::wstring& targetArgs,
                    std::wstring& outPath);

}  // namespace sysrecover
