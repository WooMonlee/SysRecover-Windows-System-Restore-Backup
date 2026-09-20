#pragma once
// 还原安全检查（fail-closed）。四项见 AGENTS.md §11。
#include <string>

#include "../disk/disk.h"

namespace sysrecover {

// 返回空串=通过，否则为拒绝原因（UTF-8 中文）。
std::string CheckRestoreTarget(const PartitionInfo& p,
                               const std::wstring& imagePath);

}  // namespace sysrecover
