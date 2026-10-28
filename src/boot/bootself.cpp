// 自足引导实现（2026-10-27，用户反馈 2）。见 bootself.h 的说明与资产清单。
#include "bootself.h"

#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <cstring>

#include "../common/process.h"

namespace sysrecover {
namespace {

// UEFI 模板里的两个 16B 分区 GUID 占位（= GUID 的内存布局：Data1/2/3 小端）。
//   ESP 卷：{096c9e09-71fe-401a-8e0e-f0bee1d46018}（模板里出现 5 次）
//   系统卷：{51157e2a-0001-4a11-9e00-5a175a175a17}（模板里出现 10 次）
const unsigned char kEspPlaceholder[16] = {0x09, 0x9e, 0x6c, 0x09, 0xfe, 0x71,
                                           0x1a, 0x40, 0x8e, 0x0e, 0xf0, 0xbe,
                                           0xe1, 0xd4, 0x60, 0x18};
const unsigned char kVolPlaceholder[16] = {0x2a, 0x7e, 0x15, 0x51, 0x01, 0x00,
                                           0x11, 0x4a, 0x9e, 0x00, 0x5a, 0x17,
                                           0x5a, 0x17, 0x5a, 0x17};

// 模板里的占位出现次数（模板被改动时日志能立刻看出来）。
const int kEspHits = 5;
const int kVolHits = 10;

std::string A(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

bool Exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

unsigned long long SizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    ULARGE_INTEGER u{};
    u.HighPart = fad.nFileSizeHigh;
    u.LowPart = fad.nFileSizeLow;
    return u.QuadPart;
}

// 卷设备名：volRoot 形如 "C:\\" → "\\\\.\\C:"
std::wstring VolumeDev(const std::wstring& volRoot) {
    if (volRoot.size() < 2) return {};
    return L"\\\\.\\" + volRoot.substr(0, 2);
}

bool ReadAll(const std::wstring& path, std::vector<unsigned char>& out,
             size_t cap) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart > 0 &&
              (unsigned long long)sz.QuadPart <= cap;
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr) != 0 &&
             rd == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
    return ok && wr == n;
}

bool CopyForce(const std::wstring& src, const std::wstring& dst,
               std::string& log) {
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) return true;
    log += "copy FAIL " + A(src) + " -> " + A(dst) + " (err=" +
           std::to_string(GetLastError()) + ")\n";
    return false;
}

bool EnsureDirTree(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (IsDir(dir)) return true;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos >= 2)
        EnsureDirTree(dir.substr(0, pos));
    CreateDirectoryW(dir.c_str(), nullptr);
    return IsDir(dir);
}

// 递归复制目录（跳过重解析点；失败只记日志，软项不影响结果）。
bool CopyDirTree(const std::wstring& src, const std::wstring& dst,
                 std::string& log, int depth = 0) {
    if (depth > 8 || !IsDir(src)) return false;
    EnsureDirTree(dst);
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            std::wstring s = src + L"\\" + fd.cFileName;
            std::wstring d = dst + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                CopyDirTree(s, d, log, depth + 1);
            else
                CopyForce(s, d, log);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    return true;
}

// 只复制子目录（如 Boot\PCAT\zh-CN → Boot\zh-CN），不复制同层文件。
bool CopyChildDirs(const std::wstring& src, const std::wstring& dst,
                   std::string& log) {
    if (!IsDir(src)) return false;
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 ||
            wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        CopyDirTree(src + L"\\" + fd.cFileName, dst + L"\\" + fd.cFileName, log);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return true;
}

bool CopySoftFile(const std::wstring& src, const std::wstring& dst,
                  std::string& log) {
    if (!Exists(src)) {
        log += "soft: missing " + A(src) + "\n";
        return false;
    }
    EnsureDirTree(dst.substr(0, dst.find_last_of(L'\\')));
    return CopyForce(src, dst, log);
}

// 卷的逻辑扇区大小（4Kn = 4096）。失败返回 0。
DWORD LogicalSectorSize(const std::wstring& volRoot) {
    std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) return 0;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    DWORD bps = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo,
                        sizeof(geo), &ret, nullptr))
        bps = geo.Geometry.BytesPerSector;
    CloseHandle(h);
    return bps;
}

unsigned long long FreeBytesOf(const std::wstring& root) {
    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr))
        return freeBytes.QuadPart;
    return 0;
}

