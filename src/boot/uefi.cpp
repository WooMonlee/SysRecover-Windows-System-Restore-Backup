// UEFI 固件启动项（Boot####）+ BootOrder —— 从 Windows 直接写 NVRAM。
// 依据：UEFI 规范 §3 Boot Manager（Boot#### 的 OptionalData 作为该镜像的
// LoadOptions 传递）；内核 efi-stub 的 efi_convert_cmdline() 按 efi_char16_t*
// 读取，故命令行必须是 UTF-16LE。Rufus 的 EFICreateNewEntry 为同款实现。
#include "../common/i18n.h"
#include "uefi.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "bcd.h"
#include "bcd_parse.h"
#include "../common/process.h"
#include "../common/version.h"
#include "../common/zip.h"  // Crc32（部署后校验，A6）

// ── MOK/UKI 备选线（PIT-062）──────────────────────────────────────
// 「固件启动项 → shimx64.efi → 我们签名的 UKI」这条路（需要一次性 MOK 注册）
// 已不再是默认：现在默认走 **Canonical 签名链**（PIT-066 变体 D，零注册）。
// 按用户要求：**代码留在仓库里，但默认不编译**，不占用产品体积；日后若
// Canonical 的签名链被收紧，把这里改成 1 重新编译即可启用。
//   相关资产（也在仓库里，不随包分发）：bootfiles/sb/{shimx64.efi, mmx64.efi,
//   fbx64.efi, zj-mok.cer} + 构建脚本 tools/build-uki.py + keys/zj-mok.*
#define ZJ_ENABLE_MOK_PATH 0

namespace sysrecover {
namespace {

// 文件 CRC32 + 部署后校验（A6）：救援文件必须与 bootfiles 源一致 —— 防旧版
// 残留/半截拷贝导致"旧救援配新契约"混用（用户 2026-10-04 要求杜绝）。
bool FileCrc32(const std::wstring& path, uint32_t& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<char> buf;
    char tmp[1 << 20];
    DWORD rd = 0;
    while (ReadFile(h, tmp, sizeof(tmp), &rd, nullptr) && rd > 0)
        buf.insert(buf.end(), tmp, tmp + rd);
    CloseHandle(h);
    out = Crc32(buf.data(), buf.size());
    return true;
}

bool VerifyCopy(const std::wstring& src, const std::wstring& dst,
                std::string& log) {
    uint32_t a = 0, b = 0;
    if (!FileCrc32(src, a) || !FileCrc32(dst, b) || a != b) {
        log += "verify FAIL " + std::string(dst.begin(), dst.end()) + "\n";
        return false;
    }
    return true;
}

// EFI_GLOBAL_VARIABLE 命名空间。
const wchar_t kGlobalGuid[] = L"{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";

// 条目描述 = 身份标识。固件启动菜单字体未必含中文（PIT-054 的方框教训），
// 故用 ASCII；再配合 FilePath 里的 ZJRESTORE 判归属。
const wchar_t kEntryDesc[] = L"SysRecover";

const wchar_t kKernelPath[] = L"\\EFI\\ZJRESTORE\\vmlinuz-zjrestore.efi";

// 内核命令行（= Boot#### 的 OptionalData，UTF-16LE）。zjre=1 是我们自己的标记。
const wchar_t kCmdLine[] =
    L"initrd=\\EFI\\ZJRESTORE\\initramfs-zjrestore.cpio.gz console=tty0 "
    L"nvme_core.io_timeout=1 zjre=1";

#pragma pack(push, 1)
struct EfiDirNode {      // 描述符结束节点
    BYTE type;           // 0x7f
    BYTE subType;        // 0xff
    WORD length;         // 4
};
struct EfiHdNode {       // Media / HardDrive（短格式，固件会自行展开成完整路径）
    BYTE type;           // 4
    BYTE subType;        // 1
    WORD length;         // 42
    DWORD partNumber;
    unsigned long long partStart;  // 起始 LBA
    unsigned long long partSize;   // 扇区数
    GUID signature;                // GPT PartitionId
    BYTE mbrType;        // 2 = GPT
    BYTE sigType;        // 2 = GUID
};
#pragma pack(pop)

typedef BOOL(WINAPI* FnSetFwEx)(LPCWSTR, LPCWSTR, PVOID, DWORD, DWORD);
FnSetFwEx g_setEx = nullptr;

// Win8+ 的 Ex 版可**显式指定变量属性**，我们固定 NON_VOLATILE|BOOTSERVICE|
// RUNTIME(0x7)，保证启动项非易失（重启后仍在）。没有 Ex（Win7）时回退普通版。
void ResolveSetEx() {
    if (g_setEx)
        return;
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (!h)
        h = LoadLibraryW(L"kernel32.dll");
    if (!h)
        return;
    void* p = reinterpret_cast<void*>(
        GetProcAddress(h, "SetFirmwareEnvironmentVariableExW"));
    g_setEx = reinterpret_cast<FnSetFwEx>(p);
}

BOOL SetVarW(LPCWSTR name, LPCWSTR guid, PVOID data, DWORD size) {
    ResolveSetEx();
    SetLastError(0);
    if (g_setEx)
        return g_setEx(name, guid, data, size, 0x7);
    return SetFirmwareEnvironmentVariableW(name, guid, data, size);
}

// 写固件变量需要 SeSystemEnvironmentPrivilege（管理员默认持有但**未启用**）。
bool EnableEnvPrivilege(std::string& log) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        log += "OpenProcessToken failed\n";
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_SYSTEM_ENVIRONMENT_NAME, &luid)) {
        log += "LookupPrivilegeValue(SeSystemEnvironment) failed\n";
        CloseHandle(tok);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(0);
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(tok);
    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        log += "AdjustTokenPrivileges(SeSystemEnvironment) failed err=" +
               std::to_string(err) + "\n";
        return false;
    }
    return true;
}

