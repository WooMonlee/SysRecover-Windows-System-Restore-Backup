// 备份/还原共享操作层实现。语义对齐 CLI（Phase 2-4 已验证）与旧 C# 编排器。
#include "../common/i18n.h"
#include "ops.h"
#include "shortcut.h"
#include "swapleft.h"
#include "universal.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <vector>

#include "../boot/bcd.h"
#include "../boot/bootfix.h"
#include "../boot/bootpath.h"
#include "../boot/bootself.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/ea.h"
#include "../common/crypt.h"
#include "../common/logger.h"
#include "../common/pathutil.h"
#include "../common/cpucap.h"
#include "../common/process.h"
#include "../common/progress.h"
#include "../common/refscan.h"
#include "../common/relocate.h"
#include "../common/selfarch.h"
#include "../common/singleton.h"
#include "../common/vss.h"
#include "../common/version.h"
#include "../disk/disk.h"
#include "../disk/mbrcode.h"
#include "../wim/exclude.h"
#include "safety.h"
#include "selfdiag.h"
#include "imgsrc.h"

namespace sysrecover {
namespace {

// 网络路径判定：UNC（\\server\share\...）或**映射网络驱动器**（DRIVE_REMOTE）。
// 用途：**暂存+重启**的还原由 Linux 救援层读镜像，而救援层**访问不到网络** →
// 网络镜像必然 image not found，必须在格式化之前就拦下（问题清单 D1/H1）。
bool IsNetworkPath(const std::wstring& p) {
    if (p.size() >= 2 && (p[0] == L'\\' || p[0] == L'/') &&
        (p[1] == L'\\' || p[1] == L'/'))
        return true;  // \\server\share\...
    if (p.size() >= 2 && p[1] == L':') {
        wchar_t root[4] = {p[0], L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_REMOTE)
            return true;
    }
    return false;
}

// 用户显式「安装菜单」的标记。日常还原的临时项用另一个名字/GUID，
// 首启清理只认临时项；这个标记只用于“已安装常驻菜单”的判重，
// 避免一次日常还原静默覆盖用户菜单。
std::wstring MenuFlagPath() {
    return ContractDir() + L"\\restore-menu.flag";
}

bool HasInstalledMenu() {
    DWORD a = GetFileAttributesW(MenuFlagPath().c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void SetMenuFlag(bool installed) {
    std::wstring p = MenuFlagPath();
    if (!installed) {
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(p.c_str());
        return;
    }
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LogWarn("menu flag write failed err=" + std::to_string(GetLastError()));
        return;
    }
    const char k[] = "installed\n";
    DWORD wr = 0;
    WriteFile(h, k, (DWORD)sizeof(k) - 1, &wr, nullptr);
    CloseHandle(h);
}

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

// UTF-8 → 宽串（读 restore-task.conf 里的路径用，conf 是 W2U 写出来的）。
std::wstring U2W(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归删除目录树（EA 临时目录清理用；失败不致命）。
// 同 swapleft.cpp DeleteRec 的修法（PIT 用户 2026-10-15）：清属性 + 跳过 junction。
void DeleteTreeRec(const std::wstring& dir) {
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            // junction/symlink：只删链接本身，绝不递归进目标
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                RemoveDirectoryW(p.c_str()) || DeleteFileW(p.c_str());
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTreeRec(p);
                continue;
            }
            if (!DeleteFileW(p.c_str())) {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
    RemoveDirectoryW(dir.c_str());
}

// 递归复制目录（软件自保用；不处理 ACL）。
bool CopyDir(const std::wstring& src, const std::wstring& dst) {
    if (!IsDir(src))
        return false;
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ok &= CopyDir(s, d);
        else if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
            ok = false;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool SameDrive(wchar_t a, wchar_t b) {
    return towupper(a) == towupper(b);
}

// 0.9.38：ESP 诊断必须有不依赖主日志的落点。主日志在部分 PE 的 support 包里
// 会缺失（GUI/CLI 日志根或文件共享问题），所以这里直接写 `ESP-diag.txt`：
// 同时写“日志根\logs”和“程序目录\logs”，support 的 logs 树一定能收到。
void EspDiagAppendAt(const std::wstring& dir, const std::string& text) {
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = dir + L"\\ESP-diag.txt";
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &wrote, nullptr);
    CloseHandle(h);
}

void EspDiagLine(const std::string& s) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char ts[64] = {};
    snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string line = std::string(ts) + s + "\r\n";
    const std::wstring primary = LogBaseDir() + L"\\logs";
    const std::wstring app = AppDir() + L"\\logs";
    EspDiagAppendAt(primary, line);
    if (_wcsicmp(primary.c_str(), app.c_str()) != 0)
        EspDiagAppendAt(app, line);
    LogFileInfo("ESP-DIAG " + s);
}

void EspDiagVolumes() {
    EspDiagLine("--- logical drives ---");
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        std::wstring root = std::wstring(1, c) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        wchar_t label[64] = {}, fs[32] = {};
        GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr,
                              nullptr, fs, 32);
        ULARGE_INTEGER total = {};
        GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr);
        wchar_t vn[MAX_PATH] = {};
        GetVolumeNameForVolumeMountPointW(root.c_str(), vn, MAX_PATH);
        char line[512] = {};
        snprintf(line, sizeof(line),
                 "%c: type=%u label=%ls fs=%ls total=%llu volume=%ls",
                 (char)c, type, label, fs,
                 (unsigned long long)total.QuadPart, vn);
        EspDiagLine(line);
    }
}


// 目标盘的分区风格（找不到该盘 → Unknown）。
PartitionStyle DiskStyleOf(uint32_t diskIndex) {
    for (const auto& d : EnumerateDisks())
        if (d.index == diskIndex)
            return d.style;
    return PartitionStyle::Unknown;
}

// ②b：目标盘 ESP **无盘符**时按**卷 GUID** 直接挂（GPT 下卷 GUID = 分区 GUID，
// 由 GuidToString 给出、已去花括号小写；mountvol 只认 `\\?\Volume{...}` 格式，
// GUID 解析大小写不敏感，这里仍转大写求稳）。多盘机「还原到非固件盘」时 ①（固件
// ESP 在别的盘）被盘号校验拒掉、③ 又要求有盘符 —— 没有 ②b 就只能硬失败。
bool MountEspByGuid(const std::wstring& volumeOrGuid, std::wstring& root,
                    std::string& log) {
    if (volumeOrGuid.empty())
        return false;
    // volumeName 来自 FindFirstVolume，已经是 \\?\Volume{...}\；只有传分区
    // GUID 时（老调用/兜底）才拼出卷名。0.9.33：刚分区的 ESP 用真实卷名
    // 才能立即挂上，分区 GUID 在少数 PE/盘上要等重启后的 mount manager。
    std::wstring vn = volumeOrGuid;
    if (vn.rfind(L"\\\\?\\", 0) != 0) {
        std::wstring g = volumeOrGuid;
        for (auto& ch : g)
            ch = (wchar_t)towupper(ch);
        vn = L"\\\\?\\Volume{" + g + L"}\\";
    }
    if (!vn.empty() && vn.back() != L'\\')
        vn += L'\\';
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'S'; c <= L'Z'; ++c) {
        if (mask & (1u << (c - L'A')))
            continue;
        std::wstring r = std::wstring(1, c) + L":\\";
        // 0.9.33：优先用 Win32 SetVolumeMountPointW，不依赖外部的
        // mountvol.exe —— 部分精简 PE 根本没有 mountvol，表现为“找不到 ESP”，
        // 换到正常 Windows 重试又正常。
        if (SetVolumeMountPointW(r.c_str(), vn.c_str())) {
            if (GetFileAttributesW(r.c_str()) != INVALID_FILE_ATTRIBUTES) {
                root = r;
                log += "ESP mounted by volume at ";
                log += (char)c;
                log += ":\\ (Win32)\n";
                return true;
            }
            DeleteVolumeMountPointW(r.c_str());
        }
        std::string out;
        std::wstring args = std::wstring(1, c) + L": " + vn;
        if (RunProcess(SysToolPath(L"mountvol.exe"), args, out) != 0)
            continue;
        if (GetFileAttributesW(r.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::string o2;  // 挂上了却看不到内容 → 撤掉换下一个空闲符
            RunProcess(SysToolPath(L"mountvol.exe"),
                       std::wstring(1, c) + L": /d", o2);
            continue;
        }
        root = r;
        log += "ESP mounted by volume at ";
        log += (char)c;
        log += ":\\ (mountvol)\n";
        return true;
    }
    return false;
}

// 0.9.37：PE 10586 这类 PE 的卷枚举有时拿不到“卷 ↔ 分区”的 extent 映射，
// 即使 Explorer 里 ESP 明明有 E: 盘符，`PartitionInfo.letter` 仍为空。
// 最后按 ESP 的卷标 `ESP` + FAT 文件系统 + 分区大小核对，直接用已有盘符。
// 只接受大小与目标 ESP 分区一致的 FAT 卷，避免误选数据盘。
std::wstring FindEspDriveByLabel(const PartitionInfo& esp) {
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'D'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        std::wstring root = std::wstring(1, c) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_CDROM || type == DRIVE_REMOTE ||
            type == DRIVE_NO_ROOT_DIR)
            continue;
        wchar_t label[64] = {}, fs[32] = {};
        if (!GetVolumeInformationW(root.c_str(), label, 64, nullptr, nullptr,
                                   nullptr, fs, 32))
            continue;
        if (_wcsicmp(label, L"ESP") != 0) continue;
        if (_wcsicmp(fs, L"FAT32") != 0 && _wcsicmp(fs, L"FAT") != 0)
            continue;
        ULARGE_INTEGER total = {};
        if (esp.sizeBytes > 0) {
            // PE10586 实测：300MB 的 ESP，p1=314,572,800 字节，E: 的
            // GetDiskFreeSpaceEx total=310,378,496（差 4MB，FAT32/分区保留区
            // 与分区长度本来就不完全相等）。允许 16MB 误差，避免把正确 ESP
            // 误判成“大小不匹配”。
            const unsigned long long kTolerance = 16ull << 20;
            if (!GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr) ||
                total.QuadPart == 0)
                continue;
            const unsigned long long diff =
                total.QuadPart > esp.sizeBytes
                    ? total.QuadPart - esp.sizeBytes
                    : esp.sizeBytes - total.QuadPart;
            if (diff > kTolerance)
                continue;
        }
        return root;
    }
    return L"";
}

// 取一个**可用的 ESP 根**（形如 "X:\\"）。顺序（docs/15 · P4 + Q2 盘号限定，
// 2026-10-18）：
//   ① `mountvol <空闲盘符>: /s` —— 固件**真正启动**的那个分区。关键点：它按固件
//      启动项找，**不依赖分区类型 GUID**，所以 DiskGenius 把 ESP 标成 Basic Data
//      时它照样挂得上。**Q2：targetDisk>=0 时挂完校验盘号**（VolumeDiskIndex =
//      IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）—— 多盘机固件 ESP 可能在别的盘，
//      往那儿写引导 = 写错盘；**证实**在别的盘 → UnmountEsp + 清空 espRoot 续查
//      ②③（UnmountEsp 是 const 引用、不清字符串，必须手动 clear）；盘号取不到
//      （-1）→ 保留挂载 + WARN（fail-open：ioctl 失败极罕见，不能因它把合法还原
//      卡死）。
//   ② 目标盘上类型 GUID 认对的 ESP：**有盘符**直接用（mountvol 失败时的兜底）；
//      **无盘符** → ②b 按卷 GUID 挂载（见上）。
//   ③ 回退：目标盘 FAT 分区且根下有 \EFI\Microsoft\Boot\bootmgfw.efi（故障 B 的正解）。
// targetDisk>=0 且三路都失败 → false（**绝不回退到错盘**）；-1 = 不限盘
// （备份本机 ESP 等，行为与 Q2 之前完全一致）。
// guidMatched=true 表示"GPT 类型 GUID 也认对了"（正常机器；false 时调用方只记警告）。
bool AcquireEspRoot(std::wstring& espRoot, bool& guidMatched, std::string& log,
                    int targetDisk = -1) {
    // 0.9.33（用户 2026-10-26 实测）：刚用向导建好分区的磁盘，Windows 的
    // mount manager 偶尔要过几百毫秒才把 ESP 卷注册完整 —— 第一次找 ESP
    // 失败、重启后同一套分区就正常。这里重试三次（并每次重新枚举，
    // 等卷名/盘符出现），而不是一次失败就直接判死。
    for (int attempt = 0; attempt < 3; ++attempt) {
        EspDiagLine("AcquireEspRoot start attempt=" + std::to_string(attempt + 1) +
                    " targetDisk=" + std::to_string(targetDisk));
        EspDiagVolumes();
        EspDiagLine("partitions: " + W2U(DescribePartitions()));
        PartitionInfo esp;
        guidMatched = FindEspPartition(esp, targetDisk);
        EspDiagLine("FindEspPartition matched=" +
                    std::string(guidMatched ? "1" : "0") +
                    " letter=" + W2U(esp.letter) +
                    " volumeName=" + W2U(esp.volumeName) +
                    " guid=" + W2U(esp.guid) +
                    " offset=" + std::to_string(esp.offsetBytes) +
                    " size=" + std::to_string(esp.sizeBytes));
        // ① 固件 ESP（mountvol /s）+ 盘号校验
        espRoot = MountEsp(log);
        EspDiagLine("MountEsp /s -> " + W2U(espRoot));
        if (!espRoot.empty()) {
            if (targetDisk >= 0) {
                int d = VolumeDiskIndex(espRoot[0]);
                if (d >= 0 && d != targetDisk) {
                    log += "mounted ESP is on disk " + std::to_string(d) +
                           " but target is disk " + std::to_string(targetDisk) +
                           "; unmount, look on target disk\n";
                    UnmountEsp(espRoot, log);
                    espRoot.clear();
                } else if (d < 0) {
                    log += "WARN: cannot verify mounted ESP disk index; keeping it\n";
                    EspDiagLine("MountEsp disk index unknown; keeping root");
                }
            }
            if (!espRoot.empty()) {
                EspDiagLine("AcquireEspRoot OK via mountvol/s: " + W2U(espRoot));
                return true;
            }
        }
        // ② 目标盘 GUID ESP：有盘符
        if (guidMatched && !esp.letter.empty()) {
            std::wstring root = esp.letter + L":\\";
            if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) {
                espRoot = root;
                log += "ESP via existing drive letter\n";
                EspDiagLine("AcquireEspRoot OK via existing letter: " + W2U(root));
                return true;
            }
            EspDiagLine("existing ESP letter not accessible: " + W2U(root));
        }
        // ②b 目标盘 GUID ESP：无盘符 → 用真实卷名挂（卷名取不到才退回
        //     partition GUID）。这是刚分区 ESP 的正解。
        if (guidMatched && esp.letter.empty()) {
            const std::wstring& volume =
                esp.volumeName.empty() ? esp.guid : esp.volumeName;
            EspDiagLine("trying MountEspByGuid volume=" + W2U(volume));
            if (MountEspByGuid(volume, espRoot, log)) {
                EspDiagLine("AcquireEspRoot OK via volume GUID: " + W2U(espRoot));
                return true;
            }
        }
        // ②c PE 卷枚举拿不到 letter 时的兜底：已有盘符 + 卷标 ESP + 大小核对。
        if (guidMatched) {
            const std::wstring byLabel = FindEspDriveByLabel(esp);
            if (!byLabel.empty()) {
                espRoot = byLabel;
                log += "ESP via label/size fallback at ";
                log += (char)espRoot[0];
                log += ":\\\n";
                EspDiagLine("AcquireEspRoot OK via label/size: " + W2U(byLabel));
                return true;
            }
            EspDiagLine("label/size fallback found nothing");
        }
        // ③ 回退（仍限定目标盘）
        PartitionInfo fb;
        if (FindEspPartitionFallback(fb, targetDisk) && !fb.letter.empty()) {
            espRoot = fb.letter + L":\\";
            log += "ESP via fallback (FAT partition with \\EFI boot files)\n";
            EspDiagLine("AcquireEspRoot OK via FAT/EFI fallback: " + W2U(espRoot));
            return true;
        }
        EspDiagLine("AcquireEspRoot attempt " + std::to_string(attempt + 1) +
                    " failed; retry/backoff");
        if (attempt < 2) {
            log += "ESP acquire retry " + std::to_string(attempt + 1) +
                   "/3 (volume not ready)\n";
            Sleep(attempt == 0 ? 300 : 700);
        }
    }
    EspDiagLine("AcquireEspRoot FAILED targetDisk=" + std::to_string(targetDisk) +
                " partitions=" + W2U(DescribePartitions()));
    return false;
}