// 目录总字节数（递归，深度 <=6）。SizeOf 对目录返回 0，ESP 空间预检必须用它。
unsigned long long DirSize(const std::wstring& dir, int depth = 0) {
    if (depth > 6 || !IsDir(dir)) return 0;
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE hf = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += DirSize(p, depth + 1);
        else
            total += SizeOf(p);
    } while (FindNextFileW(hf, &fd));
    FindClose(hf);
    return total;
}

}  // namespace

// ── 物理盘直写兜底（2026-10-28，火绒实测）────────────────────────
// 安全软件（火绒「写磁盘保留扇区」、360 等）会拦**卷句柄**对引导扇区的写：
// WriteFile 直接返回 ERROR_ACCESS_DENIED，而 `\\.\PhysicalDriveN` 的直写不受
// 这套保护（实测同一台机器上 MBR 代码写盘成功、卷 VBR 写被拦）。
// 故卷写失败时按「分区起始偏移 + 相对偏移」经物理盘重试一次。
int VolumeDiskNumber(const std::wstring& volRoot) {
    const std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) return -1;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    VOLUME_DISK_EXTENTS ext{};
    DWORD ret = 0;
    int disk = -1;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        &ext, sizeof(ext), &ret, nullptr) &&
        ext.NumberOfDiskExtents >= 1)
        disk = (int)ext.Extents[0].DiskNumber;
    CloseHandle(h);
    return disk;
}

unsigned long long VolumeStartOffset(const std::wstring& volRoot) {
    const std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) return 0;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    PARTITION_INFORMATION_EX pi{};
    DWORD ret = 0;
    unsigned long long off = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &pi,
                        sizeof(pi), &ret, nullptr))
        off = (unsigned long long)pi.StartingOffset.QuadPart;
    CloseHandle(h);
    return off;
}

// 以物理盘为通道读写卷内偏移（off = 卷内相对偏移）。
class PhysVolume {
public:
    ~PhysVolume() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

    bool open(const std::wstring& volRoot, std::string& log) {
        if (h_ != INVALID_HANDLE_VALUE) return true;
        const int disk = VolumeDiskNumber(volRoot);
        base_ = VolumeStartOffset(volRoot);
        if (disk < 0 || base_ == 0) {
            log += "phys: layout unavailable (disk=" + std::to_string(disk) +
                   " offset=" + std::to_string(base_) +
                   "); spanned/unusual volume?\n";
            return false;
        }
        wchar_t dev[64] = {};
        swprintf(dev, 64, L"\\\\.\\PhysicalDrive%d", disk);
        h_ = CreateFileW(dev, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            log += "phys: open PhysicalDrive" + std::to_string(disk) +
                   " failed err=" + std::to_string(GetLastError()) + "\n";
            return false;
        }
        log += "phys: using PhysicalDrive" + std::to_string(disk) +
               " (partition offset " + std::to_string(base_) + ")\n";
        return true;
    }

    bool read(unsigned long long off, void* buf, DWORD n) const {
        if (!valid()) return false;
        OVERLAPPED ov = MakeOv(off);
        DWORD rd = 0;
        return ReadFile(h_, buf, n, &rd, &ov) != 0 && rd == n;
    }