std::wstring VarName(int num) {
    wchar_t n[10];
    swprintf(n, 10, L"Boot%04X", num);
    return n;
}

bool ReadVar(const std::wstring& name, std::vector<BYTE>& out) {
    DWORD cap = 8192;
    out.resize(cap);
    SetLastError(0);
    DWORD len = GetFirmwareEnvironmentVariableW(name.c_str(), kGlobalGuid, out.data(), cap);
    if (len == 0) {
        out.clear();
        return false;
    }
    out.resize(len);
    return true;
}

// 解析 EFI_LOAD_OPTION：返回描述与 FilePathList（供判归属）。
bool ParseOption(const std::vector<BYTE>& b, std::wstring& desc,
                 std::wstring& devPath) {
    if (b.size() < 8)
        return false;
    WORD fpll = 0;
    memcpy(&fpll, &b[4], 2);
    size_t off = 6;
    std::wstring d;
    while (off + 2 <= b.size()) {
        wchar_t c;
        memcpy(&c, &b[off], 2);
        off += 2;
        if (c == 0)
            break;
        d.push_back(c);
    }
    if (off + fpll > b.size())
        return false;
    // 设备路径里可打印的 ASCII 足够判 ZJRESTORE 归属
    std::wstring p;
    for (size_t i = off; i < off + fpll && i + 1 < b.size(); ++i) {
        wchar_t c = (wchar_t)b[i];
        if (c >= 32 && c < 127)
            p.push_back(c);
    }
    desc = d;
    devPath = p;
    return true;
}

bool IsOurs(const std::vector<BYTE>& b) {
    std::wstring desc, dev;
    if (!ParseOption(b, desc, dev))
        return false;
    return desc == kEntryDesc && dev.find(L"ZJRESTORE") != std::wstring::npos;
}

// 找我们已存在的条目；找到返回条目号，否则 -1。
int FindOurEntry() {
    std::vector<BYTE> order;
    if (!ReadVar(L"BootOrder", order))
        return -1;
    int n = (int)(order.size() / 2);
    for (int i = 0; i < n; ++i) {
        WORD v = 0;
        memcpy(&v, &order[i * 2], 2);
        std::vector<BYTE> b;
        if (!ReadVar(VarName(v), b))
            continue;
        if (IsOurs(b))
            return v;
    }
    return -1;
}

int FindFreeEntry() {
    BYTE tmp[8];
    for (int i = 0; i <= 0xFFFF; ++i) {
        SetLastError(0);
        if (GetFirmwareEnvironmentVariableW(VarName(i).c_str(), kGlobalGuid, tmp, sizeof(tmp)) == 0 &&
            GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return i;
    }
    return -1;
}

// ESP 卷的 GPT 信息（构造短格式 HardDrive 节点用）。
bool QueryEspGpt(const std::wstring& espRoot, GUID& sig, DWORD& partNum,
                 unsigned long long& start, unsigned long long& size,
                 std::string& log) {
    std::wstring dev = L"\\\\.\\" + espRoot.substr(0, 2);
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "open ESP volume failed err=" + std::to_string(GetLastError()) +
               "\n";
        return false;
    }
    PARTITION_INFORMATION_EX pi{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &pi,
                         sizeof(pi), &ret, nullptr)) {
        log += "IOCTL_DISK_GET_PARTITION_INFO_EX failed err=" +
               std::to_string(GetLastError()) + "\n";
        CloseHandle(h);
        return false;
    }
    if (pi.PartitionStyle != PARTITION_STYLE_GPT) {
        log += "ESP is not a GPT partition\n";
        CloseHandle(h);
        return false;
    }
    DWORD bps = 512;
    DISK_GEOMETRY_EX geo{};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo,
                        sizeof(geo), &ret, nullptr) &&
        geo.Geometry.BytesPerSector)
        bps = geo.Geometry.BytesPerSector;
    CloseHandle(h);
    sig = pi.Gpt.PartitionId;
    partNum = pi.PartitionNumber;
    start = pi.StartingOffset.QuadPart / bps;
    size = pi.PartitionLength.QuadPart / bps;
    return true;
}

