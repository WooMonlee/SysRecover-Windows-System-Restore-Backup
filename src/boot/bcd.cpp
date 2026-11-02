// BCD 操作实现。
#include "bcd.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "../common/process.h"
#include "bcd_parse.h"

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

// ── 任意 BCD 存储：产物断言 / 死条目清理（PIT-209）────────────────────
namespace {

std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
                        nullptr);
    return s;
}

std::wstring U2W8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string ToLowerAscii(std::string v) {
    for (auto& c : v)
        c = (char)std::tolower((unsigned char)c);
    return v;
}

// 一个 BCD 设备的三种状态：不存在 / 在但没有系统 / 在且有系统 / 判断不了。
enum class VolState { Missing, NoWindows, HasWindows, Unknown };

struct DriveItem {
    wchar_t letter = 0;
    std::wstring dev;  // \Device\HarddiskVolumeN
    std::wstring vol;  // \\?\Volume{GUID}\ （卷 GUID 路径）
};

struct VolMap {
    std::vector<DriveItem> items;
    void Build() {
        const DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'A'; c <= L'Z'; ++c) {
            if (!(mask & (1u << (c - L'A')))) continue;
            DriveItem it;
            it.letter = c;
            const std::wstring d(1, c);
            wchar_t dev[1024] = {};
            if (QueryDosDeviceW((d + L":").c_str(), dev, 1024) <= 0) continue;
            it.dev = dev;
            wchar_t vol[1024] = {};
            const std::wstring root = d + L":\\";
            if (GetVolumeNameForVolumeMountPointW(root.c_str(), vol, 1024))
                it.vol = vol;
            items.push_back(it);
        }
    }
};

VolState StateOnLetter(wchar_t letter) {
    const std::wstring root(1, letter);
    const std::wstring win = root + L":\\Windows";
    if (GetFileAttributesW((win + L"\\System32\\winload.exe").c_str()) !=
        INVALID_FILE_ATTRIBUTES)
        return VolState::HasWindows;
    const DWORD a = GetFileAttributesW(win.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return VolState::NoWindows;  // 分区在、没系统
    return VolState::HasWindows;  // 有 Windows 目录（精简镜像可能没 winload.exe）
}

// BCD 里的 device 值 → 这个分区到底还能不能启动。
VolState ClassifyDevice(const std::string& raw, const VolMap& map) {
    std::wstring v = U2W8(raw);
    const size_t b = v.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return VolState::Unknown;
    const size_t e = v.find_last_not_of(L" \t");
    v = v.substr(b, e - b + 1);
    const std::wstring kPart = L"partition=";
    if (v.size() >= kPart.size() &&
        _wcsnicmp(v.c_str(), kPart.c_str(), kPart.size()) == 0)
        v = v.substr(kPart.size());
    if (v.empty()) return VolState::Unknown;

    if (v.size() == 2 && v[1] == L':') {
        const wchar_t up = (wchar_t)towupper(v[0]);
        for (const auto& it : map.items)
            if (it.letter == up) return StateOnLetter(up);
        return VolState::Missing;  // 盘符当前不存在 = 那个分区已经不在了
    }
    const std::wstring kDev = L"\\Device\\HarddiskVolume";
    if (v.size() > kDev.size() &&
        _wcsnicmp(v.c_str(), kDev.c_str(), kDev.size()) == 0) {
        for (const auto& it : map.items)
            if (_wcsicmp(it.dev.c_str(), v.c_str()) == 0)
                return StateOnLetter(it.letter);
        // 没有盘符：只判"设备在不在"，不猜有没有系统
        const std::wstring dev = L"\\\\.\\" + v.substr(8);
        HANDLE h = CreateFileW(dev.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return VolState::Missing;
        CloseHandle(h);
        return VolState::Unknown;
    }
    if (v[0] == L'\\' && v.find(L"Volume{") != std::wstring::npos) {
        for (const auto& it : map.items)
            if (!it.vol.empty() && _wcsicmp(it.vol.c_str(), v.c_str()) == 0)
                return StateOnLetter(it.letter);
        if (GetFileAttributesW(v.c_str()) != INVALID_FILE_ATTRIBUTES)
            return VolState::Unknown;
        return VolState::Missing;
    }
    // "boot"（模板 BCD：随固件走）/ "ramdisk=..." / "unknown" → 不判
    return VolState::Unknown;
}

// 目标分区的三种写法（与 bcd_parse 的 candidates 约定一致）。
std::vector<std::string> VolumeIds(const std::wstring& winDir) {
    std::vector<std::string> ids;
    if (winDir.size() < 2 || winDir[1] != L':') return ids;
    const std::wstring dl(1, (wchar_t)towupper(winDir[0]));
    ids.push_back(W2U8(dl + L":"));
    wchar_t dev[1024] = {};
    if (QueryDosDeviceW((dl + L":").c_str(), dev, 1024) > 0)
        ids.push_back(W2U8(dev));
    wchar_t vol[1024] = {};
    const std::wstring root = dl + L":\\";
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), vol, 1024))
        ids.push_back(W2U8(vol));
    return ids;
}