// BIOS 就地还原：目标自身已有可引导的 BCD（bootmgr + \Boot\BCD + winload 条目）
// → 跳过 bcdboot，保住第三方启动项（0.6.35，与 UEFI 路径同规则）。
// 2026-10-27（用户反馈 2）：极简 PE 没有 bcdedit.exe → **退化为文件级判定**
// （BCD 里 UTF-16LE 出现 winload 即认为有条目），否则永远判 invalid、
// 白白去跑不存在的 bcdboot。
bool BiosBcdLooksValid(wchar_t letter, std::string& detail) {
    std::wstring root = std::wstring(1, letter) + L":\\";
    bool bootmgr = GetFileAttributesW((root + L"bootmgr").c_str()) !=
                   INVALID_FILE_ATTRIBUTES;
    std::wstring bcdPath = root + L"Boot\\BCD";
    bool bcd = GetFileAttributesW(bcdPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::string out;
    int rc = 0;
    bool winload = false;
    bool haveBcdEdit = false;
    if (bcd) {
        const std::wstring bcdedit = SysToolPath(L"bcdedit.exe");
        haveBcdEdit = GetFileAttributesW(bcdedit.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (haveBcdEdit) {
            rc = RunProcess(bcdedit,
                            L"/store " + bcdPath + L" /enum {default}", out);
            winload = rc == 0 && out.find("winload") != std::string::npos;
        } else {
            std::string fd;
            winload = BcdFileLooksUsable(bcdPath, fd);
            out = fd;
        }
    }
    detail = "bootmgr=" + std::to_string((int)bootmgr) +
             " bcd=" + std::to_string((int)bcd) +
             " bcdedit=" + std::to_string((int)haveBcdEdit) +
             " enum_rc=" + std::to_string(rc) +
             " winload=" + std::to_string((int)winload) +
             (out.empty() ? "" : " (" + out + ")");
    return bootmgr && bcd && winload;
}

// 系统工具是否可用。极简 PE（Win8PE/NewBee 等）里 bcdboot / bcdedit / bootsect
// 常常一个都没有 —— 引导必须能靠随包资产自足（2026-10-27，用户反馈 2）。
bool ToolAvailable(const wchar_t* exe) {
    return GetFileAttributesW(SysToolPath(exe).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 就地还原：快速格式化 → wimlib 目录模式应用（PIT-008）→ 镜像带 ESP 子镜像
// 就恢复 ESP → 系统镜像才 bcdboot 修引导。**全程不重启**。
int RunDirectRestore(const RestoreRequest& req, const PartitionInfo& target,
                     const std::wstring& imagePath, int espIndex, int eaIndex,
                     std::string& err, ProgressFn progress) {
    std::wstring letter(1, target.letter[0]);
    LogInfo("in-place restore -> " + W2U(letter) + ": (" + W2U(imagePath) +
            ", index=" + std::to_string(req.index) + ")");
    // 1) 快速格式化（不传 /V：保留原卷标）
    {
        std::string out;
        int rc = RunProcess(SysToolPath(L"format.com"),
                            letter + L": /FS:NTFS /Q /Y", out);
        LogInfo("format.com rc=" + std::to_string(rc) + " / " + out);
        if (rc != 0) {
            err = Tr("就地还原：格式化目标分区失败（") + out + Tr("）");
            LogError(err);
            ProgressDone("restore", "failed");
            return 1;
        }
    }
    // 2) 应用镜像（目录模式）
    WimEngine wim;
    if (!wim.ok()) {
        err = Tr("wimlib 初始化失败");
        ProgressDone("restore", "failed");
        return 1;
    }
    int arc = wim.Apply(imagePath, req.index, letter + L":\\",
                        [progress](int pct, const std::string& st) {
                            ProgressUpdate("restore", pct, st);
                            // 转投给上层（GUI 用来刷新进度条/百分比）；返回 true = 中止。
                            if (progress)
                                return progress(pct, st);
                            return false;
                        });
    if (arc != 0) {
        err = Tr("就地还原：应用镜像失败 rc=") + std::to_string(arc) + " (" +
              W2U(WimEngine::ErrorString(arc)) + ")";
        LogError(err);
        ProgressDone("restore", "failed");
        return 1;
    }
    // 异机还原（2026-10-21）：Windows 侧 apply 完、修引导之前注入启动驱动。
    if (req.universal) {
        std::string ulog;
        std::wstring uerr;
        auto uProgress = [&](int pct, const std::string& st) {
            ProgressUpdate("restore", pct, st);
            return progress ? progress(pct, st) : false;
        };
        if (!PrepareUniversal(letter + L":", req.driverDir, ulog, uerr,
                              uProgress)) {
            err = Tr("就地还原已完成，但异机还原准备失败：") + W2U(uerr);
            LogError(err);
            LogError("universal log:\n" + ulog);
            ProgressDone("restore", "failed");
            return 1;
        }
        LogInfo("universal: in-place prepared\n" + ulog);
    }
    // 2.5) ESP 子镜像恢复（方案 C · 就地还原侧；2026-09-30 用户规格：
    //      "还原时根据其备份的ESP分区自动还原ESP"）。与救援层同款语义：
    //      镜像里带 ESP 子镜像（备份时勾了 --esp，任务 esp_index>0）且本机
    //      找得到 ESP → 目录模式 apply（只加不删；BCD 捕获时已排除）。
    //      · 放在 bcdboot **之前** —— 让 bcdboot 最后写，引导文件/BCD 权威；
    //      · **非致命**：失败/找不到 ESP 只记日志，主系统已还原不动摇
    //        （对齐救援层 `say ERROR` 不中断主流程的先例）；
    //      · 传空进度回调：ESP 很小、瞬间完成，不回卷 GUI 进度条。
    if (espIndex > 0) {
        std::wstring espRoot;
        bool guidMatched = false;
        std::string blog;
        if (AcquireEspRoot(espRoot, guidMatched, blog, (int)target.diskIndex)) {
            int erc = wim.Apply(imagePath, espIndex, espRoot, ProgressFn{});
            LogInfo("esp subimage apply (in-place) idx=" +
                    std::to_string(espIndex) + " -> " + W2U(espRoot) +
                    " rc=" + std::to_string(erc));
            if (erc != 0)
                LogWarn("ESP subimage restore failed rc=" +
                        std::to_string(erc) +
                        " (main system restored; ESP not updated)");
            UnmountEsp(espRoot, blog);
        } else {
            LogWarn("task has esp subimage (idx " + std::to_string(espIndex) +
                    ") but no ESP partition found (skip); " + blog);
        }
    }
    // 2.6) ZJEA 修复子镜像（PIT-122 / PIT-136；1019 案 F4 补上**就地还原侧**）：
    //      Windows 侧 apply 原生保 EA（PIT-120），但"被排除的非微软重解析点
    //      文件"（PIT-135）与整体 EA 修复数据都在 ZJEA 子镜像里 —— 就地还原
    //      原先完全没走这条链。语义与救援层一致：解出 eapack/reppack 当场
    //      写回（本进程在正常 Windows / PE，Windows API 直接可用，无需补写器
    //      与 GPO 钩子）；失败**非致命**（主系统已还原不动摇，只记日志）。
    //      顺序：先 reppack（重建被排除的文件）再 eapack（其 EA 才找得到文件）。
    if (eaIndex > 0) {
        std::wstring tmp;
        {
            wchar_t buf[MAX_PATH + 1] = {};
            DWORD n = GetTempPathW(MAX_PATH, buf);
            tmp.assign(buf, n);
        }
        while (!tmp.empty() && (tmp.back() == L'\\' || tmp.back() == L'/'))
            tmp.pop_back();
        wchar_t suffix[80] = {};
        swprintf(suffix, 80, L"\\SysRecoverEA-%lu-%llu",
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long long)GetTickCount64());
        tmp += suffix;
        if (CreateDirectoryW(tmp.c_str(), nullptr)) {
            // \* = 子镜像根全部条目（eapack.dat / reppack.dat / 补写器 / 策略
            // 片段；后两者就地路径用不到，解出来无妨）。
            int xrc = wim.ExtractPaths(imagePath, eaIndex, {L"\\*"}, tmp);
            if (xrc != 0) {
                LogWarn("EA fix pack (ZJEA) extract failed rc=" +
                        std::to_string(xrc) + " (in-place restore continues)");
            } else {
                std::wstring root = letter + L":";
                auto log = [](const std::string& s) {
                    LogInfo("ea-apply: " + s);
                };
                std::wstring repPack = tmp + L"\\reppack.dat";
                if (GetFileAttributesW(repPack.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    unsigned long long rok = 0, rbad = 0;
                    int rrc = ea::ApplyRepack(repPack, root, &rok, &rbad, log);
                    LogInfo("reppack (in-place): rc=" + std::to_string(rrc) +
                            " ok=" + std::to_string(rok) +
                            " failed=" + std::to_string(rbad));
                }
                std::wstring eaPack = tmp + L"\\eapack.dat";
                if (GetFileAttributesW(eaPack.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    ea::ApplyStats st;
                    int prc = ea::ApplyPack(eaPack, root, st, log);
                    LogInfo("eapack (in-place): rc=" + std::to_string(prc) +
                            " ok=" + std::to_string(st.ok) +
                            " missing=" + std::to_string(st.missing) +
                            " failed=" + std::to_string(st.failed));
                }
            }
            DeleteTreeRec(tmp);
        } else {
            LogWarn("EA fix temp dir create failed; skipped (in-place restore "
                    "continues)");
        }
    }
    // 3) 只有系统镜像才修引导（看目标里有没有 winload.exe）
    if (req.repairBoot) {
        std::wstring winDir = letter + L":\\Windows";
        if (GetFileAttributesW((winDir + L"\\System32\\winload.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            std::string out;
            if (UseUefiBootFor(target)) {
                // 找 ESP（含回退）+ 先预检目标模板（docs/15 · P2/P4）
                std::wstring espRoot;
                bool guidMatched = false;
                std::string blog;
                std::string detail;
                if (!AcquireEspRoot(espRoot, guidMatched, blog,
                                     (int)target.diskIndex)) {
                    LogError("ESP acquire log (in-place):\n" + blog);
                    err = Tr("就地还原已完成，但找不到可用的 ESP 引导分区，无法修复引导：\n") +
                          W2U(DescribePartitions());
                    LogError(err);
                    ProgressDone("restore", "failed");
                    return 1;
                }
                if (!guidMatched)
                    LogWarn("ESP partition type is not EFI System; used fallback");
                // 0.6.35（用户 2026-10-05 规格）：**ESP BCD 已经有效就跳过 bcdboot**
                // —— bcdboot 会把 BCD 重建成只剩 Windows 条目，第三方启动项（易数
                // 一键还原等）被抹掉（客户实测投诉）。同机还原时原 BCD 本就指向同
                // 一块 C:；只有缺失/损坏时才重建（重建前导出备份）。
                // 2026-10-27（用户反馈 2）：bcdboot **缺失或失败**时用随包资产自足
                // （极简 PE 里根本没有 bcdboot；以前这里直接失败、只能拿 fixboot.exe 修）。
                std::string vd;
                if (VerifyEspBcd(espRoot, winDir, vd)) {
                    LogInfo("ESP BCD already valid -> skip bcdboot (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo("ESP BCD invalid -> rebuild: " + vd);
                    bool rebuilt = false;
                    if (ToolAvailable(L"bcdboot.exe") &&
                        TargetBcdTemplateOk(letter[0], detail) &&
                        EspHasRoomForBcdboot(espRoot, detail)) {
                        std::wstring ldir = LogBaseDir() + L"\\logs";
                        CreateDirectoryW(ldir.c_str(), nullptr);
                        BcdExport(ldir + L"\\bcd-backup-before-bcdboot");
                        int brc = RunProcess(
                            SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + std::wstring(1, espRoot[0]) +
                                L": /f UEFI",
                            out);
                        LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) +
                                " / " + out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot unavailable/not applicable -> "
                                "self-contained ESP boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureUefiBootFiles(
                            espRoot, std::wstring(1, letter[0]) + L":\\", winDir,
                            AppDir(), /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("ESP self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        std::string wlog;
                        if (EnsureWindowsBootEntry(espRoot, wlog))
                            LogInfo("firmware Windows boot entry: " + wlog);
                        else
                            LogWarn("firmware Windows boot entry (non-fatal): " +
                                    wlog);
                        if (!sok) {
                            err = Tr("就地还原已完成，但写入 ESP 引导失败：") + serr +
                                  Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                            UnmountEsp(espRoot, blog);
                            LogError(err);
                            ProgressDone("restore", "failed");
                            return 1;
                        }
                    }
                    if (!VerifyEspBcd(espRoot, winDir, detail)) {
                        err = Tr("就地还原已完成，但 ESP 引导校验未通过：") + detail +
                              Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                        UnmountEsp(espRoot, blog);
                        LogError(err);
                        ProgressDone("restore", "failed");
                        return 1;
                    }
                }
                UnmountEsp(espRoot, blog);
            } else {
                // BIOS：目标自身已有可引导 BCD（bootmgr + \Boot\BCD + winload 条目）
                // → 跳过重建，保住第三方启动项（同 UEFI 规则，0.6.35）。
                // 2026-10-27（用户反馈 2）：没有 bcdboot 时用随包模板自足。
                std::string vd;
                if (BiosBcdLooksValid(letter[0], vd)) {
                    LogInfo("BIOS BCD already valid -> skip rebuild (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo("BIOS BCD invalid -> rebuild: " + vd);
                    bool rebuilt = false;
                    if (ToolAvailable(L"bcdboot.exe")) {
                        std::wstring ldir = LogBaseDir() + L"\\logs";
                        CreateDirectoryW(ldir.c_str(), nullptr);
                        BcdExport(ldir + L"\\bcd-backup-before-bcdboot");
                        int brc = RunProcess(
                            SysToolPath(L"bcdboot.exe"),
                            winDir + L" /s " + letter + L": /f BIOS", out);
                        LogInfo("bcdboot (BIOS) rc=" + std::to_string(brc) +
                                " / " + out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot.exe missing -> self-contained BIOS boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureBiosBootFiles(
                            std::wstring(1, letter[0]) + L":\\", winDir,
                            AppDir(), /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("BIOS self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        if (!sok) {
                            err = Tr("就地还原已完成，但写入引导失败：") + serr +
                                  Tr("\n可进入 PE 用 `SysRecover.exe repair-boot` 重试修复引导。");
                            LogError(err);
                            ProgressDone("restore", "failed");
                            return 1;
                        }
                    }
                    if (!BiosBcdLooksValid(letter[0], vd))
                        LogWarn("BIOS boot verify note: " + vd);
                }
                // 分区引导区/磁盘 MBR 代码：format.com 之后不保证是微软引导代码，
                // 而 BIOS 引导缺了它就是黑屏光标闪（PIT-053）。**非致命**：安全软件
                // 可能拦写（火绒「写磁盘保留扇区」；bootself 内部已自动走物理盘兜底），
                // 写不进去只告警，不把还原判失败。
                std::string prlog;
                if (EnsureNtfsBootRegion(std::wstring(1, letter[0]) + L":\\",
                                         AppDir(), prlog))
                    LogInfo("pbr: " + prlog);
                else
                    LogWarn("pbr not fully written (non-fatal): " + prlog);
                if (DiskStyleOf(target.diskIndex) == PartitionStyle::MBR) {
                    std::string mrlog;
                    if (EnsureDiskMbrCode(target.diskIndex, AppDir(), mrlog))
                        LogInfo("mbr: " + mrlog);
                    else
                        LogWarn("mbr boot code (non-fatal): " + mrlog);
                }
            }
        } else {
            LogInfo("in-place: no winload.exe -> not a system image, skip "
                    "bcdboot");
        }
    }
    ProgressDone("restore", "done");
    LogInfo("in-place restore done (no reboot needed)");
    return 0;
}

// 往离线 ~new 的 SOFTWARE hive 里写一条 RunOnce（新系统**开机自动跑**）——
//   keepOld=false：跑 zj-swap-cleanup.cmd 静默删 ~old（无人值守默认）；
//   keepOld=true ：首启自动运行本程序 → 搬日志 + 问「是否删除旧系统」
//                  （用户 2026-10-17 规格："全自动化完成日志搬迁后再弹删除询问"）。
// 挂离线 hive 需要 SeBackup/SeRestore 特权（管理员默认持有但要显式启用）。
// 失败只 WARN 不阻断：程序已在桌面放快捷方式，用户可手动点。
//   hiveName ：临时挂载名（两个分支互斥，不会同进程冲突）；
//   valueName：RunOnce 值名（两个分支不同，深夜不会互相覆盖）；
//   cmd      ：要跑的启动命令行（REG_EXPAND_SZ 时支持 %SystemDrive% 展开）。
bool InjectOfflineRunOnce(const std::wstring& newDir, const wchar_t* hiveName,
                          const wchar_t* valueName, const wchar_t* cmd,
                          DWORD type, const char* what) {
    auto enablePriv = [](const wchar_t* name) {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(),
                              TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
            return false;
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        bool ok = GetLastError() == ERROR_SUCCESS;
        CloseHandle(tok);
        return ok;
    };
    enablePriv(L"SeBackupPrivilege");
    enablePriv(L"SeRestorePrivilege");
    const std::wstring hive = newDir + L"\\Windows\\System32\\config\\SOFTWARE";
    LONG rc = RegLoadKeyW(HKEY_LOCAL_MACHINE, hiveName, hive.c_str());
    if (rc != ERROR_SUCCESS) {
        LogWarn(std::string("A-engine: ") + what +
                " SOFTWARE hive load failed rc=" + std::to_string(rc));
        return false;
    }
    HKEY hKey = nullptr;
    const std::wstring sub = std::wstring(hiveName) +
                             L"\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, nullptr, 0,
                         KEY_WRITE, nullptr, &hKey, nullptr);
    if (rc == ERROR_SUCCESS) {
        rc = RegSetValueExW(hKey, valueName, 0, type, (const BYTE*)cmd,
                            (DWORD)((wcslen(cmd) + 1) * 2));
        RegCloseKey(hKey);
    }
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, hiveName);
    if (rc == ERROR_SUCCESS)
        LogInfo(std::string("A-engine: ") + what +
                " RunOnce injected into ~new");
    else
        LogWarn(std::string("A-engine: ") + what +
                " RunOnce inject failed rc=" + std::to_string(rc));
    return rc == ERROR_SUCCESS;
}

// 把首启清理器写进新系统，并挂 RunOnce：
//   - 只用于**日常还原的临时菜单**（menuEntry=false）；
//   - A 引擎把 helper + 公共启动文件夹 cmd 写进 ~new（不写 RunOnce）；
//   - B 引擎由救援层写同样两份文件（target 已没有离线 ~new）；
//   - cmd 先启动非提权 helper → 弹提示 → helper 自己再请求 UAC。
bool InstallFirstBootCleanup(const std::wstring& newDir,
                             const std::wstring& exeDir, std::string& log) {
    const std::wstring src = exeDir + L"\\bootfiles\\zj-restore-boot.exe";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log += "first-boot cleanup helper missing: zj-restore-boot.exe\n";
        return false;
    }
    const std::wstring dir = newDir + L"\\ProgramData\\SysRecover";
    const std::wstring startup =
        newDir + L"\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    CreateDirectoryW((newDir + L"\\ProgramData").c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(startup.c_str(), nullptr);
    const std::wstring dst = dir + L"\\zj-restore-boot.exe";
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        log += "copy zj-restore-boot.exe failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 启动文件夹：首次登录由资源管理器以普通权限启动 helper；helper 先弹说明，
    // 再自行 ShellExecute(runas) 请求 UAC。非提权实例会等待提权实例完成；
    // 成功后 cmd 立刻自删；UAC 取消/失败则 cmd 保留，下次登录重试。
    const char kCmd[] =
        "@echo off\r\n"
        "if exist \"%ProgramData%\\SysRecover\\boot-cleanup.done\" goto done\r\n"
        "\"%ProgramData%\\SysRecover\\zj-restore-boot.exe\"\r\n"
        "if exist \"%ProgramData%\\SysRecover\\boot-cleanup.done\" del \"%~f0\"\r\n"
        "goto :eof\r\n"
        ":done\r\n"
        "del \"%~f0\"\r\n";
    const std::wstring cmdPath = startup + L"\\SysRecover-RestoreBoot.cmd";
    HANDLE h = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "write startup cmd failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    DWORD wr = 0;
    WriteFile(h, kCmd, (DWORD)(sizeof(kCmd) - 1), &wr, nullptr);
    CloseHandle(h);
    log += "first-boot cleanup deployed (helper + startup cmd)\n";
    return true;
}

}  // namespace

// 目标分区该走 UEFI/ESP 分支吗？**以目标磁盘分区风格为准**（boot/bootpath.h）：
//   MBR 目标 → BIOS/GRUB4DOS 链（哪怕固件是 UEFI：那台机器本来就是 legacy 启动的）；
//   GPT 目标 → UEFI/ESP 链。
// 2026-09-29 修：此前只看 `IsUefiFirmware()`，导致 "UEFI 固件 + MBR 系统盘" 的机器
// 误走 ESP 分支 → "未找到 ESP 分区" 直接失败（用户实测，旧版 0.4.1 起一直存在）。
// 对外声明在 ops.h（GUI 要用它给"安装菜单成功"的提示分链措辞，PIT-094）——
// 定义必须在匿名命名空间**之外**，否则 ops.h 的声明没有外部定义、链接不到。
bool UseUefiBootFor(const PartitionInfo& t) {
    return ShouldUseUefiBoot(IsUefiFirmware(), DiskStyleOf(t.diskIndex));
}

// 启动项/固件变量写入失败时的统一提示（用户 2026-10-27 需求）：
// 装了金山毒霸/360 一类底层拦截的机器上，bcdedit / BootNext 都会"假成功"——
// 「被安全软件拦截」的结构化信号（2026-10-28，用户规格）：
// 火绒/360 会拦本进程写引导区/启动项（实测：bootsect 这类微软签名工具放行，
// 我们自己的进程一律 ERROR_ACCESS_DENIED）。GUI 据此弹「请退出防护 → 重试」。
// 用线程局部标志**不靠解析文案**（文案会翻译/改动）；工作线程内 Reset → 取走。
namespace {
thread_local bool t_bootBlocked = false;
void MarkBootBlocked() { t_bootBlocked = true; }
}  // namespace

void ResetBootBlocked() { t_bootBlocked = false; }
bool TakeBootBlocked() {
    const bool b = t_bootBlocked;
    t_bootBlocked = false;
    return b;
}

// 「安装到启动」前的能力扫描（2026-10-28 用户规格）。
// 与 StageRestoreImpl 的 block 预检同源（W1/W2），但**只读元数据、不动盘**，
// 让 GUI 在安装前就能告诉用户"要不要解压"。
bool CanRestoreInLinuxOnly(const std::wstring& image, int index,
                           size_t* badReparse, size_t* encrypted,
                           std::string& err) {
    if (badReparse) *badReparse = 0;
    if (encrypted) *encrypted = 0;
    // 加密镜像（ZJENC1）：救援层没有解密器 → **不能用 B 引擎**（恒为"需要解压"）。
    // GUI 会在调用本函数之前就识别出来；这里再兜一层，避免新调用方漏判。
    if (ImageIsEncrypted(image)) {
        err = Tr("这是加密镜像：救援层无法直接还原，只能「解压并安装」或 PE 就地还原。");
        LogInfo("menu scan: encrypted image -> linux-only restore disabled");
        return false;
    }
    WimEngine eng;
    std::vector<std::wstring> bad;
    size_t enc = 0;
    const int eff = index < 1 ? 1 : index;
    const int rc = eng.ScanImageForLinuxRestore(image, eff, bad, &enc);
    if (badReparse) *badReparse = bad.size();
    if (encrypted) *encrypted = enc;
    if (rc != 0) {
        err = Tr("读取镜像元数据失败（rc=") + std::to_string(rc) + Tr("）");
        LogWarn("menu scan: ScanImageForLinuxRestore rc=" + std::to_string(rc) +
                " image=" + W2U(image));
        return false;
    }
    LogInfo("menu scan: linux-only check image=" + W2U(image) + " index=" +
            std::to_string(eff) + " reparse=" + std::to_string(bad.size()) +
            " efs=" + std::to_string(enc));
    return bad.empty() && enc == 0;
}

// 命令返回 0，实际没落盘，重启后照旧进原系统，用户完全没有提示。凡写后
// 读回校验失败（或命令本身失败）都要带上这句，指向"退出防护重试/PE 就地还原"。
static std::string BootWriteBlockedHint() {
    MarkBootBlocked();
    return Tr(" 可能被安全软件（如火绒/360）拦截，写入未生效："
              "请退出其防护后重试，或用 PE 就地还原。");
}

// 目标分区是否**未**被占用（可以就地写）？—— 对外可见（ops.h），因为 GUI/CLI 要用它
// 决定提示文案（"需不需要重启"）；StageRestore 内部用的是同一个函数，不会漂移。
// 判据（用户规格）：不是正在运行的系统盘，且能对该卷加独占锁 ——
// FSCTL_LOCK_VOLUME 会因"卷上有打开的文件/句柄"而失败。
// 能就地写 → 直接还原（PE 里 / 还原到非系统盘）；否则走暂存 + 重启 + Linux。
bool CanRestoreInPlace(const PartitionInfo& t, std::string& why,
                       InPlaceReason* reason) {
    // reason：给调用方一个**稳定码**来选显示文案（GUI 确认框原来靠
    // `why.find("系统盘")` 匹配中文子串，消息一翻译就失效 —— PLAN §14 M1）
    auto setr = [&](InPlaceReason r) { if (reason) *reason = r; };
    if (t.letter.empty()) {
        setr(INPLACE_NO_LETTER);
        why = Tr("目标分区没有盘符（无法就地写）");
        return false;
    }
    wchar_t winDir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(winDir, MAX_PATH) &&
        SameDrive(winDir[0], t.letter[0])) {
        setr(INPLACE_SYSTEM_DISK);
        why = Tr("目标是正在运行的系统盘（必须重启后脱机还原）");
        return false;
    }
    // 用户规格（2026-09-23）：**PE 里只要目标不是正在运行的系统盘，就应该就地还原**，
    // 不重启也不提示重启。PE 下目标分区通常并没被真正使用，但 FSCTL_LOCK_VOLUME 仍
    // 可能因各种句柄失败 → 会误判成"必须重启"（用户实测踩到）。所以 PE 下直接放行；
    // 正常 Windows 仍用卷锁判定，只有真的锁不上（分区被占用、无法就地还原）才提示重启。
    if (IsWinPE()) {
        why = Tr("在 PE 中运行（目标非运行系统盘）→ 就地还原");
        // PE 下 FSCTL_LOCK_VOLUME 常因各种句柄失败 → 不能据此判"必须重启"（会把 PE
        // 误判成"要重启"，用户实测踩到）。所以**放行**，但仍**试一下锁**：锁不上只记
        // warn 继续（而不是拒绝）——这样格式化/wimlib 真失败时日志里有线索（问题清单 M-04）。
        std::wstring peDev = L"\\\\.\\" + t.letter + L":";
        HANDLE peH = CreateFileW(peDev.c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (peH == INVALID_HANDLE_VALUE) {
            LogWarn("PE: 打开目标卷失败 err=" + std::to_string(GetLastError()) +
                    "，仍尝试就地还原");
        } else {
            DWORD peRet = 0;
            if (DeviceIoControl(peH, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr)) {
                DeviceIoControl(peH, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0,
                                &peRet, nullptr);
            } else {
                LogWarn("PE: 目标卷加锁失败（可能有句柄占用）err=" +
                        std::to_string(GetLastError()) +
                        "，仍尝试就地还原；若格式化/写入失败请先排除占用");
            }
            CloseHandle(peH);
        }
        setr(INPLACE_PE);
        return true;
    }
    std::wstring dev = L"\\\\.\\" + t.letter + L":";
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        setr(INPLACE_OPEN_FAIL);
        why = Tr("打开目标卷失败 err=") + std::to_string(GetLastError());
        return false;
    }
    DWORD ret = 0;
    BOOL locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                  &ret, nullptr);
    if (locked)
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr);
    CloseHandle(h);
    setr(locked ? INPLACE_OK : INPLACE_LOCKED);
    why = locked ? Tr("目标分区未被占用") : Tr("目标分区正被使用（有打开的文件或句柄）");
    return locked != FALSE;
}
// 还原前空间预检（P1）—— 见 ops.h。**必须在格式化之前**调用。
int CheckRestoreSpace(const std::wstring& imagePath, int index,
                      const PartitionInfo& target, std::string& err) {
    unsigned long long bytes = 0;
    // 加密镜像走容器 meta（不解密正文）；普通镜像走 wimlib。两者同一口径。
    int rc = ImageContentSize(imagePath, index < 1 ? 1 : index, &bytes);
    if (rc != 0 || bytes == 0) {
        LogError("space precheck: cannot read image size (rc=" +
                 std::to_string(rc) + ") -> skip");
        return 0;  // 读不到大小 → 不阻断
    }
    // 内容量（已扣掉硬链接重复部分）+ 10% + 300MB（NTFS 元数据/目录索引/$LogFile 等开销）
    unsigned long long need = bytes + bytes / 10 + (300ull << 20);
    unsigned long long have = target.sizeBytes;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "space precheck: content %.1f GB, need ~%.1f GB, target %.1f GB",
             bytes / 1073741824.0, need / 1073741824.0, have / 1073741824.0);
    LogInfo(buf);
    if (have && need > have) {
        snprintf(buf, sizeof(buf),
                 Tr("目标分区空间不足：镜像解压后约需 %.1f GB（已含余量），" "但目标分区只有 %.1f GB。\n请换更大的目标分区，或改用更小的镜像。"),
                 need / 1073741824.0, have / 1073741824.0);
        err = buf;
        return 1;
    }
    return 0;
}

// ── Q1「先落盘后搬家」（用户 2026-10-18）────────────────────────────────────
// 还原确认后、写契约/格式化之前调用：程序家目录所在物理盘 == 目标盘时，
// 静默整包复制到安全分区 + 日志跟过去（不 re-exec：当前进程照旧跑完本次操作，
// 副本供还原后的下次使用）。触发范围仅还原/返回旧系统；备份不搬。
// 静态变量：同一进程内只判一次（RetryStagedRestore 再次调用时直接返回上次结果）。
namespace {
SettleResult g_settleResult;
}

SettleResult SettleHome(int targetDiskIndex, int exeDiskIndex) {
    if (g_settleResult.checked)
        return g_settleResult;

    g_settleResult.checked = true;

    // 程序目录不在固定盘（PE 的 X:\ RAM 盘 / 光盘）→ 无需搬迁
    std::wstring exeDir = ExeDir();
    if (exeDir.empty() || exeDir.size() < 2 || exeDir[1] != L':') {
        g_settleResult.detail = "exe dir not on a fixed drive -> no settle";
        return g_settleResult;
    }
    wchar_t exeLetter = (wchar_t)towupper(exeDir[0]);

    // 取不到程序目录盘号 → 不搬（fail-open，不阻断还原）
    if (exeDiskIndex < 0) {
        g_settleResult.detail = "cannot resolve exe disk index -> no settle";
        return g_settleResult;
    }

    // 程序目录不在目标盘 → 无需搬迁
    if (targetDiskIndex < 0 || exeDiskIndex != targetDiskIndex) {
        g_settleResult.detail = "exe dir not on target disk -> no settle";
        return g_settleResult;
    }

    // 程序目录在目标盘上 → 找安全分区（排除目标盘 + 系统盘优先非系统）
    DWORD mask = GetLogicalDrives();
    std::vector<wchar_t> candidates;
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wchar_t sysLetter = winDir[0];
    // 构建候选列表：所有可写固定盘，排除目标盘符
    std::vector<std::wstring> probeDirs;  // 记录探针建的目录，选完后清理未选中的
    for (wchar_t c = L'A'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        if (c == exeLetter)  // 目标盘（= 程序当前所在盘）
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        // 快速可写探针
        std::wstring probeDir = std::wstring(root) + L"ZJRESTORE";
        CreateDirectoryW(probeDir.c_str(), nullptr);
        probeDirs.push_back(probeDir);
        wchar_t probeFile[260];
        swprintf(probeFile, 260, L"%s\\.zj-probe.tmp", probeDir.c_str());
        HANDLE ph = CreateFileW(probeFile, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY |
                                                    FILE_FLAG_DELETE_ON_CLOSE,
                                nullptr);
        if (ph != INVALID_HANDLE_VALUE) {
            CloseHandle(ph);
            candidates.push_back(c);
        }
    }

    wchar_t chosen = SelectRelocateDrive(candidates, sysLetter, exeLetter);

    // 清理未选中候选盘上的空探针目录（避免在无关盘上留 ZJRESTORE 残迹）
    for (const auto& pd : probeDirs) {
        wchar_t pdLetter = pd[0];
        if (chosen != 0 && pdLetter == chosen)
            continue;  // 选中的保留（后续还要用它放整包）
        RemoveDirectoryW(pd.c_str());  // 只删空目录；有内容时静默失败
    }
    if (chosen == 0) {
        g_settleResult.noSafeDrive = true;
        g_settleResult.detail = "on target disk but no safe drive found";
        LogWarn("settle: on target disk but no safe drive found (exe dir will "
                "be lost with target)");
        return g_settleResult;
    }

    // 复制整包 + 日志 到 <chosen>:\ZJRESTORE\（注意行尾不能有 \，会吞下一行）
    std::wstring destRoot = std::wstring(1, chosen) + L":\\" + kRecoveryDir;
    CreateDirectoryW(destRoot.c_str(), nullptr);

    // 空间检查：整包大小 + 300MB 余量
    unsigned long long needBytes = AppTreeSize() + (300ull << 20);
    ULARGE_INTEGER avail = {};
    std::wstring availPath = std::wstring(1, chosen) + L":\\";
    if (GetDiskFreeSpaceExW(availPath.c_str(), &avail, nullptr, nullptr) &&
        avail.QuadPart < needBytes) {
        g_settleResult.noSafeDrive = true;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "settle: not enough space on %c: need=%.1fGB avail=%.1fGB",
                 (char)chosen, needBytes / 1073741824.0,
                 avail.QuadPart / 1073741824.0);
        g_settleResult.detail = buf;
        LogWarn(buf);
        return g_settleResult;
    }

    LogInfo("settle: copying app tree + logs to " + W2U(destRoot));
    bool copyOk = CopyDir(exeDir, destRoot);
    if (!copyOk) {
        g_settleResult.noSafeDrive = true;
        g_settleResult.detail = "copy to " + W2U(destRoot) + " failed";
        LogWarn("settle: CopyDir to " + W2U(destRoot) + " failed");
        return g_settleResult;
    }

    // 日志也跟过去（当前进程继续写原位置，但 LogBaseDir 从此返回新家）
    std::wstring srcLogs = LogBaseDir() + L"\\logs";
    std::wstring dstLogs = destRoot + L"\\logs";
    CreateDirectoryW(dstLogs.c_str(), nullptr);
    CopyDir(srcLogs, dstLogs);

    // 设置日志根覆盖 + 返回结果
    SetLogHomeOverride(destRoot);
    g_settleResult.moved = true;
    g_settleResult.newHome = destRoot;
    g_settleResult.detail = "copied to " + W2U(destRoot);
    LogInfo("settle: done, new home = " + W2U(destRoot));
    return g_settleResult;
}

const SettleResult& LastSettleResult() { return g_settleResult; }

// 失败后的"下一步"（P8）已迁到 advice.cpp —— 见 advice.h。原因（PLAN §14 M1）：
// 本文件里旧版靠「中文关键词匹配中文消息」，消息一被翻译就全部失配 → 静默不给建议。
// 下面这行 = 把老签名**声明为已删除**：谁在本翻译单元里再按旧签名调用，编译期就报错。
std::string ErrorAdvice(int rc, const std::string& err) = delete;

// 历史记录（P7）：往 日志根\logs\history.jsonl 追加一行（JSON Lines，外部/AI 好读）。
// 日志根见 LogBaseDir（PIT-104/105：一般=程序目录；软件在系统盘时=数据盘 ZJRESTORE）。
// 记"什么时候做了什么"：备份完成、就地还原、暂存还原（暂存的实际结果在救援层日志里）。
void AppendHistory(const char* action, const std::wstring& image,
                   const std::wstring& target, unsigned long long ms,
                   const char* detail) {
    std::wstring dir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE h = CreateFileW((dir + L"\\history.jsonl").c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    // 完整 JSON 字符串转义：`\` `"` 以及所有 < 0x20 的控制字符（\n\t\r\b\f 用短写，
    // 其余用 \u00xx）。原来只转义 `\` 和 `"` —— detail 一旦带换行就会写出非法 JSON
    //（问题清单 L-03）。
    auto esc = [](const std::string& s) {
        static const char* hx = "0123456789abcdef";
        std::string o;
        o.reserve(s.size() + 8);
        for (unsigned char c : s) {
            switch (c) {
                case '\\': o += "\\\\"; break;
                case '"': o += "\\\""; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                case '\b': o += "\\b"; break;
                case '\f': o += "\\f"; break;
                default:
                    if (c < 0x20) {
                        o += "\\u00";
                        o += hx[(c >> 4) & 0xF];
                        o += hx[c & 0xF];
                    } else {
                        o += static_cast<char>(c);
                    }
            }
        }
        return o;
    };
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[1200];
    snprintf(buf, sizeof(buf),
             "{\"time\":\"%04u-%02u-%02u %02u:%02u:%02u\",\"action\":\"%s\","
             "\"image\":\"%s\",\"target\":\"%s\",\"elapsed_s\":%llu,"
             "\"version\":\"%s\",\"detail\":\"%s\"}\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             action, esc(W2U(image)).c_str(), esc(W2U(target)).c_str(),
             ms / 1000, SYSRECOVER_VERSION, detail ? detail : "");
    DWORD wrote = 0;
    WriteFile(h, buf, static_cast<DWORD>(strlen(buf)), &wrote, nullptr);
    CloseHandle(h);
}

// ────────────────── ESP 分区备份（方案 C：并入主镜像，用户 2026-09-30 规格） ──────────────────

// ESP 子镜像的 <DESCRIPTION> 注释（"镜像里直接加注释"的落点；镜像内元数据，
// 不做 Tr 界面翻译）。还原侧防呆按子镜像 **name 含 "ESP"** 判断（见 StageRestore）。
static const wchar_t kEspImageDesc[] =
    L"SysRecover ESP partition backup; restored to the ESP automatically "
    L"when restoring the system (never restore this image alone)";

// EA 修复子镜像的 <DESCRIPTION>（PIT-122）：内容 = eapack.dat（带 EA 的文件
// 路径+名字+值）+ zj-ea-apply.exe（首启补写器）+ zj-regpol.bin（同步策略片段）。
// 救援层按契约 ea_index 解包投放，目标系统首启（登录前 SYSTEM）自动写回 EA。
static const wchar_t kEaImageDesc[] =
    L"SysRecover extended-attributes (EA) fix data; deployed to the target by "
    L"the rescue layer and applied on first boot (never restore this image alone)";

// ESP 捕获的排除配置（wimscript 格式，临时文件，同 EnsureExclusionConfig 做法）：
//   · \EFI\ZJRESTORE —— 我们的常驻救援载荷（约 33MB，还原后 Windows 侧会重新
//     部署，进镜像纯属浪费，且它不是"原厂 ESP 内容"）；
//   · \System Volume Information —— 系统卷信息，无备份价值（读得到也排除）；
//   · \EFI\Microsoft\Boot\BCD(+.LOG*) —— **实测 rc=47 的真凶**：运行中的 Windows
//     把 ESP 的 BCD 当注册表 hive 挂着（HKLM\BCD00000000）→ 独占锁，wimlib 一读
//     就 Failed to open a file（PIT-051 同源：那里 CopyFile 也必失败）。BCD 还原
//     后由 `bcdboot` 重建（PIT-060/090 已在流程里），且 Linux 恢复用目录模式
//     apply（只新增/覆盖、不删）→ ESP 上现役 BCD 不会被并入的子镜像动到，正合适。
// ⚠️ 排除目录写法**无尾斜杠**（与 exclude.cpp 系统盘清单一致）：wimlib/DISM 语义
//    是"匹配目录条目本身即跳过整棵树"，带尾反斜杠的 glob 反而不匹配（实测）。
static std::wstring EnsureEspExclusionConfig() {
    static const char kContent[] =
        "[ExclusionList]\n"
        "\\EFI\\ZJRESTORE\n"
        "\\System Volume Information\n"
        "\\EFI\\Microsoft\\Boot\\BCD\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG1\n"
        "\\EFI\\Microsoft\\Boot\\BCD.LOG2\n";
    wchar_t tmpDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmpDir) == 0)
        return L"";
    std::wstring path =
        std::wstring(tmpDir) + L"zjrestore-esp-exclusion.ini";
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    BOOL ok = WriteFile(h, kContent, sizeof(kContent) - 1, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != sizeof(kContent) - 1)
        return L"";
    return path;
}

// 备份目标可写预检（2026-10-21）：wimlib 只回 rc=47 "Failed to open a file"，
// 分不清“权限/网络路径/无效名/路径过长”。这里用 CreateFileW 在目标目录建一个
// 探针文件（与资源管理器同一 API），失败时把 Win32 错误码翻译成可操作的提示。
static std::wstring Win32ErrorText(DWORD e) {
    wchar_t* p = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, e, 0, (LPWSTR)&p, 0, nullptr);
    std::wstring s = (n && p) ? std::wstring(p, n) : std::wstring();
    if (p) LocalFree(p);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' ||
                          s.back() == L' '))
        s.pop_back();
    return s;
}