    bool write(unsigned long long off, const void* buf, DWORD n) {
        if (!valid()) return false;
        OVERLAPPED ov = MakeOv(off);
        DWORD wr = 0;
        const bool ok = WriteFile(h_, buf, n, &wr, &ov) != 0 && wr == n;
        if (ok) FlushFileBuffers(h_);
        return ok;
    }

private:
    OVERLAPPED MakeOv(unsigned long long off) const {
        OVERLAPPED ov{};
        const unsigned long long abs = base_ + off;
        ov.Offset = (DWORD)(abs & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(abs >> 32);
        return ov;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    unsigned long long base_ = 0;
};

// 只读读卷内偏移（卷句柄读不受安全软件拦；也不需要锁）。
bool ReadVolRaw(const std::wstring& volRoot, unsigned long long off, void* buf,
                DWORD n) {
    const std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) return false;
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    OVERLAPPED ov{};
    ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD rd = 0;
    const bool ok = ReadFile(h, buf, n, &rd, &ov) != 0 && rd == n;
    CloseHandle(h);
    return ok;
}

// 引导区现状：0x54 是微软代码（fa 33 c0）且**续段有实质内容**（两者是配套的一对，
// 只写其一会黑屏，见 PIT-052/053）。
bool PbrLooksMicrosoft(const std::wstring& volRoot, std::string& detail) {
    unsigned char sec0[512] = {};
    if (!ReadVolRaw(volRoot, 0, sec0, (DWORD)sizeof(sec0))) {
        detail = "read sector 0 failed";
        return false;
    }
    const bool codeOk =
        sec0[0x54] == 0xFA && sec0[0x55] == 0x33 && sec0[0x56] == 0xC0;
    std::vector<unsigned char> cont(4096, 0);
    int nz = 0;
    bool contOk = false;
    if (ReadVolRaw(volRoot, 512, cont.data(), 4096)) {
        for (size_t i = 0; i < 512; ++i)
            if (cont[i]) ++nz;
        contOk = nz >= 64;
    }
    detail = std::string("code=") + (codeOk ? "ok" : "bad") + " cont=" +
             (contOk ? "ok" : "bad") + " (cont_nonzero=" + std::to_string(nz) +
             ")";
    return codeOk && contOk;
}

// 系统自带 bootsect（**微软签名** → 火绒/360 信任它）。2026-10-28 实测：
// 同一台装了火绒的机器上，bootsect 写卷引导代码 **rc=0 成功**，而我们自己的
// 进程写同一扇区（卷句柄、物理盘、LOCK+DISMOUNT 三条路）全部 err=5 被拦。
// 所以：先用系统工具，写不进再上我们自己的实现。
bool RunBootsect(const std::wstring& volRoot, std::string& log) {
    if (volRoot.size() < 2 || volRoot[1] != L':') {
        log += "bootsect: no drive letter, skipped\n";
        return false;
    }
    const std::wstring exe = SysToolPath(L"bootsect.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        log += "bootsect: not available\n";
        return false;
    }
    std::string out;
    const std::wstring args = L"/nt60 " + volRoot.substr(0, 2);
    const int rc = RunProcess(exe, args, out);
    log += "bootsect /nt60 " + std::string(1, (char)volRoot[0]) +
           ": rc=" + std::to_string(rc) + "\n" + out;
    return rc == 0;
}

bool IsNtfsVolume(const std::wstring& volRoot, std::string& detail) {    std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) {
        detail = "bad volume root";
        return false;
    }
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        detail = "open " + A(dev) + " failed err=" +
                 std::to_string(GetLastError());
        return false;
    }
    unsigned char b[512] = {};
    DWORD rd = 0;
    bool ok = ReadFile(h, b, sizeof(b), &rd, nullptr) != 0 && rd == sizeof(b);
    CloseHandle(h);
    if (!ok) {
        detail = "read sector 0 failed";
        return false;
    }
    const bool ntfs = memcmp(b + 3, "NTFS    ", 8) == 0;
    if (ntfs) {
        detail = "OEM=NTFS";
        return true;
    }
    std::string oem;
    for (int i = 3; i < 11; ++i) {
        unsigned char c = b[i];
        oem.push_back(c >= 32 && c < 127 ? (char)c : '.');
    }
    detail = "OEM=" + oem;
    return false;
}

