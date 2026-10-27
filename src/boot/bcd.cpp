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

int BcdGetBootmgrTimeout(std::string& log) {
    std::string out;
    RunProcess(SysToolPath(L"bcdedit.exe"), L"/enum {bootmgr}", out);
    int v = ParseBootmgrTimeout(out);
    if (v < 0)
        log += out;  // 解析不到 → 把原文留进日志，便于排查本地化/输出差异
    return v;
}

bool BcdSetBootmgrTimeout(int seconds, std::string& log) {
    // 写后读回校验（用户 2026-10-27 需求）：装了金山毒霸/360 一类底层拦截的机器上，
    // bcdedit 可能返回 0 却没真正落盘 → 常驻菜单 timeout 仍是 0 → 开机按什么都不停。
    // 失败重试一次（首次写入常触发安全软件首次询问/拦截），仍不符才判失败。
    for (int attempt = 1; attempt <= 2; ++attempt) {
        std::string out;
        int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                            L"/set {bootmgr} timeout " + std::to_wstring(seconds),
                            out);
        log += "bcdedit timeout set rc=" + std::to_string(rc) + " (attempt " +
               std::to_string(attempt) + "): " + out;
        if (rc != 0) continue;
        std::string rlog;
        int back = BcdGetBootmgrTimeout(rlog);
        log += "timeout readback=" + std::to_string(back) + "\n";
        if (back == seconds) return true;
        log += "timeout verify FAILED (wanted " + std::to_string(seconds) +
               ", got " + std::to_string(back) + ")\n";
    }
    return false;
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
    if (rc != 0) return false;
    // 产物复核：本轮全部命令都返回 0 也可能被安全软件"假放行"（用户 2026-10-27 需求）。
    if (!BcdEntryExists(guid)) {
        log += "entry verify FAILED: bcdedit /enum does not echo this guid\n";
        return false;
    }
    log += "entry verify ok\n";
    return true;
}

std::string BcdGetBootsequence(std::string& log) {
    std::string out;
    RunProcess(SysToolPath(L"bcdedit.exe"), L"/enum {bootmgr}", out);
    std::string v = ParseBootsequence(out);
    if (v.empty())
        log += "bootsequence readback: none\n";
    else
        log += "bootsequence readback: " + v + "\n";
    return v;
}

bool BcdSetBootsequence(const std::wstring& guid, std::string& log) {
    // 写后读回校验（用户 2026-10-27 需求）：见 BcdSetBootmgrTimeout 的注释。
    // 这是"重启后能不能进救援层"的唯一开关 —— 被安全软件静默拦掉就一定会
    // 重启进原系统，D: 还白写了契约。故必须读回确认，失败重试一次。
    for (int attempt = 1; attempt <= 2; ++attempt) {
        std::string out;
        int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                            L"/bootsequence " + guid, out);
        log += "bcdedit /bootsequence rc=" + std::to_string(rc) +
               " (attempt " + std::to_string(attempt) + "): " + out;
        if (rc != 0) continue;
        std::string rlog;
        std::string back = BcdGetBootsequence(rlog);
        log += rlog;
        if (SameGuidAscii(back, guid)) return true;
        log += "bootsequence verify FAILED (wanted " +
               std::string(guid.begin(), guid.end()) + ", got " +
               (back.empty() ? std::string("<none>") : back) + ")\n";
    }
    return false;
}

bool BcdExport(const std::wstring& backupPath) {
    std::string out;
    return RunProcess(SysToolPath(L"bcdedit.exe"), L"/export \"" + backupPath + L"\"",
                      out) == 0;
}

bool BcdSetSafeBoot(bool enable, std::string& log) {
    std::string out;
    int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                        enable ? L"/set {current} safeboot minimal"
                               : L"/deletevalue {current} safeboot",
                        out);
    log += std::string("bcdedit safeboot ") + (enable ? "set" : "clear") +
           " rc=" + std::to_string(rc) + ": " + out + "\n";
    return rc == 0;
}

}  // namespace sysrecover