static bool ProbeBackupDest(const std::wstring& dest, std::string& err) {
    const std::wstring dir = ParentDir(dest);
    if (dir.empty()) return true;
    wchar_t name[64] = {};
    swprintf(name, 64, L"\\zjprobe-%lu.tmp",
             (unsigned long)GetCurrentProcessId());
    const std::wstring probe = dir + name;
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        std::wstring msg = Tr(L"备份目标不可写（错误码 ") +
                           std::to_wstring((unsigned long)e) + Tr(L"）：") +
                           Win32ErrorText(e);
        if (e == 5)
            msg += Tr(L"\n提示：本程序以管理员运行；若共享是按当前登录用户"
                      L"授权的，UAC 提权可能使用了另一个账号，请确认提权后"
                      L"的账号也能访问该共享。");
        else if (e == 53 || e == 67 || e == 64)
            msg += Tr(L"\n提示：请检查机器名/共享名是否正确、网络是否可达。");
        else if (e == 3 || e == 2)
            msg += Tr(L"\n提示：目标文件夹不存在。");
        else if (e == 123)
            msg += Tr(L"\n提示：路径格式不正确（应为 \\\\机器名\\共享名\\...）。");
        else if (e == 206)
            msg += Tr(L"\n提示：路径过长。");
        err = W2U(msg);
        LogError("backup preflight failed: " + err);
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

int RunBackup(const BackupRequest& reqIn, ProgressFn progress,
              std::string& err, ErrAdvice* adv,
              std::vector<std::wstring>* skipped) {
    if (adv) *adv = ADV_NONE;
    if (skipped) skipped->clear();
    std::vector<std::wstring> skippedList;
    if (reqIn.dest.empty()) {
        err = Tr("缺少目标镜像路径");
        return 1;
    }
    // ── 加密备份（ZJENC1，docs/30）──
    // 明文先写到 <目标>.plain.tmp（沿用现有 stage/原子写/Probe 全流程），
    // 收尾再加密成 <目标>.zje 并**删除明文**。任何失败都不留明文镜像。
    BackupRequest req = reqIn;
    const bool encrypt = !reqIn.password.empty();
    // 兜底：任何提前 return 都把明文中转删掉（加密备份的红线是"绝不留明文"）。
    struct PlainGuard {
        std::wstring p;
        bool on = false;
        ~PlainGuard() {
            if (on)
                DeleteFileW(p.c_str());
        }
    } plainGuard{reqIn.dest, false};
    if (encrypt) {
        if (reqIn.append) {
            err = Tr("加密备份不支持追加（增量请用未加密的 .wim/.esd）");
            return 2;
        }
        if (reqIn.dest.size() < 4 ||
            _wcsicmp(reqIn.dest.c_str() + reqIn.dest.size() - 4, L".zje") != 0) {
            err = Tr("加密备份的输出文件名必须以 .zje 结尾");
            return 2;
        }
        req.dest = reqIn.dest + L".plain.tmp";
        plainGuard.p = req.dest;
        plainGuard.on = true;
    }
    // 源路径归一（2026-09-28 用户 CLI 复现：`--source C:` 备份报 rc=47）：
    //   · Win32 里 `C:` 是“该盘的**当前目录**”，不是根，也不带 wimlib 要求的
    //     尾斜杠 → 盘符根判定落空 → 没走 VSS 热备，直接扫活动系统；
    //   · 扫活动系统会在某个正被占用的文件上失败，wimlib 只回
    //     `rc=47 Failed to open a file`，看不出是谁的锅。
    // 统一成 `C:/` 后，`C:` / `C:\` / `C:/` 三条写法等价（GUI 本来就传 `X:/`）。
    const std::wstring source = NormalizeVolumeRoot(req.source);
    const bool volumeRoot = IsVolumeRoot(source);
    // 目标目录不存在也要建出来：`D:\backup\x.esd` 里 D:\backup 没建时，
    // wimlib 建不出输出文件，同样只报 rc=47（2026-09-28 实测 3 秒即失败）。
    {
        std::wstring dir = ParentDir(req.dest);
        if (!dir.empty() && !MakeDirTree(dir)) {
            err = std::string(Tr("无法创建备份目录：")) + W2U(dir);
            LogError(err);
            return 1;
        }
    }
    if (!ProbeBackupDest(req.dest, err)) return 1;
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("backup", 0, "start");
    LogInfo("backup source=" + W2U(source) + " dest=" + W2U(req.dest) +
            " compress=" + req.compress +
            (volumeRoot ? " (volume root -> VSS hot backup)" : ""));
    const ULONGLONG t0 = GetTickCount64();  // P7：历史记录用
    // 盘符根（C:/ 或 C:\）→ 热备：VSS 快照 + 排除配置（PIT-008/009）；
    // 也可用 req.snapshot 显式强制（例如备份正在使用的数据库目录）；
    // req.noSnapshot 显式**禁止**（PE / 离线卷 / 挂载的 VHD 卷上 VSS 不可用）。
    bool snapshot = (req.snapshot || volumeRoot) && !req.noSnapshot;
    if (req.noSnapshot && volumeRoot)
        LogInfo("snapshot disabled by --no-snapshot (cold backup of volume root)");
    // CPU 硬上限（BackupRequest::cpuCap，GUI 的「限制CPU」下拉）：在动数据之前
    // 设好，所有成功/失败退出路径都由 guard 复位回“不限”。
    struct CpuCapGuard {
        int pct;
        explicit CpuCapGuard(int p) : pct(p) {
            if (pct <= 0) return;
            std::string why;
            if (!SetCpuCap(pct, &why))
                LogWarn("cpu cap not applied: " + why);
            else
                LogInfo("cpu cap = " + std::to_string(pct) + "%");
        }
        ~CpuCapGuard() {
            if (pct <= 0) return;
            std::string why;
            SetCpuCap(0, &why);
        }
    } cpuGuard(req.cpuCap);
    std::wstring errw;  // VSS 检查的多行提示（成功路径不会用到）
    // VSS 前置检查（用户 2026-09-26 规格）：服务停着就帮用户启动、起不来就给
    // 明确的处理指引（别等 wimlib 报 rc=89 让人干猜）；guard 析构把我们启动的
    // 服务停回原状（"完成后关闭"），覆盖下面所有成功/失败退出路径。
    vss::BackupGuard vss_guard;
    if (snapshot && !vss_guard.Ensure(errw)) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        if (adv) *adv = ADV_VSS;  // 消息自带处理步骤 → ErrorAdvice 不再追加
        err = W2U(errw);  // 多行中文处理指引（GUI MessageBox / CLI 原样打印）
        LogError(err);
        return 1;
    }
    // 热备前把注册表 hive 刷盘：运行中的 hive 常处于"脏"状态
    // （REGF base block 的 primary_seq != secondary_seq），刷一次可让 VSS 抓到
    // 干净的 hive；即便仍是脏的，随包捕获的 *.LOG1/.LOG2 也能在开机时恢复
    // （PIT-056：这两者缺一，还原后就会黑屏起不来）。
    if (snapshot) {
        RegFlushKey(HKEY_LOCAL_MACHINE);
        RegFlushKey(HKEY_USERS);
        LogInfo("registry hives flushed before snapshot");
    }
    WimEngine engine;
    if (!engine.ok()) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = Tr("wimlib 初始化失败");
        return 1;
    }
    // 重解析点检出（PIT-135，2026-10-08；1019 案修订，2026-10-19）：Linux 侧
    // wimlib（NTFS-3G 块模式）无法设置非微软 reparse（libntfs-3g 已知 bug，
    // wimlib NEWS 明说），个别微软 tag 变体（junction 落文件等）同为 set 链
    // EINVAL 必失败——实测 Intel ipfsrv.dptf 令整个 apply rc=58 中止、目标分区
    // 只剩半成品。判定 = IsUnsettableReparse（valid_reparse_data 移植）。
    // 备份时排除（并留日志），否则重启类还原必然失败。
    std::vector<std::wstring> badRep;
    int badN = ea::FindUnsettableReparse(source, badRep);
    if (badN > 0)
        LogWarn("reparse: " + std::to_string(badN) +
                " unsettable reparse entry(ies) excluded (Linux apply cannot "
                "set them, PIT-135); first: " + W2U(badRep[0]));
    std::wstring cfg =
        EnsureExclusionConfig(source, badN > 0 ? &badRep : nullptr);
    if (cfg.empty())
        LogError("排除配置生成失败，热备可能因易失文件报 rc=88（PIT-009）");
    else
        LogInfo("exclusion config: " + W2U(cfg));
    // ── ESP / EA 并入主镜像 ──
    // ESP（方案 C，用户 2026-09-30 规格 / 无忧 66 楼）：作为**同一个文件里的
    // 子镜像**（name="ESP"），还原时由契约 esp_index 定位、救援层恢复；
    // EA（PIT-122，2026-10-06）：子镜像 name="ZJEA"（见下）。
    // 原子性：非 append 时**一律**先写 `<dest>.stage`（ESP/EA 并入结束且 stage
    // 确认可用后才 rename 成正式文件）—— 并入失败绝不损坏已写好的主镜像；
    // 无子镜像时多一次 rename，代价可忽略（Windows 同卷 rename 是元数据操作）。
    const bool stage = !req.append;
    const std::wstring mainDest = stage ? req.dest + L".stage" : req.dest;
    if (stage) {
        DeleteFileW(mainDest.c_str());             // 上次异常残留的 stage
        DeleteFileW((mainDest + L".tmp").c_str());  // 其 Capture 半成品
    }
    int rc;
    if (req.append)
        rc = engine.Append(source, req.dest, req.compress, req.name,
                           snapshot, cfg, progress, L"", &skippedList);
    else
        rc = engine.Capture(source, mainDest, req.compress, req.name,
                            snapshot, cfg, progress, &skippedList);
    if (rc != 0) {
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        char buf[128];
        snprintf(buf, sizeof(buf), Tr("备份失败(rc=%d): "), rc);
        err = buf;
        err += W2U(WimEngine::ErrorString(rc));
        LogError(err);
        return 1;
    }

    // ── ESP 并入（主镜像写出之后、rename/verify 之前）──
    // 没勾 --esp，或本机没有 ESP（BIOS/MBR 等）→ 只记日志跳过（不算失败），
    // 绝大多数任务不带；stage 流程照样走（多一次无害的 rename）。
    int erc = 0;
    if (req.esp) {
        ProgressUpdate("backup", 100, "esp");
        // 阶段标记进 GUI 状态栏/CLI 控制台（ProgressUpdate 只写 progress.json，
        // 外部工具看得到、用户看不到 → 100% 后像卡死，PIT-098）。
        if (progress) progress(100, "esp");
        std::wstring espRoot;
        bool guidMatched = false;
        std::string blog;
        if (!AcquireEspRoot(espRoot, guidMatched, blog)) {
            LogWarn("esp merge skipped (no ESP found): " + blog);
        } else {
            if (!guidMatched)
                LogWarn("ESP partition type is not EFI System; used fallback "
                        "(esp merge)");
            WimEngine espEng;  // 新会话读 stage 里刚写完的主镜像（见下）
            std::wstring espSrc = espRoot;
            espSrc.back() = L'/';  // wimlib 源路径强制盘符根尾斜杠（§6）
            // snapshot=false：ESP 是 FAT 小分区，没有"易失文件"问题，
            // 也没必要再开一次 VSS 快照。
            erc = espEng.Append(espSrc, mainDest, req.compress, L"ESP", false,
                                EnsureEspExclusionConfig(), progress,
                                kEspImageDesc, &skippedList);
            UnmountEsp(espRoot, blog);
            LogInfo("esp append rc=" + std::to_string(erc) + " dest=" +
                    W2U(mainDest) + " (" + blog + ")");
        }
    }

    // ── EA 采集并入（PIT-122，2026-10-06）──
    // 背景：Linux 侧 wimlib apply 丢 Windows EA（apply.out: `Ignoring extended
    // attributes of N files`，PIT-120 客户实测 804 个文件）；Windows 侧不丢，
    // 但重启类还原必须走 Linux（§2 红线 1）。做法：把源里带 EA 的文件（路径+
    // 名字+值）打包成 eapack.dat，连同首启补写器/同步策略片段打成子镜像
    // "ZJEA"；救援层投放后由目标系统**首次开机**（登录前、SYSTEM）自动写回。
    // 非致命：采集/并入失败只警告 —— EA 修复是增益，主镜像照常交付。
    {
        std::wstring root = source;
        while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
            root.pop_back();
        if (root.empty())
            root = L"C:";
        // 审计文本沿用 logs\ea-scan-<时间>.txt（用户 2026-10-05 规格）
        std::wstring logsDir = LogBaseDir() + L"\\logs";
        CreateDirectoryW(logsDir.c_str(), nullptr);
        SYSTEMTIME stw = {};
        GetLocalTime(&stw);
        wchar_t stamp[40] = {};
        swprintf(stamp, 40, L"%04u%02u%02u-%02u%02u%02u", stw.wYear, stw.wMonth,
                 stw.wDay, stw.wHour, stw.wMinute, stw.wSecond);
        std::wstring auditPath = logsDir + L"\\ea-scan-" + stamp + L".txt";
        // 临时目录：eapack.dat + zj-regpol.bin + zj-ea-apply.exe
        std::wstring eaTmp = req.dest + L".ea.tmp";
        DeleteTreeRec(eaTmp);
        if (MakeDirTree(eaTmp)) {
            if (progress) progress(99, "ea-scan");
            ProgressUpdate("backup", 99, "ea-scan");
            std::string eaErr;
            int eaN = ea::CaptureVolume(root, eaTmp + L"\\eapack.dat",
                                        auditPath, eaErr);
            if (eaN < 0) {
                LogWarn("EA capture failed: " + eaErr);
            } else if (eaN == 0) {
                LogInfo("EA capture: no EA-bearing files in source");
            } else {
                LogInfo("EA capture: " + std::to_string(eaN) +
                        " EA-bearing file(s); audit: " + W2U(auditPath));
            }
            if (eaN <= 0)  // 无 EA（或失败）时不留空包：补写器将只做引用清理
                DeleteFileW((eaTmp + L"\\eapack.dat").c_str());
            // 非微软重解析点打包（PIT-136，用户 2026-10-08 规格）：Linux 侧
            // wimlib 写不了这类文件（libntfs-3g bug）→ 主镜像已排除（见上），
            // 这里把"内容+reparse 原始数据+属性"打包，首启补写器原生写回。
            std::string repErr;
            int repN =
                ea::CaptureReparse(root, eaTmp + L"\\reppack.dat", repErr);
            if (repN > 0)
                LogInfo("reparse pack: " + std::to_string(repN) +
                        " non-Microsoft reparse file(s) packed for first-boot "
                        "restore" +
                        (repErr.empty() ? "" : " (" + repErr + ")"));
            else if (repN < 0)
                LogWarn("reparse pack failed: " + repErr);
            // 悬空引用扫描（PIT-132）：注册表引用指向"会被备份排除的易失目录"
            // （Temp 等）且文件存在 → 还原后必然缺失（实测：豆包便携版右键扩展
            // DLL 在 Temp → 每次右键卡死）。>0 时并入修复包，首启自动清理。
            std::vector<refscan::DanglingRef> refs =
                refscan::ScanDanglingRefs(true);
            if (!refs.empty()) {
                int miss = 0;
                for (auto& r : refs)
                    if (r.missingNow) miss++;
                LogInfo("ref scan: " + std::to_string(refs.size()) +
                        " volatile-dir reference(s) (" + std::to_string(miss) +
                        " missing now, " +
                        std::to_string((int)refs.size() - miss) +
                        " at-risk) -> first-boot auto-clean");
                for (size_t i = 0; i < refs.size() && i < 10; ++i)
                    LogInfo("ref scan: " + W2U(refs[i].display));
            }
            if (eaN > 0 || !refs.empty() || repN > 0) {
                // 子镜像内容：pack（有 EA 时）+ reppack（有非微软 reparse 时）+
                // 策略片段 + 补写器（x86，x86/x64 都能跑）
                bool okFiles = ea::WriteBytes(eaTmp + L"\\zj-regpol.bin",
                                              ea::BuildSyncPolFragment());
                std::wstring applier = AppDir() + L"\\bootfiles\\zj-ea-apply.exe";
                if (okFiles)
                    okFiles = CopyFileW(applier.c_str(),
                                        (eaTmp + L"\\zj-ea-apply.exe").c_str(),
                                        FALSE) != 0;
                if (!okFiles) {
                    LogWarn("EA deploy files incomplete (zj-ea-apply.exe "
                            "missing?); EA fix skipped");
                } else {
                    WimEngine eaEng;
                    std::wstring eaSrc = eaTmp + L"\\";
                    int earc = eaEng.Append(eaSrc, mainDest, req.compress,
                                            L"ZJEA", false, L"", progress,
                                            kEaImageDesc, &skippedList);
                    if (earc != 0) {
                        LogWarn("EA pack append failed rc=" +
                                std::to_string(earc) +
                                " (main image continues without EA fix)");
                    } else {
                        LogInfo("EA/fix pack appended as subimage ZJEA (ea=" +
                                std::to_string(eaN) + " refs=" +
                                std::to_string(refs.size()) + ")");
                    }
                }
            }
        } else {
            LogWarn("EA temp dir create failed; EA fix skipped");
        }
        DeleteTreeRec(eaTmp);
    }

    // ESP 失败时统一的错误文本（tail 由调用方给）
    auto espFail = [&](const std::string& tail) {
        char buf[160];
        snprintf(buf, sizeof(buf), Tr("ESP 备份失败(rc=%d): "), erc);
        std::string e = buf;
        // -1 = 引擎未初始化（wimlib.h 不外泄 → ErrorString(-1) 不可用）
        e += erc == -1 ? std::string(Tr("wimlib 初始化失败"))
                       : W2U(WimEngine::ErrorString(erc));
        e += tail;
        return e;
    };

    // stage → 正式文件（主镜像原子落盘）
    if (stage) {
        if (erc != 0) {
            // ESP 并入半途失败可能把 stage 写坏（wimlib_overwrite 原地重写）→
            // 先 Probe：坏 → 删掉重报；好 → 照常交付（主镜像只是不含 ESP）。
            if (progress) progress(99, "probe");
            WimEngine probe;
            std::wstring why;
            if (probe.Probe(mainDest, why) != 0) {
                DeleteFileW(mainDest.c_str());
                ReleaseOpLock();
                ProgressDone("backup", "failed");
                err = espFail(Tr("（主镜像写入损坏，已删除，请重新备份）"));
                LogError(err);
                return 1;
            }
        }
        if (!MoveFileExW(mainDest.c_str(), req.dest.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD e = GetLastError();
            DeleteFileW(mainDest.c_str());
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("备份写出失败：无法提交镜像文件(err=") +
                  std::to_string(e) + ")";
            LogError(err);
            return 1;
        }
    }
    if (erc != 0) {
        // 走到这里主镜像已落盘（stage 已 rename / append 原地写完）→ 报 ESP
        // 失败但**不回滚**主镜像（A 方案同款语义：主镜像可用，只是不含 ESP）。
        if (!stage) {
            // append 模式是原地写：确认既有文件没被半途 overwrite 写坏
            if (progress) progress(99, "probe");
            WimEngine probe;
            std::wstring why;
            if (probe.Probe(req.dest, why) != 0) {
                ReleaseOpLock();
                ProgressDone("backup", "failed");
                err = espFail(Tr("（镜像写入损坏，请重新备份）"));
                LogError(err);
                return 1;
            }
        }
        ReleaseOpLock();
        ProgressDone("backup", "failed");
        err = espFail(Tr("（主镜像已生成，但未包含 ESP 备份）"));
        LogError(err);
        return 1;
    }

    // 收尾可用性检查（PIT-057 / PIT-106）：默认只做**秒级快检**（打开 +
    // "写入未完成"标记 + 子镜像数）；全量校验只在用户显式 --verify 时跑一次
    // （带真实进度）。不再默认全扫：大镜像扫一遍十几分钟，用户无法忍受。
    bool didVerify = false;
    if (req.verify) {
        ProgressUpdate("backup", 99, "verify");
        if (progress) progress(99, "verify");
        int vrc = engine.Verify(req.dest, progress);
        if (vrc != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "verify-failed");
            char buf[128];
            snprintf(buf, sizeof(buf), Tr("备份已写出但校验失败(rc=%d): "), vrc);
            err = buf;
            err += W2U(WimEngine::ErrorString(vrc));
            LogError(err);
            return 5;
        }
        didVerify = true;
        LogInfo("backup verify passed");
    }
    if (!didVerify) {
        if (progress) progress(99, "probe");
        WimEngine probe;
        std::wstring why;
        if (probe.Probe(req.dest, why) != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("备份镜像不可用：");
            err += W2U(why);
            LogError(err);
            return 1;
        }
    }
    // ── 加密收尾（docs/30）：明文 → .zje，删明文 ──
    if (encrypt) {
        if (progress) progress(99, "encrypt");
        ProgressUpdate("backup", 99, "encrypt");
        std::vector<ZjeImage> meta;
        {
            WimEngine pe;
            std::vector<ImageDesc> imgs;
            if (pe.ListImages(req.dest, imgs) == 0) {
                for (const auto& d : imgs) {
                    ZjeImage z;
                    z.index = d.index;
                    z.name = d.name;
                    z.description = d.description;
                    z.sizeBytes = d.sizeBytes;
                    z.creationTime = d.creationTime;
                    bool has = false;
                    if (pe.ImagePathExists(
                            req.dest, d.index,
                            L"\\Windows\\system32\\winload.exe", &has) == 0)
                        z.hasWinload = has;
                    meta.push_back(std::move(z));
                }
            }
        }
        CryptProgress cp = [&](unsigned long long done,
                               unsigned long long total) -> bool {
            if (!progress)
                return false;
            const int pct = total ? (int)(done * 100 / total) : -1;
            return progress(pct, "encrypt");
        };
        std::wstring werr;
        LogInfo("encrypting backup -> " + W2U(reqIn.dest));
        const int zrc =
            ZjeEncryptFile(req.dest, reqIn.dest, reqIn.password, meta, cp, werr);
        // 明文不论成败都不留（失败时更不能留：用户以为已加密）
        DeleteFileW(req.dest.c_str());
        if (zrc != 0) {
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("加密备份失败：") + W2U(werr);
            LogError(err);
            return 1;
        }
        // 容器可用性（密码 + 元数据 MAC）
        std::wstring neverr;
        if (ZjeReadMeta(reqIn.dest, reqIn.password, nullptr, nullptr, neverr) !=
            0) {
            DeleteFileW(reqIn.dest.c_str());
            ReleaseOpLock();
            ProgressDone("backup", "failed");
            err = Tr("加密镜像写出后不可用：") + W2U(neverr);
            LogError(err);
            return 1;
        }
        LogInfo("encrypted backup done: " + W2U(reqIn.dest));
    }
    // 源文件"异常属性"审计 + EA 修复并入：已在上面的 `ea-scan` 块完成
    // （PIT-122 起审计与 eapack/ZJEA 子镜像一体化，不再单独扫第二遍）。
    if (skipped) *skipped = skippedList;
    if (!skippedList.empty()) {
        LogWarn("backup: incomplete, skipped " +
                std::to_string(skippedList.size()) +
                " unreadable file(s); see skipped unreadable file lines");
    }
    ReleaseOpLock();
    ProgressDone("backup", "done");
    LogInfo("backup done");
    AppendHistory("backup", reqIn.dest, source, GetTickCount64() - t0,
                  skippedList.empty() ? "ok" : "ok-skipped");
    return 0;
}

