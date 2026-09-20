// BCD 操作实现。
#include "bcd.h"

#include <algorithm>
#include <cctype>

#include "../common/process.h"

namespace sysrecover {
namespace {

bool ContainsGuid(const std::string& out, const std::wstring& guid) {
    // GUID 转窄串（纯 ASCII，直接截断即可）
    std::string narrow;
    narrow.reserve(guid.size());
    for (wchar_t c : guid)
        narrow.push_back(static_cast<char>(c));
    std::string a = out, b = narrow;
    std::transform(a.begin(), a.end(), a.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return a.find(b) != std::string::npos;
}

}  // namespace

bool BcdEntryExists(const std::wstring& guid) {
    std::string out;
    // CreateProcessW 直接传参，无 PowerShell 花括号问题（PIT-004）。
    RunProcess(SysToolPath(L"bcdedit.exe"), L"/enum " + guid, out);
    return ContainsGuid(out, guid);
}

bool BcdCreateBootsector(const std::wstring& guid, const std::wstring& desc,
                         wchar_t driveLetter, std::string& log) {
    std::string out;
    // 条目**已存在也不能跳过**：它可能是换机/换盘符/旧布局（救援文件曾放数据盘
    // D:）遗留下来的，device 还指着旧分区。若直接 return，就会出现"救援文件在
    // C:、BCD 条目 device=partition=D:"的错配 → bootmgr 去 D: 找 \grldr.mbr 找不到
    // → 报 0xc000000F（所需设备无法访问）。因此每次都必须把 device/path 刷成本次
    // 部署盘（幂等 = 自愈）。
    if (!BcdEntryExists(guid)) {
        // 关键：/application bootsector + path \grldr.mbr（PIT-001/002）。
        int rc = RunProcess(
            SysToolPath(L"bcdedit.exe"),
            L"/create " + guid + L" /d \"" + desc + L"\" /application bootsector",
            out);
        log += out;
        if (rc != 0)
            return false;
    } else {
        log += "entry exists, refresh device/path\n";
    }
    std::wstring dev = L"/set ";
    dev += guid;
    dev += L" device partition=";
    dev += driveLetter;
    dev += L":";
    int rc = RunProcess(SysToolPath(L"bcdedit.exe"), dev, out);
    log += out;
    if (rc != 0)
        return false;
    std::wstring path = L"/set " + guid + L" path \\grldr.mbr";
    rc = RunProcess(SysToolPath(L"bcdedit.exe"), path, out);
    log += out;
    if (rc != 0)
        return false;
    // 顺带刷新描述（旧条目可能残留旧版本文案）
    std::wstring descCmd = L"/set " + guid + L" description \"" + desc + L"\"";
    RunProcess(SysToolPath(L"bcdedit.exe"), descCmd, out);
    // displayorder：先摘（曾存在会报"找不到元素"，忽略），再 addlast 挂到末尾，
    // 避免重复调用 /addlast 在部分系统上对"已在列表中"的条目返回错误。
    RunProcess(SysToolPath(L"bcdedit.exe"),
               L"/displayorder " + guid + L" /remove", out);
    std::wstring order = L"/displayorder " + guid + L" /addlast";
    rc = RunProcess(SysToolPath(L"bcdedit.exe"), order, out);
    log += out;
    return rc == 0;
}

bool BcdSetBootsequence(const std::wstring& guid) {
    std::string out;
    return RunProcess(SysToolPath(L"bcdedit.exe"), L"/bootsequence " + guid, out) == 0;
}

bool BcdExport(const std::wstring& backupPath) {
    std::string out;
    return RunProcess(SysToolPath(L"bcdedit.exe"), L"/export \"" + backupPath + L"\"",
                      out) == 0;
}

}  // namespace sysrecover
