#pragma once
// 捕获排除配置（WimScript.ini 风格 [ExclusionList]），对齐旧原型 ExclusionService。
#include <string>
#include <vector>

namespace sysrecover {

// 默认排除清单（静态文本，含 [ExclusionList] 段头）。抽出来是为了可单元测试。
const char* DefaultExclusionConfig();

// 内置的云同步目录名（供调用方探测"源里是否存在"）。
const std::vector<std::wstring>& CloudFolderNames();

// **纯函数**：默认清单 + 已存在的云同步目录（调用方探测好传进来，便于测试）。
// 重复项只追加一次；返回内容以换行结尾。
std::string BuildExclusionContent(
    const std::vector<std::wstring>& presentCloudFolders);

// 生成"默认排除 + 源内已存在的云同步目录"配置文件，返回临时 ini 路径。
// 失败返回空串（此时捕获将不带排除项）。
std::wstring EnsureExclusionConfig(const std::wstring& sourceRoot);

}  // namespace sysrecover
