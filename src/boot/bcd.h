#pragma once
// BCD 操作（bcdedit 封装）。成熟方案见 AGENTS.md §7，严禁发明。
// 条目存在性：bcdedit /enum {GUID} 看输出是否回显 GUID（两者退出码都 0）。
#include <string>

namespace sysrecover {

// 固定恢复条目 GUID（与旧原型一致，便于对照）。
inline const wchar_t* RecoveryGuid() {
    return L"{12345678-1234-1234-1234-123456789abc}";
}

// 条目是否存在（ASCII GUID 匹配，不受代码页影响）。
bool BcdEntryExists(const std::wstring& guid);

// 创建实模式启动扇区条目：device partition=X: + path \grldr.mbr + displayorder。
// 已存在则直接返回 true。
bool BcdCreateBootsector(const std::wstring& guid, const std::wstring& desc,
                         wchar_t driveLetter, std::string& log);

// 单次启动（BIOS 路径；UEFI 用固件 BootNext，见 boot/uefi.h）。
bool BcdSetBootsequence(const std::wstring& guid);

// 备份 BCD（bcdedit /export），失败不致命。
bool BcdExport(const std::wstring& backupPath);

}  // namespace sysrecover