bool EnsureNtfsBootRegion(const std::wstring& volRoot,
                          const std::wstring& exeDir, std::string& log) {
    std::string nd;
    if (!IsNtfsVolume(volRoot, nd)) {
        // 不是 NTFS（FAT/未格式化）→ 不写，避免把 NTFS 引导代码写到别的文件系统上。
        log += "pbr: skip (" + nd + ")\n";
        return true;
    }
    const DWORD bps = LogicalSectorSize(volRoot);
    if (bps > 512) {
        // 4Kn：引导代码的"后续 8 个扇区"是 4096B×8，与随包 512B 扇区布局不符；
        // 且微软不支持 4Kn 盘的 BIOS 引导（PIT-133）→ 跳过并告警，不写坏盘。
        log += "pbr: WARN 4Kn volume (logical sector=" + std::to_string(bps) +
               "), PBR code skipped (BIOS boot unsupported on 4Kn)\n";
        return true;
    }
    std::vector<unsigned char> code, cont;
    if (!ReadAll(exeDir + L"\\bootfiles\\ntfs-boot-code.bin", code, 1 << 20) ||
        code.size() < 426) {
        log += "pbr: asset ntfs-boot-code.bin missing/short (size=" +
               std::to_string(code.size()) + ")\n";
        return false;
    }
    if (!ReadAll(exeDir + L"\\bootfiles\\ntfs-boot-cont.bin", cont, 1 << 20) ||
        cont.size() < 4096) {
        log += "pbr: asset ntfs-boot-cont.bin missing/short (size=" +
               std::to_string(cont.size()) + ")\n";
        return false;
    }

    // ── ① 只读检查：已经是微软引导区（代码 + 续段都在）→ 一个字节都不写 ──
    // 用户 2026-10-28 规格：尽量少动盘、少惊动安全软件。format.com 正常写过后
    // 走的就是这条路（无写入 → 无拦截 → 无提示）。
    std::string st;
    if (PbrLooksMicrosoft(volRoot, st)) {
        log += "pbr: already Microsoft (" + st + "), no write\n";
        return true;
    }
    log += "pbr: needs repair (" + st + ")\n";

    // ── ② 系统自带工具优先：bootsect（微软签名，安全软件信得过）──
    if (RunBootsect(volRoot, log)) {
        if (PbrLooksMicrosoft(volRoot, st)) {
            log += "pbr: ok via bootsect (" + st + ")\n";
            return true;
        }
        log += "pbr: bootsect ran but state still bad (" + st + ")\n";
    }

    // ── ③ 自足实现（bootsect 缺失/失败时才走到这里）──
    std::wstring dev = VolumeDev(volRoot);
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "pbr: open " + A(dev) + " for write failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    // 卷在用时必须先锁；锁不上也继续试（PE / 非系统盘通常能直接写）。
    DWORD ret = 0;
    const bool locked =
        DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr) != 0;
    if (!locked) log += "pbr: lock volume failed (continue) err=" +
                        std::to_string(GetLastError()) + "\n";

    bool w1 = false, w2 = false;
    // ⚠️ 卷句柄裸读写有两条硬要求（2026-10-27 实测）：
    //   ① 偏移必须扇区对齐（直接 seek 0x54 写 426B 会失败）；
    //   ② 偏移要用 **OVERLAPPED** 显式给出（卷句柄不是普通可 seek 文件，
    //      SetFilePointerEx + ReadFile 在这里返回失败且 GetLastError=0）。
    // 因此：读整扇区 → 改 0x54 处 426B → 整扇区写回。
    auto volRead = [&](unsigned long long off, void* buf, DWORD nb) -> bool {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD rd = 0;
        return ReadFile(h, buf, nb, &rd, &ov) != 0 && rd == nb;
    };
    auto volWrite = [&](unsigned long long off, const void* buf, DWORD nb) -> bool {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)(off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD wr = 0;
        return WriteFile(h, buf, nb, &wr, &ov) != 0 && wr == nb;
    };
    unsigned char sec0[512] = {};
    if (!volRead(0, sec0, (DWORD)sizeof(sec0))) {
        log += "pbr: read sector 0 failed\n";
        if (locked)
            DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                            nullptr);
        CloseHandle(h);
        return false;
    }
    // 安全软件（火绒「写磁盘保留扇区」、360 等）会**拦本进程**写引导扇区：
    // 卷句柄、物理盘、甚至 LOCK+DISMOUNT 之后都是 ERROR_ACCESS_DENIED（2026-10-28
    // 实测三条路全拦；只有微软签名的 bootsect 放行）。物理盘仍作为兜底尝试。
    PhysVolume phys;
    bool physUsed = false;

    // 走到这里说明引导区**确有缺陷**：代码段与续段是配套的一对，必须**整对**写，
    // 绝不"只补一半"（只写 426B 会黑屏，PIT-052）。
    {
        unsigned char patched[512];
        memcpy(patched, sec0, sizeof(patched));
        memcpy(patched + 0x54, code.data(), 426);
        SetLastError(0);
        w1 = volWrite(0, patched, (DWORD)sizeof(patched));
        if (!w1) {
            log += "pbr: volume write of boot sector denied err=" +
                   std::to_string(GetLastError()) +
                   " (security software?); trying physical-disk fallback\n";
            w1 = phys.open(volRoot, log) && phys.write(0, patched, 512);
            if (w1) {
                physUsed = true;
                log += "pbr: boot sector written via physical disk\n";
            }
        }
    }
    // 扇区 1..8（引导代码续段）：每次都写，保证 mkntfs/format 留下的占位代码
    // 被覆盖（PIT-053）。
    w2 = volWrite(512, cont.data(), 4096);
    if (!w2) {
        log += "pbr: volume write of boot continuation denied; trying "
               "physical-disk fallback\n";
        w2 = phys.open(volRoot, log) && phys.write(512, cont.data(), 4096);
        if (w2) physUsed = true;
    }
    FlushFileBuffers(h);

    unsigned char back[512] = {};
    bool rok = volRead(0, back, (DWORD)sizeof(back));
    std::vector<unsigned char> backCont(4096, 0);
    bool cok = volRead(512, backCont.data(), 4096) &&
               memcmp(backCont.data(), cont.data(), 4096) == 0;
    // 物理直写后卷缓存可能还是旧值 → 用物理通道复核，避免误报 INCOMPLETE
    if (physUsed && (!rok || !cok)) {
        if (phys.read(0, back, 512)) rok = true;
        if (phys.read(512, backCont.data(), 4096))
            cok = memcmp(backCont.data(), cont.data(), 4096) == 0;
    }
    if (locked)
        DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                        nullptr);
    CloseHandle(h);

    // 验收：0x54 处必须是 fa 33 c0 8e（PIT-053 指纹）+ 续段与随包资产一致。
    const bool ok = w1 && w2 && rok && cok && back[0x54] == 0xFA &&
                    back[0x55] == 0x33 && back[0x56] == 0xC0;
    char line[192];
    snprintf(line, sizeof(line),
             "pbr: %s (code=%d cont=%d cont_verified=%d head=0x%02X%02X%02X)\n",
             ok ? "ok" : "INCOMPLETE", (int)w1, (int)w2, (int)cok, back[0x54],
             back[0x55], back[0x56]);
    log += line;
    if (!ok)
        log += "pbr: BLOCKED by security software (reserved-sector protection). "
               "Ask the user to exit/whitelist it (e.g. Huorong 'protect disk "
               "reserved sectors'), or repair from a PE / use 'restore then "
               "reboot' (Linux side writes the PBR without Windows AV).\n";
    return ok;
}