bool CopyOne(const std::wstring& src, const std::wstring& dst,
             std::string& log) {
    // 目标若已存在且带只读/隐藏属性，先归零，保证能覆盖
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
        return true;
    char s[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, dst.c_str(), -1, s, sizeof(s), nullptr,
                        nullptr);
    log += std::string("copy ") + s + " failed err=" +
           std::to_string(GetLastError()) + "\n";
    return false;
}

// 源文件大小（不存在返回 0），供 ESP 空间检查用。
unsigned long long FileSize(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return (unsigned long long)sz.QuadPart;
}

// 文件/目录是否存在。
bool PathExists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 目录里是否存在匹配 pattern 的文件（如 Fonts\*_boot.ttf）。
bool HasFileMatching(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    return true;
}

// 递归删除文件/目录（先清属性，否则只读/隐藏的删不掉）。
bool RemoveTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            RemoveTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0;
}

}  // namespace

// P6：读固件 db，看它信任哪张微软 UEFI CA（只做字符串扫描 —— 证书的 CN 在 DER 里
// 是明文）。**必须定义在匿名命名空间之外**（要对外链接给 CLI 的 diag 用）。
int FirmwareTrustedUefiCas() {
    std::string dummy;
    EnableEnvPrivilege(dummy);  // 读固件变量需要 SeSystemEnvironmentPrivilege
    std::vector<BYTE> buf(64 * 1024);
    SetLastError(0);
    DWORD len = GetFirmwareEnvironmentVariableW(
        L"db", kGlobalGuid, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0)
        return 0;  // 读不到（非 UEFI / 无权限 / 固件不给）
    std::string s(reinterpret_cast<const char*>(buf.data()), len);
    int mask = 0;
    if (s.find("Microsoft Corporation UEFI CA 2011") != std::string::npos)
        mask |= kFirmwareCa2011;
    if (s.find("Microsoft UEFI CA 2023") != std::string::npos ||
        s.find("Windows UEFI CA 2023") != std::string::npos)
        mask |= kFirmwareCa2023;
    return mask;
}

