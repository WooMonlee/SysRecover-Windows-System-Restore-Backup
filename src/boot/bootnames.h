#pragma once
// 引导条目的显示名（ASCII）：
//   Menu = 用户显式「安装菜单」的常驻条目；
//   Temporary = 日常还原的一次性条目，首启由“恢复引导菜单”清理。
// 两者必须使用不同名称——清理只认 Temporary，绝不能误删常驻菜单。
namespace sysrecover {

inline constexpr wchar_t kMenuEntryDesc[] = L"SysRecover";
inline constexpr wchar_t kMenuBcdDesc[] = L"SysRecover Recovery Environment";
inline constexpr wchar_t kTempEntryDesc[] = L"SysRecover Temporary Restore";

}  // namespace sysrecover