std::wstring FirstOsDefaultDevice(const std::vector<BcdEntryInfo>& os) {
    for (const auto& e : os)
        if (e.isDefault) return U2W8(e.device);
    return os.empty() ? std::wstring() : U2W8(os.front().device);
}

}  // namespace

bool BcdEnumStore(const std::wstring& storePath, std::string& out,
                  std::string& log) {
    const int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                              L"/store \"" + storePath + L"\" /enum all", out);
    if (rc != 0) {
        log += "bcdedit /store enum failed rc=" + std::to_string(rc) + "\n";
        return false;
    }
    return true;
}

bool BcdDeleteEntry(const std::wstring& storePath, const std::wstring& guid,
                    std::string& log) {
    std::string out;
    const int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                              L"/store \"" + storePath + L"\" /delete " + guid +
                                  L" /f",
                              out);
    log += "bcdedit delete " + std::string(guid.begin(), guid.end()) +
           " rc=" + std::to_string(rc) + ": " + out + "\n";
    return rc == 0;
}

bool BcdSetStoreBootmgr(const std::wstring& storePath, const wchar_t* element,
                        const wchar_t* value, std::string& log) {
    std::string out;
    const int rc = RunProcess(
        SysToolPath(L"bcdedit.exe"),
        L"/store \"" + storePath + L"\" /set {bootmgr} " + element + L" " +
            value,
        out);
    log += "bcdedit set {bootmgr} " + W2U8(element) +
           " rc=" + std::to_string(rc) + "\n";
    return rc == 0;
}

bool BcdDelStoreBootmgr(const std::wstring& storePath, const wchar_t* element,
                        std::string& log) {
    std::string out;
    const int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                              L"/store \"" + storePath +
                                  L"\" /deletevalue {bootmgr} " + element,
                              out);
    log += "bcdedit deletevalue {bootmgr} rc=" + std::to_string(rc) + "\n";
    return rc == 0;
}

bool VerifyBcdStore(const std::wstring& storePath, const std::wstring& winDir,
                    bool uefi, std::string& detail) {
    std::string out;
    if (!BcdEnumStore(storePath, out, detail)) return false;
    std::string why;
    if (!BcdEnumShowsOsEntry(out, &why)) {
        detail = "BCD incomplete: " + why;
        return false;
    }
    const std::vector<BcdEntryInfo> os = BcdListOsEntries(out);
    const std::wstring dev = FirstOsDefaultDevice(os);
    // BIOS 模板 BCD 的 device 是 `boot`（随固件走）→ 不能按分区校验
    if (!dev.empty() && _wcsicmp(dev.c_str(), L"boot") != 0) {
        const std::vector<std::string> ids = VolumeIds(winDir);
        if (!BcdOsEntryTargetsVolume(out, ids, &why)) {
            detail = "BCD boot entry: " + why;
            return false;
        }
    }
    if (uefi && !BcdOsEntryIsUefiLoader(out, &why)) {
        detail = "BCD loader: " + why;
        return false;
    }
    return true;
}

