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
    if (BcdEntryExists(guid)) {
        log += "entry exists, skip create\n";
        return true;
    }
    std::string out;
    // 关键：/application bootsector + path \grldr.mbr（PIT-001/002）。
    int rc = RunProcess(
        SysToolPath(L"bcdedit.exe"),
        L"/create " + guid + L" /d \"" + desc + L"\" /application bootsector",
        out);
    log += out;
    if (rc != 0)
        return false;
    std::wstring dev = L"/set ";
    dev += guid;
    dev += L" device partition=";
    dev += driveLetter;
    dev += L":";
    rc = RunProcess(SysToolPath(L"bcdedit.exe"), dev, out);
    log += out;
    if (rc != 0)
        return false;
    std::wstring path = L"/set " + guid + L" path \\grldr.mbr";
    rc = RunProcess(SysToolPath(L"bcdedit.exe"), path, out);
    log += out;
    if (rc != 0)
        return false;
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