bool BcdFileLooksUsable(const std::wstring& bcdPath, std::string& detail) {
    std::vector<unsigned char> b;
    if (!ReadAll(bcdPath, b, 4u << 20)) {
        detail = "unreadable";
        return false;
    }
    if (b.size() < 4096) {
        detail = "too small (" + std::to_string(b.size()) + "B)";
        return false;
    }
    static const char kNeedle[] = "w\0i\0n\0l\0o\0a\0d\0";  // UTF-16LE "winload"
    const size_t nlen = sizeof(kNeedle) - 1;
    bool found = false;
    for (size_t i = 0; i + nlen <= b.size(); ++i)
        if (memcmp(b.data() + i, kNeedle, nlen) == 0) {
            found = true;
            break;
        }
    detail = (found ? "ok (" : "no winload entry (") +
             std::to_string(b.size()) + "B)";
    return found;
}

int PatchBcdBytes(std::vector<unsigned char>& buf, const unsigned char from[16],
                  const unsigned char to[16]) {
    if (memcmp(from, to, 16) == 0 || buf.size() < 16) return 0;
    int hits = 0;
    for (size_t i = 0; i + 16 <= buf.size(); ++i) {
        if (memcmp(buf.data() + i, from, 16) == 0) {
            memcpy(buf.data() + i, to, 16);
            ++hits;
            i += 15;
        }
    }
    return hits;
}

bool GetVolumeGuidRaw(const std::wstring& volRoot, unsigned char out[16],
                      std::string& log) {
    std::wstring dev = VolumeDev(volRoot);
    if (dev.empty()) {
        log += "guid: bad volume root\n";
        return false;
    }
    HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "guid: open " + A(dev) + " failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    PARTITION_INFORMATION_EX pi{};
    DWORD ret = 0;
    bool ok = DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0,
                              &pi, sizeof(pi), &ret, nullptr) != 0;
    CloseHandle(h);
    if (!ok) {
        log += "guid: IOCTL_DISK_GET_PARTITION_INFO_EX failed err=" +
               std::to_string(GetLastError()) + "\n";
        return false;
    }
    if (pi.PartitionStyle != PARTITION_STYLE_GPT) {
        log += "guid: not a GPT partition\n";
        return false;
    }
    // GUID 的内存布局就是 BCD 设备元素里的 16 字节（Data1/2/3 小端 + Data4）。
    memcpy(out, &pi.Gpt.PartitionId, 16);
    return true;
}