static bool BuildAndWrite(int num, const GUID& sig, DWORD partNum,
                          unsigned long long start, unsigned long long size,
                          const std::wstring& targetPath, const wchar_t* cmdLine,
                          std::string& log) {
    EfiHdNode hd{};
    hd.type = 4;
    hd.subType = 1;
    hd.length = sizeof(EfiHdNode);
    hd.partNumber = partNum;
    hd.partStart = start;
    hd.partSize = size;
    hd.signature = sig;
    hd.mbrType = 2;
    hd.sigType = 2;

    size_t klen = targetPath.size();
    WORD fpLen = (WORD)(4 + (klen + 1) * 2);
    std::vector<BYTE> fp(fpLen);
    fp[0] = 4;  // Media
    fp[1] = 4;  // FilePath
    memcpy(&fp[2], &fpLen, 2);
    memcpy(&fp[4], targetPath.c_str(), (klen + 1) * 2);

    EfiDirNode end{0x7f, 0xff, 4};

    DWORD fpll =
        (DWORD)(sizeof(EfiHdNode) + fpLen + sizeof(EfiDirNode));
    size_t descBytes = (wcslen(kEntryDesc) + 1) * 2;
    size_t cmdBytes = cmdLine ? (wcslen(cmdLine) + 1) * 2 : 0;

    std::vector<BYTE> buf(6 + descBytes + fpll + cmdBytes);
    DWORD attr = 1;  // LOAD_OPTION_ACTIVE
    memcpy(&buf[0], &attr, 4);
    memcpy(&buf[4], &fpll, 2);
    size_t off = 6;
    memcpy(&buf[off], kEntryDesc, descBytes);
    off += descBytes;
    memcpy(&buf[off], &hd, sizeof(hd));
    off += sizeof(hd);
    memcpy(&buf[off], fp.data(), fpLen);
    off += fpLen;
    memcpy(&buf[off], &end, sizeof(end));
    off += sizeof(end);
    if (cmdBytes)
        memcpy(&buf[off], cmdLine, cmdBytes);

    std::wstring name = VarName(num);
    SetLastError(0);
    if (!SetVarW(name.c_str(), kGlobalGuid, buf.data(), (DWORD)buf.size())) {
        log += "write " + std::string(name.begin(), name.end()) +
               " failed err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 回读校验
    std::vector<BYTE> back;
    if (!ReadVar(name, back) || !IsOurs(back)) {
        log += "verify entry failed\n";
        return false;
    }
    return true;
}

bool InstallUefiBootEntry(const std::wstring& espRoot,
                          const std::wstring& exeDir, std::string& log) {
    if (!EnableEnvPrivilege(log))
        return false;

    // 0) 清掉旧版本残留：BCD 里的 bootapp 恢复条目（会出现在 Windows 启动菜单，
    //    且必然 0xc000007b 失败）+ 指向它的 bootsequence。
    {
        std::string out;
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/delete " + std::wstring(RecoveryGuid()) + L" /f", out);
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/deletevalue {bootmgr} bootsequence", out);
    }

    // 1) 救援文件 → ESP 的 EFI\ZJRESTORE 目录（注意：C++ 注释行尾不能是反斜杠，
    //    否则会行继续、把下一行代码吞掉）
    std::wstring efiDir = espRoot + L"EFI";
    std::wstring dir = efiDir + L"\\ZJRESTORE";
    CreateDirectoryW(efiDir.c_str(), nullptr);
    // 先清掉上次安装留下的文件：不同走法部署的文件集不同（直启内核 / shim 一套 /
    // UKI + efiloader，各自 ~50MB），不清会越装越多、把 ESP 塞满
    // （实测踩到 "ESP space: need 49.3MB, free 31.9MB" → 装不上）。
    RemoveTree(dir);
    CreateDirectoryW(dir.c_str(), nullptr);

    // Secure Boot 开着时，固件只加载**微软签名**的镜像 → 我们的内核不能直接当
    // 启动目标。两种走法（PIT-063 对比实验，ZJ_SB_MODE 切换）：
    //   shim    ：固件启动项 → shimx64.efi（微软签名）→ grubx64.efi(= 我们签名的
    //             UKI)。标准机制，但首次要在 MokManager 注册一次公钥。
    //   bootapp ：BCD bootapp → efiloader.efi → 我们的 UKI。零用户交互，但依赖
    //             `nointegritychecks`（关掉 bootmgr 的完整性校验）。
    SbMode mode = CurrentSbMode();
    std::string mlog = std::string("secure boot mode: ") + SbModeName(mode);
    mlog += "\n";
    log += mlog;

    std::wstring targetPath = kKernelPath;
    const wchar_t* cmdLine = kCmdLine;
    bool useFirmwareEntry = true;

    // 要写进 ESP 的文件（exeDir 相对路径 → ESP 里的文件名）
    std::vector<std::pair<std::wstring, std::wstring>> files;
#if ZJ_ENABLE_MOK_PATH
    if (mode == SbMode::Shim) {
        files = {
            {L"\\bootfiles\\sb\\shimx64.efi", L"shimx64.efi"},
            {L"\\bootfiles\\sb\\mmx64.efi", L"mmx64.efi"},
            {L"\\bootfiles\\sb\\fbx64.efi", L"fbx64.efi"},
            {L"\\bootfiles\\sb\\zjrestore-uki.efi", L"grubx64.efi"},
            {L"\\bootfiles\\sb\\zj-mok.cer", L"zj-mok.cer"},
        };
        // shim 的默认二级就是同目录的 grubx64.efi；命令行在 UKI 里，无需 OptionalData
        targetPath = L"\\EFI\\ZJRESTORE\\shimx64.efi";
        cmdLine = nullptr;
    } else
#endif
    if (mode == SbMode::Grub) {
        // shim（**微软双签：CA2011 + CA2023**）→ Debian 签名的 GRUB（shim 内嵌同一把
        // 证书，**无需 MOK 注册**）→ grub.cfg 用普通 `linux`/`initrd` 加载我们的内核。
        // 2026-09-23 换链（备选 B）：Ubuntu → Debian。原因：Ubuntu 的 shim 目前只带
        // CA2011 单签，**2026 新出厂只信 CA2023 的固件起不来**；Debian 的是双签。
        // 不再需要 mmx64/fbx64 —— 那是 MOK 备选线的资产（本项目 grub 模式用不到）。
        files = {
            {L"\\bootfiles\\sb\\shimx64.efi", L"shimx64.efi"},
            {L"\\bootfiles\\sb\\grubx64.efi", L"grubx64.efi"},
            {L"\\bootfiles\\sb\\grub.cfg", L"grub.cfg"},
            {L"\\bootfiles\\vmlinuz-zjrestore", L"vmlinuz-zjrestore"},
            {L"\\bootfiles\\initramfs-zjrestore.cpio.gz",
             L"initramfs-zjrestore.cpio.gz"},
        };
        targetPath = L"\\EFI\\ZJRESTORE\\shimx64.efi";
        cmdLine = nullptr;
    } else if (mode == SbMode::Bootapp) {
        // bootmgr 加载 efiloader，efiloader 再加载我们的 UKI（命令行/initrd 都在
        // UKI 自己的节里）。不需要固件启动项。
        files = {
            {L"\\bootfiles\\sb\\efiloader.efi", L"efiloader.efi"},
            {L"\\bootfiles\\sb\\zjrestore-uki.efi", L"zjrestore-uki.efi"},
        };
        useFirmwareEntry = false;
    } else {
        files = {
            {L"\\bootfiles\\vmlinuz-zjrestore",
             L"vmlinuz-zjrestore.efi"},
            {L"\\bootfiles\\initramfs-zjrestore.cpio.gz",
             L"initramfs-zjrestore.cpio.gz"},
        };
    }

    // 空间检查：ESP 一般只有 100MB，Windows 自己的引导文件已占 ~40MB，而我们要塞
    // ~52MB（内核+initramfs，或签名版 UKI + shim）。装不下时**提前算清并报错**，
    // 而不是拷到一半失败、留个半残的引导层。
    // 余量 16MB（2026-09-29 由 1MB 提高）：`bcdboot` 刷新该分区上的
    // `EFI\Microsoft\Boot` 目录（bootmgfw 1.5MB + bootmgr.efi 1.5MB + Fonts
    // 4.5MB + Resources/MUI + BCD）实测约 8.6MB；留 16MB 才能保证"我们装完之后
    // bcdboot 还写得进"（docs/15）。
    unsigned long long need = 0;
    for (const auto& f : files)
        need += FileSize(exeDir + f.first);
    ULARGE_INTEGER freeBytes{};
    if (need > 0 && GetDiskFreeSpaceExW(espRoot.c_str(), &freeBytes, nullptr,
                                        nullptr)) {
        const unsigned long long kMargin = 16ull << 20;  // 16MB：给 bcdboot 留位
        char line[192];
        snprintf(line, sizeof(line),
                 "ESP space: need %.1f MB, free %.1f MB\n", need / 1048576.0,
                 freeBytes.QuadPart / 1048576.0);
        log += line;
        if (freeBytes.QuadPart < need + kMargin) {
            snprintf(line, sizeof(line),
                     Tr("ESP 空间不足：需要 %.1fMB，可用 %.1fMB。请缩小 ESP 上的" "其它文件，或换用更小的 initramfs\n"),
                     need / 1048576.0, freeBytes.QuadPart / 1048576.0);
            log += line;
            return false;
        }
    }

    bool copied = true;
    for (const auto& f : files)
        copied &= CopyOne(exeDir + f.first, dir + L"\\" + f.second, log);
    if (!copied) {
        log += "copy UEFI rescue files FAIL\n";
        return false;
    }
    // A6：部署后逐文件 CRC 校验（源 vs ESP 落盘），不一致即失败；并写明文构建戳
    for (const auto& f : files) {
        if (!VerifyCopy(exeDir + f.first, dir + L"\\" + f.second, log)) {
            log += "verify deployed rescue FAIL\n";
            return false;
        }
    }
    {
        std::string vb = std::string(SYSRECOVER_VERSION) + "\n";
        HANDLE hv = CreateFileW((dir + L"\\rescue-build.txt").c_str(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hv != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(hv, vb.data(), (DWORD)vb.size(), &w, nullptr);
            CloseHandle(hv);
        }
    }
    if (mode == SbMode::None)
        SetFileAttributesW((dir + L"\\vmlinuz-zjrestore.efi").c_str(),
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);

    // ── bootapp 走法：BCD bootapp 条目（bootmgr → efiloader → 我们的 UKI）──
    // 不需要固件启动项；单次启动用 BCD `bootsequence`（与 BIOS 路径同法）。
    if (!useFirmwareEntry) {
        std::wstring guid = RecoveryGuid();
        std::string out;
        RunProcess(SysToolPath(L"bcdedit.exe"), L"/delete " + guid + L" /f",
                   out);  // 幂等：先删旧条目
        int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                            L"/create " + guid +
                                L" /d \"SysRecover\" /application bootapp",
                            out);
        log += out;
        if (rc != 0) {
            log += "bcdedit /create bootapp FAILED rc=" + std::to_string(rc) +
                   "\n";
            return false;
        }
        bool ok = true;
        struct {
            const wchar_t* args;
        } const sets[] = {
            {L"device boot"},
            {L"path \\EFI\\ZJRESTORE\\efiloader.efi"},
            {L"nointegritychecks true"},  // 关掉 bootmgr 对 bootapp 的完整性校验
            {L"loadoptions \\EFI\\ZJRESTORE\\zjrestore-uki.efi"},
        };
        for (const auto& s : sets) {
            std::string o2;
            int r2 = RunProcess(SysToolPath(L"bcdedit.exe"),
                                L"/set " + guid + L" " + s.args, o2);
            if (r2 != 0) {
                log += std::string("bcdedit /set ") + " FAILED: " + o2 + "\n";
                ok = false;
            }
        }
        RunProcess(SysToolPath(L"bcdedit.exe"),
                   L"/displayorder " + guid + L" /addlast", out);
        if (!ok)
            return false;
        // 单次启动
        if (RunProcess(SysToolPath(L"bcdedit.exe"), L"/bootsequence " + guid,
                       out) != 0) {
            log += "set bcd bootsequence FAILED: " + out + "\n";
            return false;
        }
        // 清掉可能残留的固件启动项（免得 BootOrder 里躺个走不通的）
        RemoveUefiBootEntry(log);
        log += "bootapp ready: bootmgr -> efiloader -> zjrestore-uki.efi\n";
        return true;
    }

    // 2) 固件启动项
    GUID sig{};
    DWORD partNum = 0;
    unsigned long long start = 0, size = 0;
    if (!QueryEspGpt(espRoot, sig, partNum, start, size, log))
        return false;

    int num = FindOurEntry();
    if (num < 0)
        num = FindFreeEntry();
    if (num < 0) {
        log += "no free Boot#### slot\n";
        return false;
    }
    if (!BuildAndWrite(num, sig, partNum, start, size, targetPath, cmdLine, log))
        return false;

    // 3) 挂进 BootOrder（**末尾**：默认仍进 Windows，固件启动菜单里能选到）
    std::vector<BYTE> order;
    ReadVar(L"BootOrder", order);
    int n = (int)(order.size() / 2);
    std::vector<WORD> list(n);
    bool present = false;
    for (int i = 0; i < n; ++i) {
        memcpy(&list[i], &order[i * 2], 2);
        if (list[i] == (WORD)num)
            present = true;
    }
    if (!present)
        list.push_back((WORD)num);
    std::vector<BYTE> nb(list.size() * 2);
    for (size_t i = 0; i < list.size(); ++i)
        memcpy(&nb[i * 2], &list[i], 2);
    SetLastError(0);
    if (!SetVarW(L"BootOrder", kGlobalGuid, nb.data(), (DWORD)nb.size())) {
        log += "set BootOrder failed err=" + std::to_string(GetLastError()) +
               "\n";
        return false;
    }
    char line[96];
    snprintf(line, sizeof(line), "firmware boot entry Boot%04X installed\n", num);
    log += line;

    // Secure Boot 且密钥**还没注册**时：把下一次启动指到我们的条目，这样重启就
    // 直接进 MokManager 让用户注册（否则普通重启只进 Windows，用户根本看不到
    // 注册界面）。注册完成后 BootNext 已消耗，之后正常进 Windows。
#if ZJ_ENABLE_MOK_PATH
    if (mode == SbMode::Shim && !IsMokEnrolled(exeDir)) {
        WORD v = (WORD)num;
        SetLastError(0);
        if (SetVarW(L"BootNext", kGlobalGuid, &v, sizeof(v)))
            log += "BootNext set -> next boot enters MokManager to enroll "
                   "zj-mok.cer\n";
        else
            log += "set BootNext failed err=" +
                   std::to_string(GetLastError()) + "\n";
    }
#endif
    return true;
}

bool UefiBootEntryExists(std::string& detail) {
    int num = FindOurEntry();
    if (num < 0) {
        detail = Tr("未安装（固件启动项不存在）");
        return false;
    }
    char line[64];
    snprintf(line, sizeof(line), Tr("已安装 Boot%04X"), num);
    detail = line;
    return true;
}

bool RemoveUefiBootEntry(std::string& log) {
    if (!EnableEnvPrivilege(log))
        return false;
    int num = FindOurEntry();
    if (num < 0) {
        log += "no firmware entry to remove\n";
        return true;
    }
    // 1) 从 BootOrder 摘掉
    std::vector<BYTE> order;
    if (ReadVar(L"BootOrder", order)) {
        std::vector<WORD> list;
        for (size_t i = 0; i + 1 < order.size(); i += 2) {
            WORD v = 0;
            memcpy(&v, &order[i], 2);
            if (v != (WORD)num)
                list.push_back(v);
        }
        std::vector<BYTE> nb(list.size() * 2);
        for (size_t i = 0; i < list.size(); ++i)
            memcpy(&nb[i * 2], &list[i], 2);
        SetVarW(L"BootOrder", kGlobalGuid, nb.data(), (DWORD)nb.size());
    }
    // 2) 删 Boot####（SetVariable 长度 0 = 删除）
    std::wstring name = VarName(num);
    SetVarW(name.c_str(), kGlobalGuid, nullptr, 0);
    // 3) BootNext 若指向我们，一并清掉
    WORD next = 0;
    DWORD len = GetFirmwareEnvironmentVariableW(L"BootNext", kGlobalGuid, &next, sizeof(next));
    if (len == sizeof(next) && next == (WORD)num)
        SetVarW(L"BootNext", kGlobalGuid, nullptr, 0);
    log += "firmware boot entry removed\n";
    return true;
}

bool SetUefiBootNext(std::string& log) {
    if (!EnableEnvPrivilege(log))
        return false;
    int num = FindOurEntry();
    if (num < 0) {
        log += "SetUefiBootNext: entry not installed\n";
        return false;
    }
    WORD v = (WORD)num;
    SetLastError(0);
    if (!SetVarW(L"BootNext", kGlobalGuid, &v, sizeof(v))) {
        log += "set BootNext failed err=" + std::to_string(GetLastError()) +
               "\n";
        return false;
    }
    char line[64];
    snprintf(line, sizeof(line), "BootNext = Boot%04X\n", num);
    log += line;
    return true;
}

bool IsSecureBootEnabled() {
    // 固件全局命名空间里的 `SecureBoot`：1 = 开启（User Mode），0 = 关闭。
    BYTE v = 0;
    DWORD len = GetFirmwareEnvironmentVariableW(L"SecureBoot", kGlobalGuid, &v,
                                                sizeof(v));
    return len >= 1 && v != 0;
}

bool IsMokEnrolled(const std::wstring& exeDir) {
    // MOK 有独立命名空间；MokListRT 是给运行时读的副本（含已注册的证书 DER）。
    const wchar_t kMokGuid[] = L"{605DAB50-E046-4300-ABB6-3DDBB810D214}";
    std::vector<BYTE> mok(64 * 1024);
    DWORD len = GetFirmwareEnvironmentVariableW(L"MokListRT", kMokGuid,
                                                mok.data(),
                                                (DWORD)mok.size());
    if (len == 0)
        return false;
    mok.resize(len);
    // 读我们的公钥证书（DER），在 MokListRT 里找这段字节
    HANDLE h = CreateFileW((exeDir + L"\\bootfiles\\sb\\zj-mok.cer").c_str(),
                           GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    std::vector<BYTE> cert(4096);
    DWORD got = 0;
    ReadFile(h, cert.data(), (DWORD)cert.size(), &got, nullptr);
    CloseHandle(h);
    if (got == 0)
        return false;
    cert.resize(got);
    return std::search(mok.begin(), mok.end(), cert.begin(), cert.end()) !=
           mok.end();
}

namespace {
// 读引导走法开关文件 <exeDir>\sb-mode.txt（内容：shim / grub / bootapp）。
// **文件优先于环境变量**：UAC 提权/双击启动时环境变量不一定继承得到
// （实测踩过：设了 ZJ_SB_MODE=grub，日志里却还是 "secure boot mode: shim"）。
std::wstring ReadSbModeFile() {
    wchar_t p[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, p, MAX_PATH))
        return L"";
    std::wstring s = p;
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    std::wstring file = s.substr(0, pos) + L"\\sb-mode.txt";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    char buf[64] = {};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    std::wstring w;
    for (DWORD i = 0; i < got; ++i) {
        char c = buf[i];
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            if (!w.empty())
                break;
            continue;
        }
        w.push_back((wchar_t)tolower((unsigned char)c));
    }
    return w;
}
}  // namespace