// 还原的两条"安装"模式（共用同一套检查与契约，只有"是否立即重启"不同）：
//   menuEntry=false：StageRestore —— 判定就地/暂存，暂存则设**单次启动**（BootNext/
//                    bootsequence），重启后自动执行；
//   menuEntry=true ：StageRestoreMenu —— 只写常驻契约 + 常驻启动项，**不设单次启动**，
//                    留给用户在开机菜单里自行选择（用户 2026-09-29 规格）。
int StageRestoreImpl(const RestoreRequest& req, std::string& err,
                     bool* needReboot, ProgressFn progress, ErrAdvice* adv,
                     bool menuEntry) {
    if (adv) *adv = ADV_NONE;
    if (req.image.empty()) {
        err = Tr("缺少镜像路径");
        return 1;
    }
    // 0.05) 引擎取值与「引擎 × 备返」组合校验（docs/24 P1-1/P1-2，2026-10-11）：
    //   ① --engine 只认 swap/block/空（空=默认 swap）——防手滑值写进契约后
    //      在救援层被缺省成另一种引擎（静默换承诺）；
    //   ② 备返（keepOld，保留旧系统 ~old）由 A 引擎的改名交换实现；B 引擎
    //      （--engine block）会 mkntfs 格式化目标分区，旧系统必然消失 ——
    //      静默组合 = 用户以为有保护其实没有 → 拒绝（fail-closed）。
    //      当前前端组合不可达（GUI 恒 A、CLI 无 keepold 旗标），属防御性拦截。
    if (!req.engine.empty() && req.engine != "swap" && req.engine != "block") {
        err = Tr("--engine 仅支持 swap（默认，A 引擎）或 block（B 引擎兜底），"
                 "收到未知值已中止，未动目标分区。");
        LogError(err);
        return 2;
    }
    if (req.keepOld && req.engine != "swap") {
        err = Tr("备返（保留旧系统）仅支持默认 A 引擎：--engine block 会格式化"
                 "目标分区并丢弃旧系统。请改用默认引擎，或取消备返。");
        LogError(err);
        AppendHistory("restore-rejected", req.image, req.image, 0,
                      "keepOld requested with block engine");
        return 4;
    }
    // 0.02) 加密镜像（ZJENC1，docs/30 §9 引擎矩阵）：
    //   · A 引擎（swap，Windows 侧解压 ~new）/ PE 就地还原 → 支持；
    //   · B 引擎（block，重启后由 Linux wimlib apply）→ **拒绝**（救援层没有解密器）；
    //   · 常驻菜单（menuEntry）→ **拒绝**（开机阶段没人能输密码）。
    // 口令只放内存（imgsrc 的进程表），不写日志/契约。
    const bool encImage = ImageIsEncrypted(req.image);
    if (encImage) {
        if (!req.password.empty())
            SetImagePassword(req.image, req.password);
        if (menuEntry) {
            err = Tr("这是加密镜像：不能安装到启动菜单（开机阶段无法输入密码）。"
                     "请改用「解压并安装」，或只在需要时用「开始恢复」。");
            LogError(err);
            return 4;
        }
        if (!req.engine.empty() && req.engine == "block") {
            err = Tr("这是加密镜像：不支持「重启后还原」（救援层没有解密能力）。"
                     "请改用默认的「还原后重启」（A 引擎）或在 PE 中就地还原。");
            LogError(err);
            return 4;
        }
        if (GetImagePassword(req.image).empty()) {
            err = Tr("这是加密镜像，需要先输入密码");
            LogError(err);
            return 4;
        }
    }
    // 0) 镜像可用性**快检**（秒级，PIT-057 + PIT-106）：只拒绝"上次没写完/
    //    不完整"的镜像（否则 Linux 侧 apply rc=84，而目标分区已被格式化 →
    //    开机黑屏）；**不做全量扫描**（大镜像十几分钟，用户无法忍受）。
    {
        std::wstring why;
        if (ImageProbe(req.image, why) != 0) {
            if (adv) *adv = ADV_INCOMPLETE;
            err = Tr("镜像不可用，请重新备份：");
            err += W2U(why);
            LogError(err);
            return 5;
        }
    }
    // 0.5) 子镜像检查（方案 C，用户 2026-09-30 规格 + PIT-122 EA 子镜像）：
    //   ① 名含 "ESP" 的子镜像 = ESP 分区备份，**不能单独当系统还原** → 拒绝
    //      （GUI 弹框 / CLI 打印，退出码 4；在一切写盘动作之前）；"ZJEA" 同理；
    //   ② 顺带从镜像**内容**发现 ESP / EA 子镜像 index（esp_index / ea_index，
    //      0=没有）写进契约，救援层 apply 完主系统后按需恢复 ESP、投放 EA 修复。
    int espIndex = 0;
    int eaIndex = 0;
    {
        std::vector<ImageDesc> imgs;
        const int eff = req.index < 1 ? 1 : req.index;
        if (ImageList(req.image, imgs) != 0) {
            // Probe 刚过 → 这里失败几乎不可能；按 0 处理（不恢复 ESP/不投 EA）
            LogWarn("ListImages failed; esp/ea restore disabled");
        } else {
            for (const auto& d : imgs) {
                if (d.name.find(L"ESP") != std::wstring::npos) {
                    if (espIndex == 0)
                        espIndex = d.index;
                    if (d.index == eff) {
                        err = Tr("子镜像「") + W2U(d.name) +
                              Tr("」是 ESP 分区备份，不能单独还原为系统。恢复系统"
                                 "时会自动把 ESP 一并恢复；请选择系统子镜像。");
                        LogError(err);
                        AppendHistory("restore-rejected", req.image, req.image, 0,
                                      "esp image selected as restore target");
                        return 4;
                    }
                }
                if (d.name.find(L"ZJEA") != std::wstring::npos) {
                    if (eaIndex == 0)
                        eaIndex = d.index;
                    if (d.index == eff) {
                        err = Tr("子镜像「") + W2U(d.name) +
                              Tr("」是扩展属性（EA）修复数据，不能单独还原为"
                                 "系统；请选择系统子镜像。");
                        LogError(err);
                        AppendHistory("restore-rejected", req.image, req.image, 0,
                                      "ea image selected as restore target");
                        return 4;
                    }
                }
            }
            if (espIndex > 0)
                LogInfo("esp subimage found in image: index=" +
                        std::to_string(espIndex));
            if (eaIndex > 0)
                LogInfo("ea subimage found in image: index=" +
                        std::to_string(eaIndex));
        }
    }
    // 0.7) 镜像内容预检（I-1，2026-10-06 实测定稿；docs/19 批次 G）：
    //   **规则：镜像里必须有 `\Windows\system32\winload.exe`（Win7+ 的加载器）**，
    //   否则在**写契约/重启之前**拒绝，杜绝"格式化完才发现起不来"。覆盖：
    //   Vista 以前（XP/2003/2000/98）系统镜像、安装源类"镜像"（实测某 XP esd
    //   连 ntoskrnl 都没有）、合成/局部镜像（演练盘素材已补 dummy winload.exe）。
    //   ⚠️ 救援层还有同款兜底（双保险；老契约/绕过此检查时仍安全）。
    {
        const int eff = req.index < 1 ? 1 : req.index;
        bool winload = false;
        if (ImageHasWinload(req.image, eff, &winload) == 0 && !winload) {
            err = Tr("该镜像不是 Windows 7 及以上的系统镜像（缺少 "
                     "\\Windows\\system32\\winload.exe；Vista 以前系统 / 安装源"
                     "/ 不完整镜像均不支持还原），已中止，未动目标分区。");
            LogError(err);
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "not a win7+ system image (no winload.exe)");
            return 4;
        }
    }
    // 0.9) 异机还原前置检查（2026-10-21，0.9）：仅 Win10/11；只允许 Windows
    //      侧路径（A 引擎 / PE 就地）；x64 镜像需 64 位环境（DISM 才能服务）。
    if (req.universal) {
        if (menuEntry) {
            err = Tr("「安装菜单」不支持异机还原（菜单项在 Linux 救援层执行，"
                     "无法注入启动驱动）。请直接执行还原并勾选异机还原。");
            LogError(err);
            return 4;
        }
        int major = 0, minor = 0;
        bool img64 = false;
        std::wstring uerr;
        const int eff2 = req.index < 1 ? 1 : req.index;
        if (ProbeImageOs(req.image, eff2, &major, &minor, &img64, uerr) != 0) {
            err = Tr("异机还原：无法读取镜像内的系统版本（") + W2U(uerr) + Tr("）");
            LogError(err);
            return 1;
        }
        if (!IsUniversalOsSupported(major, minor)) {
            err = Tr("异机还原仅支持 Windows 10/11（镜像版本 ") +
                  std::to_string(major) + "." + std::to_string(minor) +
                  Tr("）。Win7/8.1 请使用同机还原、PE 就地还原或新装系统。");
            LogError(err);
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "universal restore requires Win10/11");
            return 4;
        }
        if (img64 && !IsHost64()) {
            err = Tr("异机还原：镜像是 64 位，但当前环境不是 64 位"
                     "（DISM 无法服务 x64 镜像）。请在 64 位 PE/系统中运行。");
            LogError(err);
            return 4;
        }
        if (!req.driverDir.empty()) {
            int n = 0;
            if (!ValidateDriverFolder(req.driverDir, &n, uerr)) {
                err = Tr("异机还原：驱动文件夹无效（") + W2U(uerr) + Tr("）");
                LogError(err);
                return 4;
            }
            LogInfo("universal: driver pack inf count=" + std::to_string(n));
        }
        LogInfo("universal: image OS " + std::to_string(major) + "." +
                std::to_string(minor) +
                (img64 ? " x64" : " x86") + ", engine=" +
                (req.engine.empty() ? "block" : req.engine));
    }
    // 0.8) Linux 侧无法还原的内容预检（1019 案 F5 + W2；**仅 B 引擎/block**）：
    //   B 引擎重启进 Linux 救援层用 wimlib（NTFS-3G 块模式）apply，有两类内容
    //   还原不了，都会在 **mkntfs 之后**才暴露：
    //   ① 特定重解析点（PIT-135 + 1019 修订：非微软 tag、junction 落非目录
    //      文件等）→ apply rc=58 中止（目标半成品、系统没了）。
    //   ② EFS 加密文件（W2，2026-10-19 实测）→ apply **rc=0 但文件变 0 字节**
    //      （wimlib 只打 "Ignoring EFS-encrypted data"），静默数据丢失。
    //   只读元数据扫描（非微软/junction 零误报子集 + ENCRYPTED 属性）命中即
    //   拒绝 —— 此时还没动任何分区。A 引擎（swap）由 Windows 侧 libwim 解压
    //   （EFS/重解析点都能写回），无需检查。
    //   结果随任务写进契约（reparse_scan / efs_scan：0=干净/2=不可用），救援层
    //   据此决定是否走"契约认证"快路径（W1/W2；老契约缺键 → 救援层自行扫描）。
    int reparseScan = -1;
    int efsScan = -1;
    if (req.engine == "block") {
        WimEngine eng;
        std::vector<std::wstring> bad;
        size_t encrypted = 0;
        const int eff = req.index < 1 ? 1 : req.index;
        int frc =
            eng.ScanImageForLinuxRestore(req.image, eff, bad, &encrypted);
        if (frc == 0 && !bad.empty()) {
            err = Tr("镜像里含 Linux 恢复环境无法写回的重解析点（如「") +
                  W2U(bad[0]) +
                  Tr("」）—— 用「重启后还原」会中止并留下半成品。请改选「还原后"
                     "重启」，或进 PE 模式就地还原；也可用新版软件重新备份后再试。");
            LogError("image has unsettable reparse point(s): " +
                     W2U(bad[0]) + " (" + std::to_string(bad.size()) +
                     " found; rejected before touching target)");
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "image has unsettable reparse point (Linux apply "
                          "would fail; PIT-135)");
            return 4;
        }
        if (frc == 0 && encrypted > 0) {
            err = Tr("镜像里含 EFS 加密文件（") + std::to_string(encrypted) +
                  Tr(" 个），Linux 恢复环境无法还原其内容（还原后这些文件会是"
                     "空的）。请改选「还原后重启」，或进 PE 模式就地还原。");
            LogError("image has EFS-encrypted entries: " +
                     std::to_string(encrypted) +
                     " (rejected before touching target; Linux apply would "
                     "restore them empty)");
            AppendHistory("restore-rejected", req.image, req.image, 0,
                          "image has EFS-encrypted files (Linux apply would "
                          "restore empty files; W2)");
            return 4;
        }
        if (frc != 0) {
            reparseScan = 2;  // 扫描不可用 → 救援层兜底自行判定（fail-closed）
            efsScan = 2;
            LogWarn("linux-restore precheck skipped rc=" + std::to_string(frc) +
                    " (rescue layer will verify)");
        } else {
            reparseScan = 0;  // 已扫描且干净 → 救援层走契约认证快路径
            efsScan = 0;
        }
    }
    // 1) 定位目标分区与所在磁盘
    DiskInfo tdisk;
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if ((int)d.index != req.disk)
            continue;
        for (const auto& p : d.parts) {
            if ((int)p.partNumber != req.part)
                continue;
            tdisk = d;
            target = p;
            found = true;
        }
    }
    if (!found) {
        char buf[96];
        snprintf(buf, sizeof(buf), Tr("找不到目标分区 磁盘%d 分区%d"), req.disk,
                 req.part);
        err = buf;
        return 1;
    }
    // 1b) 软件/镜像若在目标分区（会被格式化）上 → 镜像移动、软件复制到数据盘。
    //     放在 <数据盘>\SysRecover\（持久保留，不参与还原后清理），绝不丢用户镜像。
    std::wstring imagePath = req.image;
    {
        std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        std::wstring keep = dataDrive.empty() ? L"" : dataDrive + L"SysRecover";
        // 8) 镜像在目标分区 → 必须移走，否则还原时被一起格式化掉
        if (tgtLetter && imagePath.size() > 1 && imagePath[1] == L':' &&
            SameDrive(imagePath[0], tgtLetter)) {
            if (keep.empty()) {
                if (adv) *adv = ADV_IMAGE_IN_TARGET;
                err = Tr("镜像在目标分区内，且找不到可存放它的数据盘；请先手动移走镜像");
                return 4;
            }
            CreateDirectoryW(keep.c_str(), nullptr);
            size_t sl = imagePath.find_last_of(L"\\/");
            std::wstring fn =
                sl == std::wstring::npos ? imagePath : imagePath.substr(sl + 1);
            std::wstring dst = keep + L"\\" + fn;
            LogInfo("image on target partition; moving to " + W2U(dst));
            if (!MoveFileExW(imagePath.c_str(), dst.c_str(),
                             MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
                err = Tr("镜像在目标分区内，搬到数据盘失败（空间不足？）；请先手动移走镜像");
                if (adv) *adv = ADV_SPACE;  // 旧关键词版命中的也是"空间不足"（保留原建议）
                return 4;
            }
            if (ImageIsEncrypted(imagePath))
                SetImagePassword(dst, GetImagePassword(imagePath));
            imagePath = dst;
        }
        // 7) 软件在目标分区 → SettleHome（Q1「先落盘后搬家」）：静默整包复制到
        //    安全分区 + 日志跟过去（不 re-exec：当前进程照旧跑完本次操作）。
        std::wstring exeDir0 = ExeDir();
        if (ShouldSettleHome(exeDir0, tgtLetter)) {
            int exeDisk = VolumeDiskIndex(exeDir0[0]);
            int tgtDisk = (int)target.diskIndex;
            SettleHome(tgtDisk, exeDisk);
        }
    }
    // 2) 安全门禁（fail-closed，§11）
    std::string reason = CheckRestoreTarget(target, imagePath, adv);
    if (!reason.empty()) {
        err = reason;
        return 4;
    }
    // 2.05) 目标盘健康（PIT-086，用户 2026-09-26）：坏盘 → 记日志（**展示**由 GUI 弹框 /
    //       CLI 打印负责；这里不阻断 —— 与 BitLocker 一致，取不到 SMART 也放行）。
    {
        std::string hw = CheckDiskHealth((int)target.diskIndex);
        if (!hw.empty())
            LogWarn("target disk health: " + hw);
    }
    // 2.1) 空间预检（P1）：镜像未压缩大小 vs 目标分区大小。
    //      必须在这里（**格式化之前**）拦下 —— 否则会出现"数据没了、系统也没装上"。
    //      就地还原与"暂存+重启"两条路都从这一点过，所以放在分支之前。
    {
        std::string spErr;
        if (CheckRestoreSpace(imagePath, req.index, target, spErr) == 1) {
            err = spErr;
            if (adv) *adv = ADV_SPACE;
            LogError("space precheck failed: " + spErr);
            return 1;
        }
    }
    // 2.2) 就地还原 vs「暂存+重启」**必须在 A 引擎解压之前判定**（2026-10-12
    //      问题 4）：旧顺序是先解压 ~new（几分钟）再判就地，而 PE / 非系统盘
    //      目标走就地 → 接着 format 把刚解压的 ~new 一起格式化掉、再整镜像
    //      apply 第二遍 —— 表现为"100% 后停 16 秒、又从 1% 跑到 100%"
    //      （同一遍镜像解了两遍）。就地时整段 swap（解压/备返/RunOnce）跳过。
    std::string inplaceWhy;
    const bool inPlace = !menuEntry && CanRestoreInPlace(target, inplaceWhy);
    LogInfo(std::string("restore path: ") +
            (inPlace ? "in-place" : (menuEntry ? "menu-entry" : "staged-reboot")) +
            (inplaceWhy.empty() ? "" : " (" + inplaceWhy + ")"));
    // 日常还原（暂存+重启）会使用独立的**临时**引导条目，不能覆盖用户显式
    // 安装的常驻菜单。先让用户清菜单，避免“还原后旧菜单失效”。
    if (!inPlace && !menuEntry && HasInstalledMenu()) {
        err = Tr("已安装「安装菜单」的常驻启动项；为避免覆盖它，日常还原已中止。"
                 "请先在主界面点「清除菜单」，再开始还原。");
        LogError(err);
        AppendHistory("restore-rejected", req.image, req.image, 0,
                      "daily restore while persistent menu installed");
        return 4;
    }
    // 异机还原只能走 Windows 侧路径：暂存的 B 引擎（Linux apply）没有 DISM。
    if (!inPlace && req.universal && req.engine != "swap") {
        err = Tr("异机还原不支持「重启后还原」（B 引擎在 Linux 里执行，无法注入"
                 "启动驱动）。请改选「还原后重启」，或在 PE 中使用就地还原。");
        LogError(err);
        return 4;
    }

    // 2.25) 暂存路径的会话前置（Q2，2026-10-18）：就地还原任何组合放行；
    //       「暂存+重启」/菜单项依赖重启后固件沿当前同一种方式引导，两种必坏
    //       组合提前拒绝（见 bootpath.h::CheckStagedBootPath）。
    if (!inPlace) {
        switch (CheckStagedBootPath(IsUefiFirmware(),
                                     DiskStyleOf(target.diskIndex))) {
            case StagedBootPath::BiosSessionGptTarget:
                err = Tr("当前会话是传统(CSM)引导，目标是 GPT 盘 —— 重启后无法通过 BIOS 链引导。"
                         "请改用 UEFI 模式的 PE/U盘启动后就地还原（无需重启）。");
                LogError("staged boot path rejected: BIOS session + GPT target");
                return 4;
            case StagedBootPath::UefiSessionMbrTarget:
                err = Tr("当前会话是 UEFI 引导，目标是 MBR 盘 —— 重启后 UEFI 无法执行传统引导链。"
                         "请改用传统(CSM)模式的 PE/U盘启动后就地还原（无需重启）。");
                LogError("staged boot path rejected: UEFI session + MBR target");
                return 4;
            case StagedBootPath::Ok:
                break;
        }
    }

    // A 引擎残留门禁（就地与暂存共用，docs/23 §5）：目标盘还有未处理的 ~old
    // （用户选「保留不动」/清理未跑）→ 不动盘、中止；重新运行程序会再次询问。
    // 就地还原**同样要拦**：就地 = format 整个目标分区，等于不询问就删掉用户
    // 刚刚选择「保留」的旧系统。
    auto swapLeftGuard = [&]() -> bool {
        if (target.letter.empty()) {
            err = Tr("目标分区无盘符，A 引擎无法解压");
            return false;
        }
        WaitSwapCleanup(target.letter[0]);  // 等启动期后台残留清理收尾
        DWORD a = GetFileAttributesW((target.letter + L":\\~old").c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            err = Tr("目标盘存在未处理的 ~old 残留，A 引擎还原已中止"
                     "（重新运行程序会再次询问如何处理）");
            LogError(err);
            return false;
        }
        return true;
    };

    // ── 就地还原：到此直接执行（不写契约、不部署引导、不重启）────────────
    if (inPlace) {
        if (req.engine == "swap" && !swapLeftGuard())
            return 4;
        if (!AcquireOpLock()) {
            err = Tr("已有备份/还原实例在运行");
            return 1;
        }
        ProgressUpdate("restore", 0, "start");
        LogInfo("restore stage image=" + W2U(imagePath) + " disk=" +
                std::to_string(req.disk) + " part=" + std::to_string(req.part));
        LogInfo(std::string("restore mode: in-place (") + inplaceWhy + ")");
        const ULONGLONG t1 = GetTickCount64();
        // 加密镜像：就地还原是 Windows 侧读 → 解密到同卷临时明文（用完即删）。
        // 放在"镜像搬出目标分区"之后，避免临时明文被格式化掉。
        std::wstring plainImage = imagePath;
        bool plainTemp = false;
        if (ImageIsEncrypted(imagePath)) {
            std::wstring perr;
            plainImage = ResolveImagePlain(imagePath, progress, perr, &plainTemp);
            if (plainImage.empty()) {
                err = W2U(perr);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                return 1;
            }
        }
        int rc = RunDirectRestore(req, target, plainImage, espIndex, eaIndex,
                                  err, progress);
        if (plainTemp)
            ReleaseImagePlain(imagePath);
        if (needReboot)
            *needReboot = false;
        ReleaseOpLock();
        AppendHistory("restore-inplace", req.image, imagePath,
                      GetTickCount64() - t1, rc == 0 ? "ok" : "failed");
        return rc;
    }

    // 临时引导项的首启清理需要知道 A 引擎的 ~new 位置（B 引擎没有 ~new，
    // 目前只能记录告警；见下方 InstallFirstBootCleanup 调用）。
    std::wstring swapNewDir;
    // 加密镜像：Windows 侧暂存的 ESP/EA 子镜像相对目录（写进契约 esp_fix_dir/
    // ea_fix_dir，救援层只做拷贝）。非加密镜像恒为空。
    std::wstring espFixRel;
    std::wstring eaFixRel;
    // ── A 引擎（0.7，docs/21/22/23）：Windows 原生解压到 <目标>:\~new ──
    // 必须在写契约/部署引导之前完成：解压成功才继续。失败/中断 → 报错、不碰
    // 旧系统、清理 ~new（L1 定案：让用户换镜像重试；断电由下次启动清孤儿）。
    if (req.engine == "swap") {
        if (!swapLeftGuard())
            return 4;
        unsigned long long content = 0;
        {
            std::vector<ImageDesc> imgs;
            if (ImageList(imagePath, imgs) == 0) {
                int want = req.index < 1 ? 1 : req.index;
                for (const auto& im : imgs)
                    if (im.index == want) content = im.sizeBytes;
            }
        }
        if (progress) progress(1, "extract");
        //「刚发生的接着处理」：近期完整 ~new 且 content 与所选镜像一致 → 复用
        // （免重解压）；过期/不完整/镜像已换 → 丢弃重解（docs/23 §5 决策表）。
        bool reuse = false;
        // 加密镜像：Windows 侧暂存的 ESP/EA 子镜像相对目录（见外层的
        // espFixRel/eaFixRel 声明）。
        std::wstring newDir = target.letter + L":\\~new";
        swapNewDir = newDir;
        {
            DWORD a = GetFileAttributesW(newDir.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
                SwapLeftInfo ni;
                if (ProbeSwapNew(target.letter[0], ni)) {
                    SwapLeftVerdict v = DecideSwapLeftover(ni);
                    if (v.action == SwapLeftAction::Reuse && content > 0 &&
                        ni.markerContent == content) {
                        reuse = true;
                        LogInfo(
                            "A-engine: reuse existing ~new (content matches "
                            "selected image)");
                    } else {
                        LogInfo("A-engine: discard leftover ~new (" + v.reason +
                                (v.action == SwapLeftAction::Reuse
                                     ? "; content mismatch, redo extract"
                                     : "") +
                                ")");
                        DeleteTreeRec(newDir);
                    }
                } else {
                    DeleteTreeRec(newDir);  // 探测竞态 → 保守删除
                }
            }
        }
        if (!reuse) {
        SwapSpaceEstimate se;
        if (!EstimateSwapSpace(target, content, se) || !se.enough) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     Tr("A 引擎空间不足：需要约 %.1f GB（内容+页文件+余量），"
                        "目标盘可用 %.1f GB。请清理磁盘后重试，或改用 "
                        "`--engine block`（B 引擎兜底：先格式化再写入，只需"
                        "目标总容量 ≥ 镜像体积）。"),
                     se.required / 1073741824.0, se.freeBytes / 1073741824.0);
            err = buf;
            if (adv) *adv = ADV_SPACE;
            LogError(err);
            return 1;
        }
        LogInfo("A-engine: extract to " + W2U(newDir) + " (content " +
                std::to_string(content) + " bytes)");
        WimEngine we;
        if (!we.ok()) {
            err = Tr("wimlib 初始化失败");
            return 1;
        }
        // 加密镜像：Windows 侧解压前解密到同卷临时明文（A 引擎不用 Linux 读镜像）。
        std::wstring plainImage = imagePath;
        bool plainTemp = false;
        if (ImageIsEncrypted(imagePath)) {
            std::wstring perr;
            plainImage = ResolveImagePlain(imagePath, progress, perr, &plainTemp);
            if (plainImage.empty()) {
                err = W2U(perr);
                DeleteTreeRec(newDir);
                return 1;
            }
        }
        int erc = we.Apply(plainImage, req.index < 1 ? 1 : req.index, newDir,
                           progress);
        if (erc != 0) {
            DeleteTreeRec(newDir);
            if (plainTemp)
                ReleaseImagePlain(imagePath);
            char b2[160];
            snprintf(b2, sizeof(b2), Tr("A 引擎解压失败(rc=%d): "), erc);
            err = b2;
            err += W2U(WimEngine::ErrorString(erc));
            LogError(err);
            return 1;
        }
        // ── 加密镜像的 ESP / EA 子镜像：Windows 侧暂存（docs/30 §9）──
        // 救援层读不了加密容器 → 在这里把子镜像解到 ~new\ZJRESTORE\{espfix,eafix}，
        // 契约改用 esp_fix_dir/ea_fix_dir（esp_index/ea_index 置 0），Linux 只做拷贝。
        // 非加密镜像保持原行为（Linux 用 wimlib apply esp_index/ea_index）。
        if (ImageIsEncrypted(imagePath) && (espIndex > 0 || eaIndex > 0)) {
            const std::wstring zjRoot = newDir + L"\\ZJRESTORE";
            MakeDirTree(zjRoot);
            WimEngine xe;
            if (espIndex > 0) {
                const std::wstring dst = zjRoot + L"\\espfix";
                DeleteTreeRec(dst);
                MakeDirTree(dst);
                int xrc = xe.Apply(plainImage, espIndex, dst, ProgressFn{});
                if (xrc == 0) {
                    espFixRel = L"ZJRESTORE/espfix";
                    LogInfo("encrypted: ESP subimage staged to ~new\\ZJRESTORE\\espfix");
                } else {
                    LogWarn("encrypted: ESP subimage stage failed rc=" +
                            std::to_string(xrc) +
                            " (main system continues without ESP restore)");
                    DeleteTreeRec(dst);
                }
            }
            if (eaIndex > 0) {
                const std::wstring dst = zjRoot + L"\\eafix";
                DeleteTreeRec(dst);
                MakeDirTree(dst);
                int xrc = xe.Apply(plainImage, eaIndex, dst, ProgressFn{});
                if (xrc == 0) {
                    eaFixRel = L"ZJRESTORE/eafix";
                    LogInfo("encrypted: EA subimage staged to ~new\\ZJRESTORE\\eafix");
                } else {
                    LogWarn("encrypted: EA subimage stage failed rc=" +
                            std::to_string(xrc) +
                            " (main system continues without EA fix)");
                    DeleteTreeRec(dst);
                }
            }
            // 暂存成功后才把契约里的 esp_index/ea_index 清掉（避免 Linux 去读密文）
            if (!espFixRel.empty())
                espIndex = 0;
            if (!eaFixRel.empty())
                eaIndex = 0;
        }
        if (plainTemp)
            ReleaseImagePlain(imagePath);
        // 异机还原（2026-10-21）：交换之前注入启动驱动；失败则丢弃 ~new，
        // 旧系统原样保留（可回退）。
        if (req.universal) {
            std::string ulog;
            std::wstring uerr;
            auto uProgress = [&](int pct, const std::string& st) {
                ProgressUpdate("restore", pct, st);
                return progress ? progress(pct, st) : false;
            };
            if (!PrepareUniversal(newDir, req.driverDir, ulog, uerr,
                                  uProgress)) {
                DeleteTreeRec(newDir);
                err = Tr("异机还原准备失败：") + W2U(uerr);
                LogError(err);
                LogError("universal log:\n" + ulog);
                return 1;
            }
            LogInfo("universal: A-engine prepared\n" + ulog);
        }
        {   // 完成标记：zjswap.sh 先验后动；无标记 = 不完整 → 拒绝交换
            SYSTEMTIME stw = {};
            GetLocalTime(&stw);
            char line[160];
            int len = snprintf(
                line, sizeof(line),
                "content=%llu time=%04d-%02d-%02d %02d:%02d:%02d\n", content,
                stw.wYear, stw.wMonth, stw.wDay, stw.wHour, stw.wMinute,
                stw.wSecond);
            HANDLE h = CreateFileW((newDir + L"\\.zj-done").c_str(),
                                   GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(h, line, (DWORD)len, &wr, nullptr);
                CloseHandle(h);
            }
        }
        LogInfo("A-engine: ~new ready (with .zj-done marker)");
        }  // if (!reuse)
        // 复用旧 ~new 时（reuse=true）不重解子镜像：若上一轮已暂存过，契约照样要写
        // esp_fix_dir/ea_fix_dir，否则救援层会以为没有 ESP/EA。
        if (ImageIsEncrypted(imagePath)) {
            if (espFixRel.empty() &&
                GetFileAttributesW((newDir + L"\\ZJRESTORE\\espfix").c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                espFixRel = L"ZJRESTORE/espfix";
            if (eaFixRel.empty() &&
                GetFileAttributesW((newDir + L"\\ZJRESTORE\\eafix").c_str()) !=
                    INVALID_FILE_ATTRIBUTES)
                eaFixRel = L"ZJRESTORE/eafix";
            if (!espFixRel.empty())
                espIndex = 0;
            if (!eaFixRel.empty())
                eaIndex = 0;
        }
        // ── keepOld 处理 ─────────────────────────────────────────────
        // keepOld=true  → 写 .zj-keepold 标记 + 部署程序到 ~new\Program Files\SysRecover
        //                 + 建桌面快捷方式（2026-10-15 恢复 e5fb0a7 部署代码；
        //                 用户可直接点快捷方式处理 ~old）。
        // keepOld=false → RunOnce 静默 .cmd 删 ~old（开机自动清，无人值守默认）。
        // 失败仅 WARN 不阻断（用户可手动启动程序处理）。
        if (req.keepOld) {
            // 1. 写备返标记到目标根（zjswap.sh 会把它移到 ~old/.zj-keepold）
            {
                const std::wstring marker = target.letter + L":\\.zj-keepold";
                HANDLE hm = CreateFileW(marker.c_str(), GENERIC_WRITE, 0,
                                        nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hm != INVALID_HANDLE_VALUE) {
                    const char txt[] = "keepold=1";
                    DWORD wr = 0;
                    WriteFile(hm, txt, sizeof(txt) - 1, &wr, nullptr);
                    CloseHandle(hm);
                    LogInfo("A-engine: keepold marker written to target root");
                } else {
                    LogWarn("A-engine: keepold marker write failed err=" +
                            std::to_string(GetLastError()));
                }
            }
            // 2. 部署完整程序到 ~new 下的 Program Files\SysRecover（行末不能以
            //    \ 结尾 —— 反斜杠会把下一行吃进注释，导致 progDst "未声明"）
            const std::wstring progDst =
                newDir + L"\\Program Files\\SysRecover";
            const bool copied = CopyDir(AppDir(), progDst);
            if (copied) {
                LogInfo("A-engine: program deployed to " + W2U(progDst));
                // 3. 在新系统**公用桌面**创建快捷方式（换位后生效）。
                //    用户 2026-10-15 规格：只放公用桌面一份 —— 个人桌面 + 公用
                //    桌面会出现两个图标（精简镜像的 Public\Desktop 对所有用户
                //    生效，再往个人桌面放就是重复）。
                //    目标必须写换位后的最终路径：~new 换位后变成 <盘>:\Program Files\...
                const std::wstring pubDesk =
                    newDir + L"\\Users\\Public\\Desktop";
                CreateDirectoryW(pubDesk.c_str(), nullptr);
                const std::wstring lnkTarget =
                    target.letter +
                    L":\\Program Files\\SysRecover\\SysRecoverUI.exe";
                std::wstring lnkPath;
                if (CreateShortcut(pubDesk, L"SysRecover", lnkTarget, L"",
                                   lnkPath)) {
                    LogInfo("A-engine: shortcut created: " + W2U(lnkPath));
                } else {
                    LogWarn("A-engine: shortcut create failed: " +
                            W2U(pubDesk));
                }
                // 4. RunOnce：新系统**首启自动运行本程序**（用户 2026-10-17 规格）。
                //    首启流程 = 自动启动 → 把救援层/旧版留在数据盘的日志搬回程序
                //    目录（MigrateLogsToLogHome）→ 才弹「是否删除旧系统」三选框。
                //    路径用 %SystemDrive% 让**新系统**自己展开（还原到别的盘也正确；
                //    含空格的 "Program Files" 必须带引号），REG_EXPAND_SZ 才支持展开。
                InjectOfflineRunOnce(
                    newDir, L"ZJFIRSTBOOT", L"ZJSysRecoverFirstBoot",
                    L"\"%SystemDrive%\\Program Files\\SysRecover\\SysRecoverUI.exe\"",
                    REG_EXPAND_SZ, "firstboot");
            } else {
                LogWarn("A-engine: program deploy failed, skip shortcut");
            }
        } else {
            // keepOld=false → 原方案：RunOnce 静默 .cmd 删 ~old
            // 先清掉上一次「备返」可能留在目标根的标记：否则它会被 zjswap 一起
            // 移进 ~old，让本应自动删除的 ~old 变成 AskUndo（用户明明没勾备返）。
            {
                const std::wstring marker = target.letter + L":\\.zj-keepold";
                if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    if (DeleteFileW(marker.c_str()))
                        LogInfo("A-engine: stale keepold marker removed");
                    else
                        LogWarn("A-engine: stale keepold marker remove failed err=" +
                                std::to_string(GetLastError()));
                }
            }
            const std::wstring cmdPath =
                newDir + L"\\Windows\\System32\\zj-swap-cleanup.cmd";
            static const wchar_t kCmd[] =
                L"@echo off\r\n"
                L"if exist \"%SystemRoot%\\System32\" "
                L"if not exist \"%SystemDrive%\\~new\" (\r\n"
                L"  rd /s /q \"%SystemDrive%\\~old\" >nul 2>&1\r\n"
                L")\r\n"
                L"del /f /q \"%~f0\" >nul 2>&1\r\n";
            HANDLE hc = CreateFileW(cmdPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
            if (hc != INVALID_HANDLE_VALUE) {
                DWORD wr = 0;
                WriteFile(hc, kCmd, (DWORD)(sizeof(kCmd) - 2), &wr, nullptr);
                CloseHandle(hc);
                InjectOfflineRunOnce(newDir, L"ZJSWAPCLEANUP",
                                     L"ZJSwapCleanup",
                                     L"%SystemRoot%\\System32\\zj-swap-cleanup.cmd",
                                     REG_SZ, "cleanup");
            } else {
                LogWarn("A-engine: cleanup cmd write failed err=" +
                        std::to_string(GetLastError()));
            }
        }
    }
    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("restore", 0, "start");
    LogInfo("restore stage image=" + W2U(imagePath) + " disk=" +
            std::to_string(req.disk) + " part=" + std::to_string(req.part));

    // 2.5) 就地还原分支**已在 2.2 提前返回**（问题 4：必须赶在 A 引擎解压之前，
    //      否则 ~new 白解压一遍还被自己的 format 掉）—— 这里只会走到两种模式：
    //      暂存+重启（PIT-064）或常驻菜单项。判定结果复用 2.2 的 inPlace/inplaceWhy，
    //      不再二次调用 CanRestoreInPlace（会再次开/锁目标卷）。
    {
        if (menuEntry) {
            LogInfo("restore mode: menu entry (contract + persistent boot entry; "
                    "no reboot)");
        } else {
            LogInfo(std::string("restore mode: staged reboot (") + inplaceWhy + ")");
        }
        // 网络镜像防呆（问题清单 D1/H1）：暂存+重启后由 **Linux 救援层**读镜像，而救援层
        // 访问不到网络（UNC / 映射网络驱动器）→ 必然 image not found。这里**提前拒绝**
        //（就地还原不在此列 —— 那是 Windows 自己读，网络没问题）。
        if (IsNetworkPath(imagePath)) {
            err =
                Tr("镜像在网络路径上（UNC 或映射网络驱动器）。还原系统盘需要重启进救援层，" "而救援层无法访问网络 —— 请先把镜像复制到**本地分区**（如 D:\\）再还原。");
            LogError("staged restore rejected: image on network path");
            AppendHistory("restore-rejected", req.image, imagePath, 0,
                          "image on network path");
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            return 2;
        }
        if (menuEntry)
            AppendHistory("restore-menu", req.image, imagePath, 0,
                          "menu entry installed (no reboot)");
        else
            AppendHistory("restore-staged", req.image, imagePath, 0,
                          "staged; 实际结果见救援层日志");
    }

    std::wstring exeDir = ContractDir();  // Q1 settle 后返回新家；否则 = ExeDir()
    // 日志根同 LogBaseDir（PIT-105）：软件在系统盘时 BCD 备份也放数据盘，
    // 免得随系统盘一起被格式化。
    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    if (req.repairBoot)
        BcdExport(logsDir + L"\\bcd-backup");  // 改 BCD 前先备份（§11）

    // 3) 组装任务（字段对齐旧 C# StageRestoreAsync）
    RestoreTask t;
    t.imagePath = imagePath;
    t.imageIndex = req.index < 1 ? 1 : req.index;
    t.espIndex = espIndex;  // 方案 C：ESP 子镜像 index（0=镜像里没有）
    t.eaIndex = eaIndex;    // PIT-122：EA 修复子镜像 index（0=镜像里没有）
    // 加密镜像：子镜像已由 Windows 解到目标分区，救援层只拷贝（不读密文镜像）。
    t.espFixDir = espFixRel;
    t.eaFixDir = eaFixRel;
    t.reparseScan = reparseScan;  // 1019 案：0=干净 / 2=扫描不可用 / -1=未扫描
    t.efsScan = efsScan;  // 1019 案 W2：同义（EFS 加密内容预检结果）
    {   // 镜像字节数（1019 案）：救援层"契约认证"精确比对用（SIZE 判"镜像未变"）
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(imagePath.c_str(), GetFileExInfoStandard, &fad))
            t.imageSize = ((uint64_t)fad.nFileSizeHigh << 32) |
                          (uint64_t)fad.nFileSizeLow;
    }
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = req.disk;
    t.targetPart = req.part;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = req.repairBoot;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    t.engine = req.engine;  // 0.7：""/block=B 引擎；swap=A 引擎（docs/23）
    t.keepOld = req.keepOld;  // PIT-148：写进 conf，供「一键重试」原样复刻任务
    // 诊断日志目录（PIT-059）：优先软件目录本身；软件在目标盘（会被格式化）
    // 或只读介质上时，回退到数据盘 <数据盘>\ZJRESTORE（建好，保留给排错用）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        wchar_t tgtLetter = target.letter.empty() ? 0 : target.letter[0];
        wchar_t exRoot[4] = {exLetter, L':', L'\\', 0};
        bool onTarget =
            exLetter && tgtLetter && SameDrive(exLetter, tgtLetter);
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (!onTarget && fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            if (dataDrive.empty())
                t.softwareDir = exeDir;
            else
                t.softwareDir = dataDrive + kRecoveryDir;  // D:\ZJRESTORE
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
            LogInfo("software dir unusable for logs; log dir=" +
                    W2U(t.softwareDir));
        }
    }
    // 镜像位置：盘符 → 所在分区 GUID + 相对路径（正斜杠）
    if (imagePath.size() > 3 && imagePath[1] == L':') {
        wchar_t letter = towupper(imagePath[0]);
        for (const auto& d : EnumerateDisks())
            for (const auto& p : d.parts)
                if (!p.letter.empty() && towupper(p.letter[0]) == letter)
                    t.imagePartGuid = p.guid;
        std::wstring rel = imagePath.substr(3);
        for (auto& c : rel)
            if (c == L'\\')
                c = L'/';
        t.imageRelPath = rel;
    }

    // 4) _zjresy 日志写目标分区根（restore.sh 主契约；缺日志 Linux 侧找不到
    //    目标，视为致命——较旧 C# 的 warn 更严格，fail-closed）
    if (target.letter.empty()) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    std::string tlog;
    // A2（用户 2026-10-04 规格）：写新契约前，把**所有盘根**上的旧契约清掉，
    // 全系统只留即将写入的这一份 —— 救援层就不会挑到别处残留的 _zjresy 把目标
    // 指错（offset 校验仍保留作第二层防护）。
    {
        std::string clog;
        int cleaned = CleanupStrayContracts(clog);
        LogInfo("stray contracts cleanup: " + std::to_string(cleaned) +
                " removed (" + clog.substr(0, clog.size() - 1) + ")");
    }
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        if (adv) *adv = ADV_ADMIN;
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
        LogError(err + " / " + tlog);
        return 1;
    }
    // 5) 任务文件（**副契约**）：优先写 exe 目录；只读介质（光盘）或写不了时回退到
    //    <数据盘>\ZJRESTORE\（救援层会扫描各分区找它，PIT-035）。
    //    ⚠️ **写不了也不致命**：主契约是**目标分区根**上的 _zjresy 日志，救援层能从
    //    日志字段读出全部参数（PIT-035 的回退）。2026-09-23 实测：用户从**光盘**运行，
    //    这一步失败 + 被当成致命错误 → 还原根本没执行 ✗（而日志其实已经写好了 ✓）。
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("restore task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("restore task written to " + W2U(used));
    }
    // 引导层失败 → **契约回滚**（PIT-126，用户 2026-10-06 论坛反馈）：
    // 契约（目标根 _zjresy + exeDir restore-task.conf）在引导层**之前**写 ——
    // 菜单绑定/暂存任务都需要它；但引导层失败 = 任务永远不会执行 → 契约必须
    // 撤销，否则：① GUI 误判"已安装"（按钮变「删除菜单」，实为只写了两份契约）；
    // ② 目标根留一份 action=restore 的陈旧日志，将来进救援层可能被误执行。
    // RAII 守卫：repairBoot=true 时武装，之后**任何失败 return 都自动回滚**；
    // 成功路径在 return 0 前 disarm。
    bool rollbackArmed = req.repairBoot;
    struct RollbackGuard {
        bool* armed;
        ~RollbackGuard() {
            if (!*armed) return;
            std::string rlog;
            int n = CleanupStrayContracts(rlog);
            DeleteMenuBinding();
            std::wstring dd = FindDataDrive();
            if (!dd.empty()) {
                DeleteFileW((dd + L"ZJRESTORE\\restore-task.conf").c_str());
                DeleteFileW((dd + L"ZJRESTORE\\restore-task.json").c_str());
            }
            LogWarn("boot layer failed -> staged contracts rolled back (" +
                    std::to_string(n) + " stray removed)");
        }
    } rollbackGuard{&rollbackArmed};

    // 6) 引导层 + 单次启动（仅正常 Windows，§2 禁令3）
    // 走哪条链**看目标盘的分区风格**（MBR→BIOS/GRUB4DOS；GPT→UEFI/ESP），不看固件类型：
    // UEFI 固件 + MBR 盘的机器（CSM/Legacy 装的 Windows）上没有 ESP，必须走 BIOS 链。
    const bool useUefi = UseUefiBootFor(target);
    LogInfo(std::string("boot path: ") + (useUefi ? "UEFI/ESP" : "BIOS/GRUB4DOS") +
            " (firmware=" + (IsUefiFirmware() ? "UEFI" : "BIOS") +
            ", target disk style=" + StyleName(DiskStyleOf(target.diskIndex)) + ")");
    if (req.repairBoot) {
        std::string blog;
        // 进度收官标记（2026-10-18）：apply 之后引导层还要写 ~28MB 救援载荷 +
        // 逐文件 CRC 校验（暂存与菜单安装都走这里），期间没有字节级进度 →
        // 切一个 "boot" 阶段名，让界面显示「写入引导文件」而不是停在「写入 100%」。
        if (progress) progress(99, "boot");
        if (useUefi) {
            // UEFI/GPT：写**固件启动项**（Boot#### + BootOrder），由固件直接加载
            // 内核（EFI stub）并把 OptionalData 当内核命令行 —— 全程不经 bootmgr，
            // 因此没有 bootapp 的 0xc000007b（PIT-060）。ESP 不受还原影响。
            SbMode sbm = CurrentSbMode();
            if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
                // Secure Boot 开着且走 shim 链，但用户还没做过那次一次性密钥注册：
                // 此时重启会被 MokManager 拦住，还原根本不会跑（看起来"什么都没
                // 发生"）。必须在**动目标分区之前**先拦住并说清怎么注册。
                // （走 bootapp 链不需要注册，见 PIT-063。）
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再还原");
                LogError(err);
                return 1;
            }
            std::wstring espRoot;
            bool guidMatched = false;
            std::string detail;
            if (!AcquireEspRoot(espRoot, guidMatched, blog,
                                 (int)target.diskIndex)) {
                LogError("ESP acquire log (staged):\n" + blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                         "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                         " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                      W2U(DescribePartitions());
                LogError(err);
                return 1;
            }
            if (!guidMatched)
                LogWarn("ESP partition type is not EFI System; used fallback "
                        "(mountvol/\\EFI files)");
            // ① 目标系统盘得有 bcdboot 要的模板（docs/15 · P2c）
            if (!TargetBcdTemplateOk(target.letter[0], detail)) {
                UnmountEsp(espRoot, blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("目标系统缺少引导模板（") + detail +
                      Tr("）：bcdboot 无法生成引导。多见于第三方精简/万能镜像，"
                         "请换用完整系统镜像。");
                LogError(err);
                return 1;
            }
            // ② ESP 还得放得下 bcdboot 要写的 ~9MB（docs/15 · P2a）
            if (!EspHasRoomForBcdboot(espRoot, detail)) {
                UnmountEsp(espRoot, blog);
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("ESP 空间不足，无法修复引导（") + detail +
                      Tr("）：请先在 Windows 里点「删除启动还原」清掉 "
                         "\\EFI\\ZJRESTORE\\ 旧版残留，或用 diskpart 扩大 ESP。");
                LogError(err);
                return 1;
            }
            // ③ **先跑 bcdboot**（此刻 ESP 空间最富裕），再放我们 33MB 的救援载荷
            //    —— 顺序反了就会被自己的载荷挤掉空间（docs/15 · P2b）。
            //    0.6.35（用户 2026-10-05）：**BCD 已经有效就跳过 bcdboot** —— 它
            //    会重建 BCD（只剩 Windows 条目），第三方启动项（易数等）被抹掉。
            bool ok = true;
            {
                // 用**目标分区**的 Windows 目录（暂存时它还是旧系统，但引导文件一样）
                std::wstring winDir =
                    std::wstring(1, target.letter[0]) + L":\\Windows";
                std::string out;
                std::string vd;
                if (VerifyEspBcd(espRoot, winDir, vd)) {
                    LogInfo("ESP BCD already valid -> skip bcdboot (preserve "
                            "third-party boot entries): " + vd);
                } else {
                    LogInfo("ESP BCD invalid -> rebuild: " + vd);
                    // 2026-10-27（用户反馈 2）：bcdboot 缺失/失败 → 随包资产自足
                    // （极简 PE 里没有 bcdboot；以前这里直接失败、只能拿 fixboot.exe 修）。
                    bool rebuilt = false;
                    if (ToolAvailable(L"bcdboot.exe")) {
                        std::wstring args = winDir + L" /s " +
                                            std::wstring(1, espRoot[0]) + L": /f UEFI";
                        int brc = RunProcess(SysToolPath(L"bcdboot.exe"), args, out);
                        LogInfo("bcdboot (UEFI) rc=" + std::to_string(brc) + " / " +
                                out);
                        rebuilt = brc == 0;
                    } else {
                        LogInfo("bcdboot.exe missing -> self-contained ESP boot");
                    }
                    if (!rebuilt) {
                        std::string slog, serr;
                        const bool sok = EnsureUefiBootFiles(
                            espRoot, std::wstring(1, target.letter[0]) + L":\\",
                            winDir, exeDir, /*overwriteBcd=*/true, slog, serr);
                        LogInfo(std::string("ESP self-contained boot: ") +
                                (sok ? "ok" : "FAILED") + "\n" + slog);
                        std::string wlog;
                        if (EnsureWindowsBootEntry(espRoot, wlog))
                            LogInfo("firmware Windows boot entry: " + wlog);
                        else
                            LogWarn("firmware Windows boot entry (non-fatal): " +
                                    wlog);
                        if (!sok) {
                            UnmountEsp(espRoot, blog);
                            ReleaseOpLock();
                            ProgressDone("restore", "failed");
                            err = Tr("写入 ESP 引导失败（无法生成引导配置）：") + serr;
                            LogError(err);
                            return 1;
                        }
                    }
                    // ④ 产物断言：displayorder 非空 + 有 winload 条目 + 关键文件齐全
                    if (!VerifyEspBcd(espRoot, winDir, detail)) {
                        UnmountEsp(espRoot, blog);
                        ReleaseOpLock();
                        ProgressDone("restore", "failed");
                        err = Tr("ESP 引导校验未通过（未真正修好，已中止，未动目标分区）：") +
                              detail;
                        LogError(err);
                        return 1;
                    }
                }
            }
            // ⑤ 救援载荷（内核 + initramfs ≈ 33MB；它自带空间检查）
            ok = InstallUefiBootEntry(espRoot, exeDir, blog, !menuEntry);
            UnmountEsp(espRoot, blog);
            LogInfo(std::string("uefi boot entry: ") + (ok ? "ok" : "FAIL") +
                    " / " + blog);
            if (!ok) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("安装 UEFI 引导层失败: ") + blog;
                LogError(err);
                return 1;
            }
        } else {
            // BIOS/MBR：部署到**目标分区**（通常 C:）；救援文件会被本次还原格式化掉，
            // 所以不必去动数据盘（D:/E:）的根目录。
            std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
            // 每次都重装（幂等）：确保 menu.lst/bootfiles/initramfs 随本版本刷新。
            if (!InstallBootLayer(deployDrive, exeDir, blog, !menuEntry)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("安装引导层失败: ") + blog;
                LogError(err);
                return 1;
            }
            // 引导补全：生成 <目标>\ZJRESTORE\bootfix\（Linux 侧在 mkntfs 前拷进内存）
            {
                std::wstring rec = deployDrive + kRecoveryDir;
                std::string flog;
                bool bok = PrepareBootFixFiles(rec, flog);
                LogInfo(std::string("bootfix: ") + (bok ? "ok" : "FAILED") +
                        " / " + flog);
                if (!bok)
                    LogError("prepare bootfix failed: " + flog);
            }
        }
        // 单次启动（都只生效一次）：
        //   UEFI + shim/direct → 固件 BootNext（写后读回校验）
        //   UEFI + bootapp     → BCD bootsequence（此处再校验一遍）
        //   BIOS/MBR           → BCD bootsequence（写后读回校验）
        // 单次启动**只在"暂存+重启"模式设**；菜单项模式保持"用户自己选"（用户规格）。
        // 三处都校验：安全软件可让命令返回 0 但不落盘 → 重启照旧进原系统（用户反馈）。
        if (menuEntry) {
            // 常驻菜单**可被选中**的前提：{bootmgr} timeout > 0 —— timeout=0 时
            // bootmgr 不停留直接进 Windows，按什么键都没用（用户 2026-09-29 实测；
            // timeout=0 来源：还原后的系统 BCD 被 bootfix 写成 0，见 bootfix.cpp）。
            // 策略**只升不降**：原值 ≥1 一律保留（用户："原来大于 1 就保留原值"），
            // 0/缺失补成 1 秒（用户：1 秒足够用上下键选、几乎不拖慢开机）。
            // 仅 BCD 菜单路径需要（BIOS，或 Secure Boot 的 bootapp 模式）；
            // UEFI 常驻项在固件 BootOrder（开机按快捷启动键选，键名按品牌识别：
            // common/bioskey），与 bootmgr timeout 无关。
            if (!useUefi || CurrentSbMode() == SbMode::Bootapp) {
                std::string tlog;
                int cur = BcdGetBootmgrTimeout(tlog);
                if (cur < 1) {
                    if (!BcdSetBootmgrTimeout(1, tlog)) {
                        ReleaseOpLock();
                        ProgressDone("restore", "failed");
                        err = Tr("设置启动菜单停留时间失败（菜单将无法被选中）: ") +
                              tlog + BootWriteBlockedHint();
                        LogError(err);
                        return 1;
                    }
                    LogInfo("bootmgr timeout: " + std::to_string(cur) +
                            " -> 1s (menu selectable)");
                } else {
                    LogInfo("bootmgr timeout kept at " + std::to_string(cur) +
                            "s (>=1, menu selectable)");
                }
            }
            LogInfo("menu entry mode: no BootNext/bootsequence (user picks it in "
                    "the boot menu)");
        } else if (useUefi && CurrentSbMode() != SbMode::Bootapp) {
            if (!SetUefiBootNext(blog, true)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("设置固件单次启动失败: ") + blog +
                      BootWriteBlockedHint();
                LogError(err);
                return 1;
            }
        } else if (!useUefi || CurrentSbMode() == SbMode::Bootapp) {
            std::string slog;
            if (!BcdSetBootsequence(TemporaryGuid(), slog)) {
                ReleaseOpLock();
                ProgressDone("restore", "failed");
                err = Tr("设置单次启动失败（重启后可能进不了还原环境）: ") + slog +
                      BootWriteBlockedHint();
                LogError(err);
                return 1;
            }
            LogInfo("bootsequence verified: " + slog);
        }
    }
    // 日常还原（非安装菜单）→ 新系统首启自动清掉**临时**启动项/载荷。
    // 只处理 A 引擎（有离线 ~new 可写 RunOnce）；B 引擎当前没有离线 hive。
    if (!menuEntry) {
        if (req.engine == "swap" && !swapNewDir.empty()) {
            std::string clog;
            if (!InstallFirstBootCleanup(swapNewDir, exeDir, clog)) {
                LogWarn("first-boot cleanup NOT installed: " + clog);
            } else {
                LogInfo(clog);
            }
        } else {
            LogWarn("first-boot cleanup skipped (engine has no offline ~new; "
                    "temporary UEFI/BCD entry may remain until manual clear)");
        }
    }

    // 把本次部署到目标盘的痕迹收进 logs（用户 2026-10-03 规格）：盘根的
    // grldr/grldr.mbr/menu.lst/_zjresy 日志 + ZJRESTORE\bootfix|scripts|logs + 清单。
    // 用户排错只需发 logs 文件夹（含这些 + diag.txt/list.txt + 救援日志）。
    CollectDeployArtifacts(logsDir + L"\\collected",
                           target.letter.empty() ? 0 : target.letter[0]);
    ReleaseOpLock();
    // ⚠️ 只有**菜单项模式**不重启；暂存+重启模式必须保留 needReboot=true，
    // 否则 GUI/CLI 走"就地完成"分支：显示"还原完成"且**不自动重启**（用户必须
    // 手动重启，看起来像"中途停止"）。此 bug 自 ≤0.6.4 起存在，0.6.21 修复：
    // 原来这里无条件 `*needReboot = false`，注释却写着"菜单项模式不重启"。
    if (menuEntry && needReboot)
        *needReboot = false;
    ProgressDone("restore", "staged");
    if (menuEntry) {
        SetMenuFlag(true);
        LogInfo("restore menu entry installed (contract written; boots into the "
                "rescue layer only if the user picks it)");
    } else {
        LogInfo("restore staged, reboot to execute");
        // 待执行标记（用户 2026-10-05 规格）：救援层是否真的跑过，用它与
        // ZJRESTORE-status.txt 的时间比较判断 —— 引导失败/开机断电导致"救援
        // **从未执行**"时，下次启动也会明确提示，而不是"什么都没发生"。
        // 只写"暂存+重启"路径；菜单安装不写（常驻菜单不是待执行任务）。
        {
            std::wstring md = LogBaseDir() + L"\\logs";
            CreateDirectoryW(md.c_str(), nullptr);
            SYSTEMTIME st2 = {};
            GetLocalTime(&st2);
            char mk[600];
            snprintf(mk, sizeof(mk),
                     "time=%04u-%02u-%02u %02u:%02u:%02u\ntarget=%s\nimage=%s\n",
                     st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute,
                     st2.wSecond, W2U(target.letter).c_str(),
                     W2U(imagePath).c_str());
            HANDLE hm = CreateFileW(
                (md + L"\\pending-restore.txt").c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hm != INVALID_HANDLE_VALUE) {
                std::string body(mk);
                DWORD w = 0;
                WriteFile(hm, body.data(), (DWORD)body.size(), &w, nullptr);
                CloseHandle(hm);
            }
        }
    }
    rollbackArmed = false;  // 成功：契约保留（菜单绑定 / 暂存任务）
    return 0;
}