bool StageBiosBootTree(const std::wstring& destRoot, const std::wstring& winDir,
                       const std::wstring& exeDir, bool overwriteBcd,
                       std::string& log, std::string& err) {
    bool ok = true;

    // ① \bootmgr（镜像通常自带；缺失时从 <win>\Boot\PCAT 或本机补）
    const std::wstring dstBootmgr = destRoot + L"bootmgr";
    if (Exists(dstBootmgr)) {
        log += "bios: bootmgr present (" + std::to_string(SizeOf(dstBootmgr)) +
               "B)\n";
    } else {
        bool got = false;
        const std::wstring cands[] = {winDir + L"\\Boot\\PCAT\\bootmgr"};
        for (const auto& c : cands) {
            if (Exists(c) && CopyForce(c, dstBootmgr, log)) {
                log += "bios: bootmgr <- " + A(c) + "\n";
                got = true;
                break;
            }
        }
        if (!got) {
            // 本机（运行中的 Windows）兜底
            wchar_t wd[MAX_PATH] = {};
            if (GetWindowsDirectoryW(wd, MAX_PATH)) {
                std::wstring host = std::wstring(wd) + L"\\Boot\\PCAT\\bootmgr";
                if (Exists(host) && CopyForce(host, dstBootmgr, log)) {
                    log += "bios: bootmgr <- host " + A(host) + "\n";
                    got = true;
                }
                if (!got) {
                    std::wstring hb = std::wstring(wd, 3) + L"bootmgr";
                    if (Exists(hb) && CopyForce(hb, dstBootmgr, log)) {
                        log += "bios: bootmgr <- host feed " + A(hb) + "\n";
                        got = true;
                    }
                }
            }
        }
        if (!got) {
            log += "bios: bootmgr NOT FOUND\n";
            err = "bootmgr not found (image lacks Windows\\Boot\\PCAT\\bootmgr)";
            ok = false;
        }
    }

    // ② \Boot\BCD（随包模板；已有且可用时按 overwriteBcd 决定是否保留）
    const std::wstring bootDir = destRoot + L"Boot";
    const std::wstring bcdDst = bootDir + L"\\BCD";
    EnsureDirTree(bootDir);
    std::string bd;
    if (!overwriteBcd && Exists(bcdDst) && BcdFileLooksUsable(bcdDst, bd)) {
        log += "bios: keep existing BCD (" + bd + ")\n";
    } else {
        std::vector<unsigned char> tpl;
        const std::wstring tplPath = exeDir + L"\\bootfiles\\bcd\\bios.bcd";
        if (ReadAll(tplPath, tpl, 1 << 20) && tpl.size() >= 4096) {
            if (WriteAll(bcdDst, tpl.data(), tpl.size())) {
                log += "bios: BCD <- template (" + std::to_string(tpl.size()) +
                       "B, device=boot)\n";
            } else {
                log += "bios: write BCD failed err=" +
                       std::to_string(GetLastError()) + "\n";
                err = "write Boot\\BCD failed";
                ok = false;
            }
        } else {
            log += "bios: template missing: " + A(tplPath) + "\n";
            err = "bios.bcd template missing from bootfiles\\bcd";
            ok = false;
        }
    }

    // ③ 软项：字体/资源/语言目录/boot.sdi（缺则 WARN，不致命 —— 精简镜像本来
    //    也可能没有，PIT-126 的"源感知"规则）。目标系统缺时用本机 Windows 兜底
    //    （暂存时目标盘是旧系统，可能比本机还老）。
    wchar_t hostWdBuf[MAX_PATH] = {};
    const std::wstring hostWd =
        GetWindowsDirectoryW(hostWdBuf, MAX_PATH) ? std::wstring(hostWdBuf) : L"";
    auto pick = [&](const wchar_t* rel) -> std::wstring {
        std::wstring a = winDir + rel;
        if (Exists(a)) return a;
        if (!hostWd.empty()) {
            std::wstring b = hostWd + rel;
            if (Exists(b)) return b;
        }
        return {};
    };
    const std::wstring fonts = pick(L"\\Boot\\Fonts");
    if (!fonts.empty())
        CopyDirTree(fonts, bootDir + L"\\Fonts", log);
    else
        log += "bios: soft: no Fonts\n";
    const std::wstring res = pick(L"\\Boot\\Resources");
    if (!res.empty())
        CopyDirTree(res, bootDir + L"\\Resources", log);
    else
        log += "bios: soft: no Resources\n";
    const std::wstring pcat = pick(L"\\Boot\\PCAT");
    if (!pcat.empty())
        CopyChildDirs(pcat, bootDir, log);
    else
        log += "bios: soft: no PCAT (boot menu MUI not copied)\n";
    const std::wstring sdi = pick(L"\\Boot\\DVD\\PCAT\\boot.sdi");
    if (!sdi.empty())
        CopySoftFile(sdi, bootDir + L"\\DVD\\PCAT\\boot.sdi", log);
    else
        log += "bios: soft: no boot.sdi\n";

    log += ok ? "bios: boot tree ready\n" : "bios: boot tree INCOMPLETE\n";
    return ok;
}