const char* SbModeName(SbMode m) {
    switch (m) {
        case SbMode::Shim: return "shim";
        case SbMode::Grub: return "grub";
        case SbMode::Bootapp: return "bootapp";
        default: return "none";
    }
}

SbMode CurrentSbMode() {
    if (!IsSecureBootEnabled())
        return SbMode::None;
    // 模式来源：<exeDir>\sb-mode.txt（优先）→ 环境变量 ZJ_SB_MODE
    std::wstring m = ReadSbModeFile();
    if (m.empty()) {
        wchar_t buf[32] = {};
        if (GetEnvironmentVariableW(L"ZJ_SB_MODE", buf, 32) > 0) {
            for (const wchar_t* q = buf; *q; ++q)
                m.push_back((wchar_t)towlower(*q));
        }
    }
    if (m == L"bootapp")
        return SbMode::Bootapp;  // 实验用：SB 开启时走不通（PIT-065）
#if ZJ_ENABLE_MOK_PATH
    if (m == L"shim")
        return SbMode::Shim;  // 备选线：我们签名的 UKI（需一次性 MOK 注册）
#else
    (void)0;
#endif
    // 默认走 grub：**借 Canonical 的签名链**（微软签名的 shim → Canonical 签名的
    // GRUB → Canonical 签名的内核 + 我们的 initramfs）→ 零注册、零成本、无绕过，
    // 实测 QEMU 全链通过（PIT-066 变体 D）。
    return SbMode::Grub;
}