// 公开入口：默认 = 暂存+重启（单次启动）；menuEntry = 只装常驻菜单项。
int StageRestore(const RestoreRequest& req, std::string& err, bool* needReboot,
                 ProgressFn progress, ErrAdvice* adv) {
    return StageRestoreImpl(req, err, needReboot, progress, adv,
                            /*menuEntry=*/false);
}

int StageRestoreMenu(const RestoreRequest& req, std::string& err,
                     ProgressFn progress, ErrAdvice* adv) {
    bool needReboot = false;
    return StageRestoreImpl(req, err, &needReboot, progress, adv,
                            /*menuEntry=*/true);
}

// ── 返回旧系统（PIT-139，方案 C）──────────────────────────────────────
// 只读探测：目标根是否同时具备「完整旧系统 ~/.old\Windows」与「完整新系统
// 根 \Windows」。两者都在才谈得上"反向交换"；只有其一说明上次交换中断或
// 已清理过 —— 此时改走「删除旧系统」或不再提供该入口。
bool DetectReturnOldCapability(const PartitionInfo& target, bool* hasOldTree,
                               bool* hasRootTree, std::string& detail) {
    if (hasOldTree) *hasOldTree = false;
    if (hasRootTree) *hasRootTree = false;
    detail.clear();
    if (target.letter.empty()) {
        detail = "no drive letter";
        return false;
    }
    const std::wstring root = target.letter + L":\\";
    auto isDir = [&](const std::wstring& rel) -> bool {
        DWORD a = GetFileAttributesW((root + rel).c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
    };
    const bool oldDir = isDir(L"~old");
    const bool oldWin = isDir(L"~old\\Windows");
    const bool rootWin = isDir(L"Windows");
    if (hasOldTree) *hasOldTree = oldDir && oldWin;
    if (hasRootTree) *hasRootTree = rootWin;
    detail = "~old=" + std::string(oldDir ? "yes" : "no") +
             " ~old\\Windows=" + std::string(oldWin ? "yes" : "no") +
             " root\\Windows=" + std::string(rootWin ? "yes" : "no");
    return oldDir && oldWin && rootWin;
}

int StageReturnOld(const PartitionInfo& target, std::string& err,
                   bool* needReboot) {
    if (needReboot)
        *needReboot = false;

    // 等启动期后台残留清理收尾（与 StageRestore 同款，别在它还没收手时动盘）
    if (!target.letter.empty())
        WaitSwapCleanup(target.letter[0]);

    bool hasOld = false, hasRoot = false;
    std::string det;
    DetectReturnOldCapability(target, &hasOld, &hasRoot, det);
    if (!hasOld) {
        err = std::string(Tr("目标分区上没有可用的旧系统（~old\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (!hasRoot) {
        err = std::string(Tr("目标分区根目录不是完整的当前系统（\\Windows 不存在）—— 无法返回。")) +
              "\n" + det;
        LogError("return-old rejected: " + det);
        return 4;
    }
    if (target.letter.empty()) {
        err = Tr("目标分区无盘符，无法写恢复日志（Linux 侧将无法定位目标）");
        LogError(err);
        return 1;
    }
    if (HasInstalledMenu()) {
        err = Tr("已安装常驻恢复菜单；返回旧系统会覆盖它。请先在主界面点「清除菜单」，再重试。");
        LogError("return-old rejected: persistent menu installed");
        return 4;
    }

    // Q2 暂存路径会话前置（与 StageRestoreImpl 同款，见 bootpath.h）：
    // 返回旧系统 = 重启进 Linux 救援层，依赖重启后固件沿当前同一种方式引导。
    switch (CheckStagedBootPath(IsUefiFirmware(), DiskStyleOf(target.diskIndex))) {
        case StagedBootPath::BiosSessionGptTarget:
            err = Tr("当前会话是传统(CSM)引导，目标是 GPT 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 BIOS 链无法引导 GPT 盘。请改用 UEFI 模式启动后操作。");
            LogError("return-old rejected: BIOS session + GPT target");
            return 4;
        case StagedBootPath::UefiSessionMbrTarget:
            err = Tr("当前会话是 UEFI 引导，目标是 MBR 盘 —— 返回旧系统需要重启进恢复环境，"
                     "但 UEFI 无法执行传统引导链。请改用传统(CSM)模式启动后操作。");
            LogError("return-old rejected: UEFI session + MBR target");
            return 4;
        case StagedBootPath::Ok:
            break;
    }

    std::wstring exeDir = ContractDir();  // Q1 settle 后返回新家；否则 = ExeDir()
    // 软件目录若在目标盘上：先尝试 SettleHome（Q1「先落盘后搬家」，与 StageRestore
    // 同款）——搬成功则 exeDir 指向新家、继续；搬不动才前置拒绝（让用户把程序
    // 拷到别的分区或进 PE 再操作）。
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : 0;
        if (ShouldSettleHome(exeDir, target.letter.empty() ? 0 : target.letter[0])) {
            int exeDisk = VolumeDiskIndex(exLetter);
            int tgtDisk = (int)target.diskIndex;
            SettleResult sr = SettleHome(tgtDisk, exeDisk);
            if (sr.moved) {
                exeDir = ContractDir();  // settle 后 ContractDir 返回新家
                LogInfo("return-old: settled to " + W2U(sr.newHome));
            } else {
                err = Tr("本程序当前运行在目标分区上 —— 返回旧系统会把该分区整树挪走，"
                         "请先把程序复制到**别的分区**（如 D:\\）再执行。");
                LogError("return-old rejected: software dir is on the target partition, "
                         "settle failed: " + sr.detail);
                return 4;
            }
        }
    }

    if (!AcquireOpLock()) {
        err = Tr("已有备份/还原实例在运行");
        return 1;
    }
    ProgressUpdate("restore", 0, "return-old-start");
    LogInfo("return-old staged: target=" + W2U(target.letter) + " " + det);

    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    BcdExport(logsDir + L"\\bcd-backup");

    DiskInfo tdisk;
    for (const auto& d : EnumerateDisks())
        if (d.index == target.diskIndex)
            tdisk = d;

    // 契约：action=returnold + engine=swap（**关键降级**：旧救援层不认识
    // returnold 会落进 engine=swap 分支 → zjswap.sh 查不到 ~new/.zj-done →
    // exit 2 报 "old system untouched"，绝不 mkntfs；fail-closed）。
    // image_* 全空：这条路不需要镜像、不 apply。
    RestoreTask t;
    t.action = "returnold";
    t.engine = "swap";
    t.imagePath.clear();
    t.imageRelPath.clear();
    t.imagePartGuid.clear();
    t.imageIndex = 1;
    t.espIndex = 0;
    t.eaIndex = 0;
    t.targetGuid = target.guid;
    t.targetOffset = target.offsetBytes;
    t.targetSize = target.sizeBytes;
    t.targetDiskSerial = tdisk.serial;
    t.targetDisk = target.diskIndex;
    t.targetPart = (int)target.partNumber;
    t.ptType = tdisk.style == PartitionStyle::GPT ? "gpt" : "mbr";
    t.repairBoot = true;
    t.targetDiskName = tdisk.model;
    t.targetDiskSize = tdisk.sizeBytes;
    t.targetFs = target.fs;
    t.targetVolLabel = target.label;
    t.softwarePath = ExePath();
    {
        wchar_t exLetter = exeDir.size() > 1 ? exeDir[0] : L'\0';
        wchar_t exRoot[4] = {exLetter, L':', L'\\', L'\0'};
        bool fixedDisk = exLetter && GetDriveTypeW(exRoot) == DRIVE_FIXED;
        if (fixedDisk) {
            t.softwareDir = exeDir;
        } else {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            t.softwareDir = dataDrive.empty() ? exeDir : dataDrive + kRecoveryDir;
            CreateDirectoryW(t.softwareDir.c_str(), nullptr);
        }
    }

    // 清场（A2）+ 主/副契约写入
    {
        std::string clog;
        int cleaned = CleanupStrayContracts(clog);
        LogInfo("stray contracts cleanup: " + std::to_string(cleaned) +
                " removed (" + clog.substr(0, clog.size() - 1) + ")");
    }
    std::string tlog;
    if (!WriteRestoreLog(target.letter[0], t, tlog)) {
        ReleaseOpLock();
        ProgressDone("restore", "failed");
        err = Tr("写恢复日志失败（需管理员权限写 ") + W2U(target.letter) + Tr(":\\）");
        LogError(err + " / " + tlog);
        return 1;
    }
    {
        std::string wtlog;
        bool okTask = WriteRestoreTask(exeDir, t, wtlog);
        std::wstring used = exeDir;
        if (!okTask) {
            std::wstring dataDrive = FindDataDrive(target.letter.empty() ? 0 : target.letter[0]);
            std::wstring alt =
                dataDrive.empty() ? std::wstring() : dataDrive + L"ZJRESTORE";
            if (!alt.empty()) {
                CreateDirectoryW(alt.c_str(), nullptr);
                wtlog.clear();
                if (WriteRestoreTask(alt, t, wtlog)) {
                    okTask = true;
                    used = alt;
                }
            }
        }
        if (!okTask)
            LogError("return-old task write failed (non-fatal; the target-root "
                     "log is the primary contract): " + wtlog);
        else
            LogInfo("return-old task written to " + W2U(used));
    }

    // 引导层失败 → 契约回滚（PIT-126 同款：契约已在引导层之前写好，引导层
    // 失败 = 任务永远不会执行 → 必须撤销，否则目标根留一份陈旧契约）。
    bool rollbackArmed = true;
    struct RollbackGuard {
        bool* armed;
        ~RollbackGuard() {
            if (!*armed) return;
            std::string rlog;
            int n = CleanupStrayContracts(rlog);
            DeleteMenuBinding();
            LogWarn("boot layer failed -> staged contracts rolled back (" +
                    std::to_string(n) + " stray removed)");
        }
    } rollbackGuard{&rollbackArmed};

    // 引导层**无条件重装**（PIT-037）：用户可能已经「删除引导项」，而本次
    // 必须能单次启动进救援层。UEFI 下 ESP 常驻项也一并刷新。
    const bool useUefi = UseUefiBootFor(target);
    LogInfo(std::string("boot path: ") + (useUefi ? "UEFI/ESP" : "BIOS/GRUB4DOS") +
            " (firmware=" + (IsUefiFirmware() ? "UEFI" : "BIOS") +
            ", target disk style=" + StyleName(DiskStyleOf(target.diskIndex)) + ")");
    std::string blog;
    if (useUefi) {
        SbMode sbm = CurrentSbMode();
        if (sbm == SbMode::Shim && !IsMokEnrolled(exeDir)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("本机开启了 Secure Boot，但还没注册启动密钥：请先点" "「安装菜单」，重启一次，在蓝底界面选 " "Enroll key -> 选 zj-mok.cer -> 注册，之后回来再操作");
            LogError(err);
            return 1;
        }
        std::wstring espRoot;
        bool guidMatched = false;
        if (!AcquireEspRoot(espRoot, guidMatched, blog,
                             (int)target.diskIndex)) {
            LogError("ESP acquire log (returnold):\n" + blog);
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("UEFI 机器未找到可用的 ESP 引导分区，无法部署引导层。\n"
                     "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了"
                     " Basic Data（应改为 EFI System 类型）。\n当前分区表：\n") +
                  W2U(DescribePartitions());
            LogError(err);
            return 1;
        }
        if (!InstallUefiBootEntry(espRoot, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装 UEFI 引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    } else {
        std::wstring deployDrive = std::wstring(1, target.letter[0]) + L":\\";
        if (!InstallBootLayer(deployDrive, exeDir, blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("安装引导层失败: ") + blog;
            LogError(err);
            return 1;
        }
    }
    // 单次启动（只生效一次；写后读回校验，见 BootWriteBlockedHint）
    if (useUefi && CurrentSbMode() != SbMode::Bootapp) {
        if (!SetUefiBootNext(blog, true)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置固件单次启动失败: ") + blog + BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
    } else {
        std::string slog;
        if (!BcdSetBootsequence(TemporaryGuid(), slog)) {
            ReleaseOpLock();
            ProgressDone("restore", "failed");
            err = Tr("设置单次启动失败（重启后可能进不了还原环境）: ") + slog +
                  BootWriteBlockedHint();
            LogError(err);
            return 1;
        }
        LogInfo("bootsequence verified: " + slog);
    }
    // 返回旧系统也是一次性引导：把清理器注入即将回到根的旧系统 ~old，
    // 首启删除临时条目（常驻菜单已在入口处禁止覆盖）。
    {
        std::string clog;
        if (!InstallFirstBootCleanup(target.letter + L":\\~old", exeDir, clog))
            LogWarn("return-old first-boot cleanup NOT installed: " + clog);
        else
            LogInfo("return-old: " + clog);
    }

    CollectDeployArtifacts(logsDir + L"\\collected", target.letter[0]);
    AppendHistory("return-old-staged", L"", L"", 0,
                  "staged; 实际结果见救援层日志");
    ReleaseOpLock();

    // 待执行标记（PIT-115 同款）：救援层是否真的跑过，用它与 ZJRESTORE-status.txt
    // 的时间比较判断 —— 引导失败/断电导致救援层**从未执行**时，下次启动也会明确提示。
    {
        std::wstring md = LogBaseDir() + L"\\logs";
        CreateDirectoryW(md.c_str(), nullptr);
        SYSTEMTIME st2 = {};
        GetLocalTime(&st2);
        char mk[600];
        snprintf(mk, sizeof(mk),
                 "time=%04u-%02u-%02u %02u:%02u:%02u\ntarget=%s\nimage=%s\n",
                 st2.wYear, st2.wMonth, st2.wDay, st2.wHour, st2.wMinute,
                 st2.wSecond, W2U(target.letter).c_str(),
                 "return-old (~old -> root)");
        HANDLE hm = CreateFileW((md + L"\\pending-restore.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hm != INVALID_HANDLE_VALUE) {
            std::string body(mk);
            DWORD w = 0;
            WriteFile(hm, body.data(), (DWORD)body.size(), &w, nullptr);
            CloseHandle(hm);
        }
    }
    rollbackArmed = false;  // 成功：契约保留（救援层要靠它定位）
    if (needReboot)
        *needReboot = true;  // PIT-110 教训：**不要无条件清零**
    ProgressDone("restore", "staged");
    LogInfo("return-old staged, reboot to execute (action=returnold)");
    return 0;
}

// 便捷入口（PIT-139，给 CLI 用）：按磁盘号/分区号定位目标后走 StageReturnOld。
// disk/part 都 < 0 时自动找「根下有 Windows、且 ~old\Windows 也存在」的第一块
// 固定盘 —— 与 GUI 扫残留时看到的是同一棵树，不会挑错。
int StageReturnOldByDiskPart(int disk, int part, std::string& err,
                             bool* needReboot) {
    PartitionInfo target;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if (disk >= 0 && (int)d.index != disk) continue;
        for (const auto& p : d.parts) {
            if (part >= 0) {
                if ((int)p.partNumber != part) continue;
                target = p;
                found = true;
                continue;
            }
            if (p.letter.size() != 1) continue;
            PartitionInfo cand = p;
            bool hOld = false, hRoot = false;
            std::string det;
            if (DetectReturnOldCapability(cand, &hOld, &hRoot, det) && hOld &&
                hRoot) {
                target = cand;
                found = true;
                break;
            }
        }
        if (found && part < 0) break;
    }
    if (!found) {
        err = std::string(Tr("找不到「返回旧系统」的目标分区：根下没有完整系统、"
                             "~old\\Windows 也不存在。\n")) +
              Tr("可用 `list` 确认后加 --disk N --part M 指定。");
        LogError("return-old: target not found");
        return 4;
    }
    LogInfo("return-old target: disk=" + std::to_string(target.diskIndex) +
            " part=" + std::to_string(target.partNumber) + " letter=" +
            W2U(target.letter) + ":");
    return StageReturnOld(target, err, needReboot);
}

// 读"已安装菜单项"绑定的镜像/目标：<ContractDir()>\restore-task.conf（UTF-8，key=value）。
bool ReadMenuBinding(std::wstring* imagePath, int* imageIndex,
                     unsigned long long* targetOffset,
                     unsigned long long* targetSize) {
    // 只有显式「安装菜单」才写 restore-menu.flag；日常还原的临时契约不能
    // 让 GUI 把「安装菜单」按钮误显示成「删除菜单」。
    if (!HasInstalledMenu())
        return false;
    std::wstring conf = ContractDir() + L"\\restore-task.conf";
    HANDLE h = CreateFileW(conf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::string text;
    {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            text.append(buf, n);
        CloseHandle(h);
    }
    std::string img = TaskConfGet(text, "image_path");
    std::string idx = TaskConfGet(text, "image_index");
    std::string off = TaskConfGet(text, "target_offset");
    std::string sz = TaskConfGet(text, "target_size");
    if (img.empty())
        return false;
    if (imagePath)
        *imagePath = U2W(img);
    if (imageIndex)
        *imageIndex = idx.empty() ? 1 : std::atoi(idx.c_str());
    if (targetOffset)
        *targetOffset = off.empty() ? 0 : strtoull(off.c_str(), nullptr, 10);
    if (targetSize)
        *targetSize = sz.empty() ? 0 : strtoull(sz.c_str(), nullptr, 10);
    return true;
}

// 删除"菜单绑定"副契约（用户 2026-10-05 bug：「删除菜单」后按钮仍是「删除菜单」，
// 因为 ReadMenuBinding 读的 restore-task.conf 没被删）。
void DeleteMenuBinding() {
    SetMenuFlag(false);
    std::wstring dir = ContractDir();
    for (const wchar_t* nm : {L"restore-task.conf", L"restore-task.json"}) {
        std::wstring p = dir + L"\\" + nm;
        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(p.c_str()))
            LogInfo("menu binding removed: " + W2U(p));
    }
    // 顺带把"待执行标记"也清掉（若用户删菜单时还没重启执行）
    std::wstring pend = LogBaseDir() + L"\\logs\\pending-restore.txt";
    SetFileAttributesW(pend.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(pend.c_str());
}

// 一键重试（P3 / PIT-148）：从 <ContractDir()>\restore-task.conf 重建请求并
// StageRestore。仅 A/B 类失败可走此路（调用方已用 RescueStepRetryable 把关）；
// keepOld 恒 false（重试要备返请走正常流程，见 ops.h）。
int RetryStagedRestore(std::string& err, bool* needReboot) {
    err.clear();
    if (needReboot) *needReboot = false;
    // ① 读 conf（key=value；TaskConfGet 已处理注释/转义，见 task.cpp）
    std::wstring confPath = ContractDir() + L"\\restore-task.conf";
    std::string text;
    {
        HANDLE h = CreateFileW(confPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = Tr("重试失败：程序目录没有 restore-task.conf，请重新执行还原");
            LogError("retry: conf missing: " + W2U(confPath));
            return 2;
        }
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
            text.append(buf, n);
        CloseHandle(h);
    }
    // ② 只允许 action=restore（returnold 另有入口；混用会把任务改坏）
    std::string action = TaskConfGet(text, "action");
    if (!action.empty() && action != "restore") {
        err = Tr("重试失败：上次任务不是还原（") + action +
              Tr("），请重新执行「返回旧系统」");
        LogError("retry: non-restore action=" + action);
        return 2;
    }
    std::string img = TaskConfGet(text, "image_path");
    if (img.empty()) {
        err = Tr("重试失败：restore-task.conf 缺 image_path，请重新执行还原");
        LogError("retry: conf missing image_path");
        return 2;
    }
    // ③ 按 offset/size/serial 匹配当前磁盘表（盘符可能变、sdX 字母不可靠）
    std::string offS = TaskConfGet(text, "target_offset");
    std::string szS = TaskConfGet(text, "target_size");
    std::string serialS = TaskConfGet(text, "target_disk_serial");
    unsigned long long off = offS.empty() ? 0 : strtoull(offS.c_str(), nullptr, 10);
    unsigned long long sz = szS.empty() ? 0 : strtoull(szS.c_str(), nullptr, 10);
    if (!off) {
        err = Tr("重试失败：restore-task.conf 缺 target_offset，请重新执行还原");
        LogError("retry: conf missing target_offset");
        return 2;
    }
    PartitionInfo target;
    DiskInfo tdisk;
    bool found = false;
    for (const auto& d : EnumerateDisks()) {
        if (!serialS.empty() && W2U(d.serial) != serialS) continue;
        for (const auto& p : d.parts) {
            if (p.offsetBytes != off) continue;
            if (sz && p.sizeBytes && p.sizeBytes != sz) continue;
            target = p;
            tdisk = d;
            found = true;
            break;
        }
        if (found) break;
    }
    if (!found) {
        err = Tr("重试失败：按契约 offset/size 找不到目标分区（磁盘表已变化？）");
        LogError("retry: target not found offset=" + offS + " size=" + szS +
                 " serial=" + serialS);
        return 4;
    }
    LogInfo("retry: target disk=" + std::to_string(target.diskIndex) +
            " part=" + std::to_string(target.partNumber) + " letter=" +
            W2U(target.letter) + ": offset=" + offS);
    // ④ 重建请求 → StageRestore（安全检查/契约/引导层全流程复用）
    RestoreRequest req;
    req.image = U2W(img);
    req.disk = (int)tdisk.index;
    req.part = (int)target.partNumber;
    req.index = 1;
    std::string idxS = TaskConfGet(text, "image_index");
    if (!idxS.empty()) req.index = std::atoi(idxS.c_str());
    req.repairBoot = TaskConfGet(text, "repair_boot") != "0";
    std::string eng = TaskConfGet(text, "engine");
    req.engine = eng.empty() ? "block" : eng;  // 老 conf 缺键 = B 引擎
    req.keepOld = false;  // 重试要备返请走正常流程（AGENTS 注明边界）
    bool reboot = false;
    std::string serr;
    ErrAdvice adv = ADV_NONE;
    int rc = StageRestore(req, serr, &reboot, nullptr, &adv);
    if (rc == 0) {
        if (needReboot) *needReboot = reboot;
        AppendHistory("restore-retry", req.image,
                      L"disk" + std::to_wstring(req.disk) + L" part" +
                          std::to_wstring(req.part),
                      0, "ok");
        LogInfo("retry: staged ok reboot=" + std::string(reboot ? "1" : "0"));
        return 0;
    }
    AppendHistory("restore-retry", req.image,
                  L"disk" + std::to_wstring(req.disk) + L" part" +
                      std::to_wstring(req.part),
                  0, ("failed rc=" + std::to_string(rc)).c_str());
    err = serr.empty() ? Tr("重试失败（详见日志）") : serr;
    LogError("retry: StageRestore rc=" + std::to_string(rc) + " " + err);
    return rc == 4 ? 4 : 1;
}

// ── 修复引导（docs/15 · P5）──────────────────────────────────────
// 定位系统盘 → 找 ESP（GUID → mountvol → FAT 回退）→ bcdboot → 产物断言。
// 用于：还原后引导坏了、或从 PE 里救一台起不来的机器（不必重装）。
int RepairBoot(int disk, int part, std::string& msg) {
    // ① 目标系统盘
    PartitionInfo target;
    bool found = false;
    auto disks = EnumerateDisks();
    for (const auto& d : disks) {
        for (const auto& p : d.parts) {
            if (p.letter.empty())
                continue;
            if (disk > 0 || part > 0) {
                if ((int)d.index != disk || (int)p.partNumber != part)
                    continue;
            }
            std::wstring win = p.letter + L":\\Windows\\System32\\winload.exe";
            if (GetFileAttributesW(win.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            target = p;
            found = true;
            if (p.isSystem)
                break;  // 优先正在运行的系统盘
        }
        if (found && target.isSystem)
            break;
        if (found && (disk > 0 || part > 0))
            break;
    }
    if (!found) {
        msg = Tr("未找到系统盘（没有分区含 \\Windows\\System32\\winload.exe）。\n"
                 "若指定了 --disk/--part，请确认分区号正确且有盘符。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError("repair-boot: target system partition not found");
        return 1;
    }
    std::wstring winDir = target.letter + L":\\Windows";
    LogInfo("repair-boot: target=" + W2U(target.letter) + ": disk=" +
            std::to_string(target.diskIndex) + " part=" +
            std::to_string(target.partNumber));
    std::string out;
    std::string blog;
    if (!UseUefiBootFor(target)) {
        // BIOS/MBR：优先 bcdboot（可用时最省事），缺失/失败 → 随包资产自足
        //（2026-10-27，用户反馈 2：极简 PE 没有 bcdboot）。
        const std::wstring root = target.letter + L":\\";
        bool done = false;
        if (ToolAvailable(L"bcdboot.exe")) {
            int rc = RunProcess(SysToolPath(L"bcdboot.exe"),
                                winDir + L" /s " + target.letter + L": /f BIOS",
                                out);
            LogInfo("repair-boot bcdboot (BIOS) rc=" + std::to_string(rc) + " / " +
                    out);
            done = rc == 0;
        } else {
            LogInfo("repair-boot: bcdboot.exe missing -> self-contained BIOS boot");
        }
        if (!done) {
            std::string slog, serr;
            const bool sok =
                StageBiosBootTree(root, winDir, AppDir(),
                                  /*overwriteBcd=*/false, slog, serr);
            LogInfo(std::string("repair-boot self-contained BIOS: ") +
                    (sok ? "ok" : "FAILED") + "\n" + slog);
            if (!sok) {
                msg = Tr("修复引导失败：") + serr;
                LogError(msg);
                return 1;
            }
        }
        // 分区引导区 / 磁盘 MBR 代码：运行中的系统盘可能锁不住、安全软件也可能拦写
        //（火绒「写磁盘保留扇区」）。bootself 内部已按「先查 → bootsect → 自足写」，
        // 到这一步还失败 = 确实写不进去 → 如实告诉用户怎么办，而不是假装成功。
        std::string prlog;
        if (EnsureNtfsBootRegion(root, AppDir(), prlog)) {
            LogInfo("repair-boot pbr: " + prlog);
        } else {
            LogWarn("repair-boot pbr (non-fatal): " + prlog);
            MarkBootBlocked();  // GUI 据此弹「退出防护 → 重试」
            msg += Tr("\n注意：分区引导区（PBR）未能写入 —— 可能被安全软件拦写。请临时退出火绒/360 的「写磁盘保留扇区」保护（或把本程序加入信任）后重试，或在 PE 里修复。");
        }
        std::string mrlog;
        if (EnsureDiskMbrCode(target.diskIndex, AppDir(), mrlog))
            LogInfo("repair-boot mbr: " + mrlog);
        else
            LogWarn("repair-boot mbr (non-fatal): " + mrlog);
        msg = Tr("引导已修复（BIOS）。");
        return 0;
    }
    // UEFI：找 ESP（含回退）
    std::wstring espRoot;
    bool guidMatched = false;
    std::string detail;
    if (!AcquireEspRoot(espRoot, guidMatched, blog, (int)target.diskIndex)) {
        msg = Tr("未找到可用的 ESP 引导分区，无法修复引导。\n"
                 "常见原因：DiskGenius 重建分区时漏建 ESP，或把 ESP 标成了 Basic "
                 "Data（应改为 EFI System 类型）。\n当前分区表：\n") +
              W2U(DescribePartitions());
        LogError(msg);
        return 1;
    }
    if (!guidMatched)
        LogWarn("repair-boot: ESP partition type is not EFI System; used fallback");
    // 2026-10-27（用户反馈 2）：bcdboot 缺失/失败 → 随包资产自足（含 GUID 替换与
    // 固件 "Windows Boot Manager" 条目）。模板/空间预检只对 bcdboot 这条路生效。
    bool rebuilt = false;
    if (ToolAvailable(L"bcdboot.exe") && TargetBcdTemplateOk(target.letter[0], detail) &&
        EspHasRoomForBcdboot(espRoot, detail)) {
        int rc = RunProcess(
            SysToolPath(L"bcdboot.exe"),
            winDir + L" /s " + std::wstring(1, espRoot[0]) + L": /f UEFI", out);
        LogInfo("repair-boot bcdboot (UEFI) rc=" + std::to_string(rc) + " / " +
                out);
        rebuilt = rc == 0;
    } else {
        LogInfo("repair-boot: bcdboot unavailable/not applicable -> "
                "self-contained ESP boot (" + detail + ")");
    }
    if (!rebuilt) {
        std::string slog, serr;
        const bool sok = EnsureUefiBootFiles(
            espRoot, std::wstring(1, target.letter[0]) + L":\\", winDir, AppDir(),
            /*overwriteBcd=*/false, slog, serr);
        LogInfo(std::string("repair-boot self-contained ESP: ") +
                (sok ? "ok" : "FAILED") + "\n" + slog);
        std::string wlog;
        if (EnsureWindowsBootEntry(espRoot, wlog))
            LogInfo("repair-boot firmware entry: " + wlog);
        else
            LogWarn("repair-boot firmware entry (non-fatal): " + wlog);
        if (!sok) {
            UnmountEsp(espRoot, blog);
            msg = Tr("修复引导失败：") + serr;
            LogError(msg);
            return 1;
        }
    }
    if (!VerifyEspBcd(espRoot, winDir, detail)) {
        UnmountEsp(espRoot, blog);
        msg = Tr("引导仍不完整（校验未通过）：") + detail;
        LogError(msg);
        return 1;
    }
    // 打印 ESP BCD 的关键行，便于用户/我们确认（displayorder 是不是有值）
    {
        std::string enumOut;
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/store \"" + espRoot + L"EFI\\Microsoft\\Boot\\BCD\" /enum {bootmgr}",
                   enumOut);
        for (size_t pos = 0; pos < enumOut.size();) {
            size_t e = enumOut.find('\n', pos);
            std::string line = enumOut.substr(pos, e == std::string::npos ? e : e - pos);
            if (line.find("displayorder") != std::string::npos)
                LogInfo("repair-boot: " + line);
            if (e == std::string::npos)
                break;
            pos = e + 1;
        }
    }
    UnmountEsp(espRoot, blog);
    msg = Tr("引导已修复并校验通过（UEFI）\n");
    return 0;
}

// ── A 引擎空间预检（用户 2026-10-09 规格，docs/22 §1.1）────────────────
bool EstimateSwapSpace(const PartitionInfo& target,
                       unsigned long long imageContentBytes,
                       SwapSpaceEstimate& out) {
    out = SwapSpaceEstimate{};
    out.content = imageContentBytes;
    if (target.letter.empty())
        return false;
    std::wstring root = target.letter + L":\\";
    auto fsz = [&](const wchar_t* name) -> unsigned long long {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (!GetFileAttributesExW((root + name).c_str(), GetFileExInfoStandard,
                                  &fad))
            return 0;
        ULARGE_INTEGER u;
        u.HighPart = fad.nFileSizeHigh;
        u.LowPart = fad.nFileSizeLow;
        return u.QuadPart;
    };
    out.pagefile = fsz(L"pagefile.sys");
    out.swapfile = fsz(L"swapfile.sys");
    out.hiberfil = fsz(L"hiberfil.sys");
    out.slack = imageContentBytes / 10 + (2ull << 30);  // 内容×10% + 2GB
    out.required =
        out.content + out.pagefile + out.swapfile + out.hiberfil + out.slack;
    ULARGE_INTEGER freeAvail = {}, total = {}, totalFree = {};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeAvail, &total, &totalFree))
        out.freeBytes = freeAvail.QuadPart;
    else
        out.certain = false;
    out.enough = out.freeBytes >= out.required;
    // 不够（或读不到空闲）→ 不确定：上层弹一次让用户确认（用户规格：
    // "够 → 直接开始；不确定的让用户自己确认"）。
    if (!out.enough)
        out.certain = false;
    return true;
}

// 供 GUI 在确认框之前预判 A 引擎空间（用户 2026-10-20 规格）：
// "开始前直接显示 需要 X GB / 可用 Y GB；够就不提示，不够才禁用 A 引擎按钮"。
bool QuerySwapSpace(const std::wstring& imagePath, int index,
                    const PartitionInfo& target, SwapSpaceEstimate& out) {
    out = SwapSpaceEstimate{};
    unsigned long long bytes = 0;
    int rc = ImageContentSize(imagePath, index < 1 ? 1 : index, &bytes);
    if (rc != 0 || bytes == 0)
        return false;  // 读不到大小 → 无法判定（按"够"继续，与原行为一致）
    return EstimateSwapSpace(target, bytes, out);
}

}  // namespace sysrecover