bool EnsureBiosBootFiles(const std::wstring& targetRoot,
                         const std::wstring& winDir, const std::wstring& exeDir,
                         bool overwriteBcd, std::string& log, std::string& err) {
    bool ok = StageBiosBootTree(targetRoot, winDir, exeDir, overwriteBcd, log,
                                err);
    // ④ 分区引导区（PBR）：BIOS 引导的最后一步。**非致命**——安全软件
    //    可能拦写（火绒「写磁盘保留扇区」；代码里已自动改走物理盘兜底），
    //    且 format.com 本来就会写微软引导代码；真写不进去时把原因留在日志里。
    if (!EnsureNtfsBootRegion(targetRoot, exeDir, log))
        log += "bios: WARN PBR not fully written (see pbr: lines above)\n";
    log += ok ? "bios: self-contained boot files ready\n"
              : "bios: self-contained boot files INCOMPLETE\n";
    return ok;
}

bool EnsureUefiBootFiles(const std::wstring& espRoot,
                         const std::wstring& targetRoot,
                         const std::wstring& winDir, const std::wstring& exeDir,
                         bool overwriteBcd, std::string& log, std::string& err) {
    bool ok = true;
    const std::wstring msBoot = espRoot + L"EFI\\Microsoft\\Boot";
    const std::wstring fwBoot = espRoot + L"EFI\\BOOT";
    EnsureDirTree(msBoot);
    EnsureDirTree(fwBoot);

    // ① 先把 <win>\Boot\EFI 整棵镜像到 ESP 的 EFI\Microsoft\Boot（等价 bcdboot 的
    //    产物：bootmgfw/bootmgr.efi/memtest/boot.stl/语言 MUI/CIPolicies）。
    //    目标系统缺（精简镜像）时用本机 Windows 兜底。注意顺序：**先镜像、再补
    //    bootmgfw** —— 反过来会被镜像覆盖（同机镜像里的版本才是对的）。
    wchar_t hostWdBuf[MAX_PATH] = {};
    const std::wstring hostWd =
        GetWindowsDirectoryW(hostWdBuf, MAX_PATH) ? std::wstring(hostWdBuf) : L"";
    auto pickDir = [&](const wchar_t* rel) -> std::wstring {
        std::wstring a = winDir + rel;
        if (Exists(a)) return a;
        if (!hostWd.empty()) {
            std::wstring b = hostWd + rel;
            if (Exists(b)) return b;
        }
        return {};
    };
    const std::wstring efiSrc = pickDir(L"\\Boot\\EFI");
    if (!efiSrc.empty())
        CopyDirTree(efiSrc, msBoot, log);
    else
        log += "uefi: WARN Windows\\Boot\\EFI missing (boot menu resources not copied)\n";

    // ② bootmgfw.efi 兜底（镜像里没有时找本机/其它位置）
    const std::wstring dstMgfw = msBoot + L"\\bootmgfw.efi";
    if (Exists(dstMgfw) && SizeOf(dstMgfw) > 0) {
        log += "uefi: bootmgfw.efi present (" +
               std::to_string(SizeOf(dstMgfw)) + "B)\n";
    } else {
        bool got = false;
        std::vector<std::wstring> cands = {winDir + L"\\Boot\\EFI\\bootmgfw.efi",
                                           winDir + L"\\Boot\\EFI\\Boot\\bootmgfw.efi"};
        if (!hostWd.empty())
            cands.push_back(hostWd + L"\\Boot\\EFI\\bootmgfw.efi");
        for (const auto& c : cands) {
            if (Exists(c) && CopyForce(c, dstMgfw, log)) {
                log += "uefi: bootmgfw.efi <- " + A(c) + "\n";
                got = true;
                break;
            }
        }
        if (!got) {
            log += "uefi: bootmgfw.efi NOT FOUND\n";
            err = "bootmgfw.efi not found (image lacks Windows\\Boot\\EFI)";
            ok = false;
        }
    }

    // ③ 字体/资源：ESP 剩余空间紧张时跳过（软项，PIT-126）
    const unsigned long long freeB = FreeBytesOf(espRoot);
    const std::wstring fontsSrc = pickDir(L"\\Boot\\Fonts");
    const std::wstring resSrc = pickDir(L"\\Boot\\Resources");
    const unsigned long long softNeed = DirSize(fontsSrc) + DirSize(resSrc);
    if (freeB && softNeed && freeB < softNeed + (8ull << 20)) {
        log += "uefi: WARN skip Fonts/Resources (free " +
               std::to_string(freeB >> 20) + "MB < need " +
               std::to_string(softNeed >> 20) + "MB + 8MB)\n";
    } else {
        if (!fontsSrc.empty())
            CopyDirTree(fontsSrc, msBoot + L"\\Fonts", log);
        else
            log += "uefi: soft: no Fonts\n";
        if (!resSrc.empty())
            CopyDirTree(resSrc, msBoot + L"\\Resources", log);
        else
            log += "uefi: soft: no Resources\n";
    }

    // ④ 固件兜底路径 \EFI\BOOT\BOOTX64.EFI（NVRAM 无有效项时固件会走这里）
    if (Exists(dstMgfw)) {
        const std::wstring dstX64 = fwBoot + L"\\BOOTX64.EFI";
        if (!Exists(dstX64) || SizeOf(dstX64) != SizeOf(dstMgfw)) {
            if (CopyForce(dstMgfw, dstX64, log))
                log += "uefi: BOOTX64.EFI <- bootmgfw.efi\n";
            else
                log += "uefi: WARN BOOTX64.EFI copy failed\n";
        }
    }

    // ⑤ BCD：模板里两处分区 GUID 占位替换成真实值
    const std::wstring bcdDst = msBoot + L"\\BCD";
    std::string bd;
    if (!overwriteBcd && Exists(bcdDst) && BcdFileLooksUsable(bcdDst, bd)) {
        log += "uefi: keep existing BCD (" + bd + ")\n";
    } else {
        std::vector<unsigned char> tpl;
        const std::wstring tplPath = exeDir + L"\\bootfiles\\bcd\\uefi.bcd";
        if (!ReadAll(tplPath, tpl, 1 << 20) || tpl.size() < 4096) {
            log += "uefi: template missing: " + A(tplPath) + "\n";
            err = "uefi.bcd template missing from bootfiles\\bcd";
            ok = false;
        } else {
            unsigned char espGuid[16] = {}, volGuid[16] = {};
            std::string glog;
            const bool g1 = GetVolumeGuidRaw(espRoot, espGuid, glog);
            const bool g2 = GetVolumeGuidRaw(targetRoot, volGuid, glog);
            log += "uefi: guid query esp=" + std::to_string((int)g1) +
                   " vol=" + std::to_string((int)g2) + "\n" + glog;
            if (!g1 || !g2) {
                log += "uefi: cannot build BCD (partition GUID unavailable)\n";
                err = "cannot resolve GPT partition GUID for ESP/target";
                ok = false;
            } else {
                // 模板占位若恰好等于本机真实 GUID（同机验收/测试盘），替换次数应为 0。
                const bool espSame = memcmp(kEspPlaceholder, espGuid, 16) == 0;
                const bool volSame = memcmp(kVolPlaceholder, volGuid, 16) == 0;
                const int want1 = espSame ? 0 : kEspHits;
                const int want2 = volSame ? 0 : kVolHits;
                const int n1 = PatchBcdBytes(tpl, kEspPlaceholder, espGuid);
                const int n2 = PatchBcdBytes(tpl, kVolPlaceholder, volGuid);
                char line[192];
                snprintf(line, sizeof(line),
                         "uefi: BCD guid patch esp=%d/%d vol=%d/%d%s\n", n1,
                         want1, n2, want2,
                         (espSame || volSame) ? " (template already matches)" : "");
                log += line;
                if (n1 != want1 || n2 != want2)
                    log += "uefi: WARN placeholder count unexpected (template changed?)\n";
                if (WriteAll(bcdDst, tpl.data(), tpl.size()))
                    log += "uefi: BCD <- template (" + std::to_string(tpl.size()) +
                           "B)\n";
                else {
                    log += "uefi: write BCD failed err=" +
                           std::to_string(GetLastError()) + "\n";
                    err = "write ESP BCD failed";
                    ok = false;
                }
            }
        }
    }
    log += ok ? "uefi: self-contained boot files ready\n"
              : "uefi: self-contained boot files INCOMPLETE\n";
    return ok;
}

}  // namespace sysrecover