// ── 引导层健康检查（docs/15 · P2/P3）──────────────────────────────
// 起因：`bcdboot` 的返回码曾被丢弃（静默失败），且没人校验产物；ESP 一旦写残，
// 还原后就是"选择操作系统"空菜单 / 各种 0xc00000xx。这三个函数把"写没写成"
// 变成可判定的事实，并让调用方 fail-closed。
bool TargetBcdTemplateOk(wchar_t targetLetter, std::string& detail) {
    if (!targetLetter) {
        detail = "target letter is empty";
        return false;
    }
    std::wstring tpl = std::wstring(1, targetLetter) +
                       L":\\Windows\\System32\\Config\\BCD-Template";
    if (!PathExists(tpl)) {
        detail = "missing \\Windows\\System32\\Config\\BCD-Template";
        return false;
    }
    if (FileSize(tpl) == 0) {
        detail = "empty \\Windows\\System32\\Config\\BCD-Template";
        return false;
    }
    return true;
}

bool EspHasRoomForBcdboot(const std::wstring& espRoot, std::string& detail) {
    // bcdboot 刷新 \EFI\Microsoft\Boot\ 实测约 8.6MB（bootmgfw + bootmgr.efi +
    // Fonts 4.5MB + Resources + MUI + BCD）；要求 ≥ 24MB 留够余量。
    const unsigned long long kNeed = 24ull << 20;
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(espRoot.c_str(), &freeBytes, nullptr, nullptr)) {
        detail = "cannot query free space on ESP";
        return false;
    }
    char line[160];
    snprintf(line, sizeof(line), "ESP free for bcdboot: %.1f MB (need %.0f MB)",
             freeBytes.QuadPart / 1048576.0, kNeed / 1048576.0);
    detail = line;
    return freeBytes.QuadPart >= kNeed;
}