int BcdPruneDeadEntries(const std::wstring& storePath,
                        const std::wstring& targetWinDir, int* outReachable,
                        std::string& log) {
    if (outReachable) *outReachable = -1;
    std::string out;
    if (!BcdEnumStore(storePath, out, log)) return -1;
    VolMap map;
    map.Build();
    // 目标系统的盘符（"C:"）—— 指向它的一律不删（BitLocker/离线读不到文件也不误杀）
    std::wstring tgtLetter;
    if (targetWinDir.size() >= 2 && targetWinDir[1] == L':')
        tgtLetter = std::wstring(1, (wchar_t)towupper(targetWinDir[0])) + L":";
    auto isTargetDevice = [&](const std::string& dev) {
        if (tgtLetter.empty()) return false;
        std::wstring v = U2W8(dev);
        const std::wstring kPart = L"partition=";
        if (v.size() >= kPart.size() &&
            _wcsnicmp(v.c_str(), kPart.c_str(), kPart.size()) == 0)
            v = v.substr(kPart.size());
        return !v.empty() && _wcsicmp(v.c_str(), tgtLetter.c_str()) == 0;
    };
    int removed = 0;
    for (const auto& e : BcdListEntries(out)) {
        if (!e.reachable || e.isDefault) continue;  // 默认项永不删（重建交给 bcdboot）
        if (isTargetDevice(e.device)) continue;     // 目标系统自己的条目
        const std::string pathLow = ToLowerAscii(e.path);
        const std::string descLow = ToLowerAscii(e.description);
        if (pathLow.find("zjrestore") != std::string::npos ||
            descLow.find("sysrecover") != std::string::npos)
            continue;  // 我们自己的条目由专门逻辑管
        const VolState st = ClassifyDevice(e.device, map);
        const bool kill = (st == VolState::Missing) ||
                          (e.isLoader && st == VolState::NoWindows);
        if (!kill) continue;
        char line[512];
        snprintf(line, sizeof(line), "prune: delete %s (%s) device=%s\n",
                 e.id.c_str(),
                 st == VolState::Missing ? "partition gone" : "no Windows",
                 e.device.empty() ? "?" : e.device.c_str());
        log += line;
        if (BcdDeleteEntry(storePath, U2W8(e.id), log)) ++removed;
    }
    std::string after;
    if (BcdEnumStore(storePath, after, log) && outReachable) {
        int n = 0;
        for (const auto& e : BcdListOsEntries(after))
            if (e.reachable) ++n;
        *outReachable = n;
    }
    if (removed)
        log += "prune: removed " + std::to_string(removed) +
               " dead boot entr(ies)\n";
    return removed;
}

bool BcdQuietSingleOsMenu(const std::wstring& storePath, std::string& log) {
    std::string out;
    if (!BcdEnumStore(storePath, out, log)) return false;
    // 只在"displayorder 里真的只有一条、且它就是一个 Windows 系统"时收敛 ——
    // 有 GRUB4DOS 常驻菜单/多系统的机器一律不动（宁可多等 1 秒，不误删入口）。
    int total = 0;
    std::string keep;
    bool isLoader = false;
    for (const auto& e : BcdListEntries(out)) {
        if (!e.reachable) continue;
        ++total;
        keep = e.id;
        isLoader = e.isLoader;
    }
    if (total != 1 || !isLoader) {
        log += "menu: keep (displayorder entries=" + std::to_string(total) +
               ")\n";
        return false;
    }
    BcdSetStoreBootmgr(storePath, L"displayorder", U2W8(keep).c_str(), log);
    BcdSetStoreBootmgr(storePath, L"timeout", L"0", log);
    BcdDelStoreBootmgr(storePath, L"displaybootmenu", log);
    log += "menu: single OS -> no boot menu wait\n";
    return true;
}

}  // namespace sysrecover
