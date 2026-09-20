#pragma once
// 捕获排除配置（WimScript.ini 风格 [ExclusionList]），对齐旧原型 ExclusionService。
#include <string>

namespace sysrecover {

// 生成"默认排除 + 源内已存在的云同步目录"配置文件，返回临时 ini 路径。
// 失败返回空串（此时捕获将不带排除项）。
std::wstring EnsureExclusionConfig(const std::wstring& sourceRoot);

}  // namespace sysrecover