bool VerifyEspBcd(const std::wstring& espRoot, std::string& detail) {
    // ① BCD 文件在不在
    std::wstring bcd = espRoot + L"EFI\\Microsoft\\Boot\\BCD";
    if (!PathExists(bcd)) {
        detail = Tr("ESP 上没有 \\EFI\\Microsoft\\Boot\\BCD");
        return false;
    }
    // ② bcdedit 能不能读出"有 displayorder + 有 winload 条目"
    std::string out;
    int rc = RunProcess(SysToolPath(L"bcdedit.exe"),
                        L"/store \"" + bcd + L"\" /enum all", out);
    if (rc != 0) {
        detail = Tr("bcdedit 读取 ESP 上的 BCD 失败（rc=") +
                 std::to_string(rc) + Tr("）");
        return false;
    }
    std::string why;
    if (!BcdEnumShowsOsEntry(out, &why)) {
        detail = Tr("ESP 上的 BCD 不完整：") + why;
        return false;
    }
    // ③ bcdboot "写一半"的特征文件集（docs/15 §4.5）
    struct Item {
        const wchar_t* path;   // 相对 ESP 根
        const char* name;      // 报错用（ASCII）
    };
    const Item kItems[] = {
        {L"EFI\\Microsoft\\Boot\\bootmgfw.efi", "bootmgfw.efi"},
        {L"EFI\\Microsoft\\Boot\\Resources\\bootres.dll", "Resources\\bootres.dll"},
        {L"EFI\\Boot\\bootx64.efi", "EFI\\Boot\\bootx64.efi"},
    };
    for (const auto& it : kItems) {
        if (!PathExists(espRoot + it.path)) {
            detail = std::string(Tr("ESP 上缺少引导文件：")) + it.name;
            return false;
        }
    }
    std::wstring bootDir = espRoot + L"EFI\\Microsoft\\Boot";
    if (!HasFileMatching(bootDir + L"\\Fonts", L"*_boot.ttf")) {
        detail = Tr("ESP 上缺少引导字体（Fonts\\*_boot.ttf）");
        return false;
    }
    if (!DirExists(bootDir + L"\\zh-CN") && !DirExists(bootDir + L"\\en-US")) {
        detail = Tr("ESP 上缺少引导语言资源目录（zh-CN / en-US）");
        return false;
    }
    return true;
}

}  // namespace sysrecover