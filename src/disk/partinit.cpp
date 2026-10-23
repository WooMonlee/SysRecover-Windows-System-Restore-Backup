// 新硬盘初始化（2026-10-20，用户规格）。见 partinit.h 的原则说明。
#include "common/i18n.h"
#include "common/process.h"
#include "partinit.h"

#include <windows.h>
#include <winioctl.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sysrecover {
namespace {

const unsigned long long k2TB = 2ull * 1024 * 1024 * 1024 * 1024;

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool valid() const { return h != INVALID_HANDLE_VALUE; }
};

std::wstring PhysicalDrive(uint32_t idx) {
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"\\\\.\\PhysicalDrive%u", idx);
    return buf;
}

unsigned long long DiskLength(HANDLE h) {
    GET_LENGTH_INFORMATION len = {};
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len,
                        sizeof(len), &ret, nullptr))
        return len.Length.QuadPart;
    return 0;
}

// 读型号/序列号/是否可移动（与 disk.cpp::FillDiskProps 同源最小实现）。
bool DiskProps(HANDLE h, std::wstring& model, std::wstring& serial,
               bool& removable) {
    BYTE buf[1024] = {};
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf,
                         sizeof(buf), &ret, nullptr))
        return false;
    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);
    auto copyStr = [&](DWORD off) -> std::wstring {
        if (!off) return L"";
        char tmp[128] = {};
        strncpy_s(tmp, reinterpret_cast<char*>(buf) + off, sizeof(tmp) - 1);
        for (int i = (int)strlen(tmp) - 1; i >= 0 && tmp[i] == ' '; --i)
            tmp[i] = 0;
        wchar_t w[128] = {};
        MultiByteToWideChar(CP_ACP, 0, tmp, -1, w, 128);
        return w;
    };
    model = copyStr(desc->VendorIdOffset);
    std::wstring prod = copyStr(desc->ProductIdOffset);
    if (!prod.empty()) model += (model.empty() ? L"" : L" ") + prod;
    serial = copyStr(desc->SerialNumberOffset);
    removable = desc->RemovableMedia || desc->BusType == BusTypeUsb;
    return true;
}

// 读 LBA0（4096 字节，兼容 4Kn），判断"是否没有任何分区"。
// 返回 true = 可初始化；*initialized/*style 填结果。
bool ProbeEmpty(uint32_t idx, EmptyDiskInfo& out) {
    Handle h;
    h.h = CreateFileW(PhysicalDrive(idx).c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      0, nullptr);
    if (!h.valid())
        return false;
    std::wstring model, serial;
    bool removable = false;
    DiskProps(h.h, model, serial, removable);
    if (removable) return false;
    out.index = idx;
    out.model = model;
    out.serial = serial;
    out.sizeBytes = DiskLength(h.h);
    if (out.sizeBytes == 0) return false;

    std::vector<unsigned char> buf(4096, 0);
    DWORD rd = 0;
    SetFilePointer(h.h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h.h, buf.data(), (DWORD)buf.size(), &rd, nullptr) || rd < 512)
        return false;
    const bool sig = buf[510] == 0x55 && buf[511] == 0xAA;
    if (!sig) {
        out.initialized = false;
        out.style = PartitionStyle::Unknown;
        return true;  // RAW
    }
    bool any = false, protective = false;
    for (int i = 0; i < 4; ++i) {
        const unsigned char* e = buf.data() + 446 + i * 16;
        const unsigned char type = e[4];
        DWORD sectors = (DWORD)e[12] | ((DWORD)e[13] << 8) |
                        ((DWORD)e[14] << 16) | ((DWORD)e[15] << 24);
        if (type != 0 && sectors != 0) {
            any = true;
            if (type == 0xEE) protective = true;
        }
    }
    if (any) return false;  // 有分区（或保护性 GPT）→ 不自动提供
    out.initialized = true;
    out.style = PartitionStyle::MBR;
    return true;
}

bool LetterUsed(wchar_t c) {
    DWORD mask = GetLogicalDrives();
    return (mask & (1u << (c - L'A'))) != 0;
}

// 从 D 起找空闲盘符（避开 PE 可能已占用的 A/B/C）。
wchar_t PickLetter(wchar_t from, wchar_t* used, int usedN) {
    for (wchar_t c = from; c <= L'Z'; ++c) {
        if (LetterUsed(c)) continue;
        bool dup = false;
        for (int i = 0; i < usedN; ++i)
            if (used[i] == c) dup = true;
        if (!dup) return c;
    }
    return 0;
}

bool WriteAsciiFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == text.size();
}

bool HasBootCode(uint32_t idx) {
    Handle h;
    h.h = CreateFileW(PhysicalDrive(idx).c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                      0, nullptr);
    if (!h.valid()) return false;
    unsigned char buf[512] = {};
    DWORD rd = 0;
    SetFilePointer(h.h, 0, nullptr, FILE_BEGIN);
    if (!ReadFile(h.h, buf, sizeof(buf), &rd, nullptr) || rd < 512) return false;
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;
    bool code = false, active = false;
    for (int i = 0; i < 440; ++i)
        if (buf[i]) code = true;
    for (int i = 0; i < 4; ++i)
        if (buf[446 + i * 16] == 0x80) active = true;
    return code && active;
}

}  // namespace

std::vector<EmptyDiskInfo> FindEmptyDisks() {
    std::vector<EmptyDiskInfo> out;
    const auto disks = EnumerateDisks();
    for (const auto& d : disks) {
        if (!d.parts.empty()) continue;
        EmptyDiskInfo e;
        e.index = d.index;
        e.model = d.model;
        e.serial = d.serial;
        e.sizeBytes = d.sizeBytes;
        e.initialized = d.style != PartitionStyle::Unknown;
        e.style = d.style;
        if (e.sizeBytes) out.push_back(e);
    }
    // EnumerateDisks 对 RAW 盘（读不到布局）会整体跳过 —— 这类盘单独探测。
    for (uint32_t idx = 0; idx < 16; ++idx) {
        bool known = false;
        for (const auto& d : disks)
            if (d.index == idx) known = true;
        if (known) continue;
        EmptyDiskInfo e;
        if (ProbeEmpty(idx, e)) out.push_back(e);
    }
    return out;
}

void RankPartSchemes(bool uefiSession, bool uefiCapable, bool win7Family,
                     uint64_t diskSize, DiskScheme& first, DiskScheme& second,
                     bool& mbrAllowed, bool& gptAllowed) {
    gptAllowed = !win7Family;                       // Win7 x86/XP 不能 UEFI
    mbrAllowed = diskSize <= k2TB;                  // MBR 单分区上限 2TB
    bool preferGpt = false;
    if (!mbrAllowed) preferGpt = true;              // 只能 GPT
    else if (win7Family) preferGpt = false;         // 只能 MBR
    else preferGpt = uefiSession || uefiCapable;    // 现代镜像/固件优先 GPT
    if (!gptAllowed) preferGpt = false;
    if (!mbrAllowed) preferGpt = true;
    first = preferGpt ? DiskScheme::Gpt : DiskScheme::Mbr;
    second = preferGpt ? DiskScheme::Mbr : DiskScheme::Gpt;
}

PartSizeHint SuggestPartitionSizes(bool win7Family, DiskScheme scheme,
                                   uint64_t diskSize) {
    PartSizeHint h;
    const unsigned long long diskGiB = diskSize / (1024ull * 1024 * 1024);
    unsigned long long overhead = 1;  // NTFS/对齐余量
    if (scheme == DiskScheme::Gpt) overhead += 1;  // ESP+MSR 向上取整 1GB
    h.usableGb = diskGiB > overhead ? diskGiB - overhead : 0;
    const unsigned long long fit = h.usableGb;
    if (win7Family) {
        if (fit >= 65) {
            h.systemGb = 60;
            h.createData = true;
        } else if (fit >= 35) {
            h.systemGb = 30;
            h.createData = true;
        } else {
            h.systemGb = fit;
            h.createData = false;
        }
    } else {
        if (fit >= 105) {
            h.systemGb = 100;
            h.createData = true;
        } else if (fit >= 65) {
            h.systemGb = 60;
            h.createData = true;
        } else {
            h.systemGb = fit;
            h.createData = false;
        }
    }
    if (h.systemGb < 20) {
        h.systemGb = fit;
        h.createData = false;
    }
    return h;
}

bool ValidatePartInitPlan(const PartInitPlan& p, uint64_t diskSize,
                          std::wstring& err) {
    if (diskSize == 0) {
        err = Tr(L"磁盘容量读取失败，无法创建分区");
        return false;
    }
    if (p.scheme == DiskScheme::Mbr && diskSize > k2TB) {
        err = Tr(L"MBR 分区表最大支持 2TB 单分区；该磁盘超过 2TB，请改用 GPT（UEFI）方案。");
        return false;
    }
    if (p.scheme == DiskScheme::Gpt && p.win7Family) {
        err = Tr(L"Win7 x86 / XP 类系统不支持 UEFI 启动，只能使用 MBR 方案。");
        return false;
    }
    if (p.systemGb < 20) {
        err = Tr(L"系统盘容量太小（至少 20GB）");
        return false;
    }
    const unsigned long long sysMb = p.systemGb * 1024ull;
    const unsigned long long totalMb = diskSize >> 20;
    // 建数据盘时至少留 1GB；用户规格（60GB 小盘）允许"整盘只做系统盘"。
    unsigned long long need = sysMb + (p.createData ? 1024 : 0);
    if (p.scheme == DiskScheme::Gpt)
        need += p.espMb + p.msrMb;
    if (need > totalMb) {
        err = Tr(L"磁盘容量不足以容纳系统盘与数据盘，请调小系统盘容量");
        return false;
    }
    if (p.scheme == DiskScheme::Gpt) {
        if (p.espMb < 100 || p.msrMb < 16) {
            err = Tr(L"ESP/MSR 容量不合法");
            return false;
        }
    }
    return true;
}

std::string BuildDiskpartScript(const PartInitPlan& p, wchar_t espLetter,
                                wchar_t sysLetter, wchar_t dataLetter,
                                bool& needBootsect) {
    char buf[2048];
    std::string s;
    auto add = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        s += buf;
    };
    add("select disk %u\n", p.diskIndex);
    add("clean\n");
    if (p.scheme == DiskScheme::Gpt) {
        add("convert gpt\n");
        add("create partition efi size=%llu\n", p.espMb);
        add("format quick fs=fat32 label=ESP\n");
        if (espLetter) add("assign letter=%c\n", (char)espLetter);
        add("create partition msr size=%llu\n", p.msrMb);
        add("create partition primary size=%llu\n", p.systemGb * 1024ull);
        add("format quick fs=ntfs label=Windows\n");
        if (sysLetter) add("assign letter=%c\n", (char)sysLetter);
        if (p.createData) {
            add("create partition primary\n");
            add("format quick fs=ntfs label=Data\n");
            if (dataLetter) add("assign letter=%c\n", (char)dataLetter);
        }
        needBootsect = false;
    } else {
        add("convert mbr\n");
        add("create partition primary size=%llu\n", p.systemGb * 1024ull);
        add("format quick fs=ntfs label=Windows\n");
        add("active\n");
        if (sysLetter) add("assign letter=%c\n", (char)sysLetter);
        if (p.createData) {
            add("create partition primary\n");
            add("format quick fs=ntfs label=Data\n");
            if (dataLetter) add("assign letter=%c\n", (char)dataLetter);
        }
        needBootsect = true;
    }
    add("exit\n");
    return s;
}

int ApplyPartInitPlan(const PartInitPlan& p, std::wstring& err,
                      std::string& log) {
    wchar_t letters[3] = {0, 0, 0};
    letters[0] = PickLetter(L'D', letters, 0);
    letters[1] = PickLetter(L'D', letters, 1);
    letters[2] = PickLetter(L'D', letters, 2);
    if (!letters[0] || !letters[1] || !letters[2]) {
        err = Tr(L"没有足够的空闲盘符可分配");
        return 1;
    }
    bool needBootsect = false;
    const std::string script = BuildDiskpartScript(
        p, letters[0], letters[1], letters[2], needBootsect);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring sp = std::wstring(tmp) + L"zjpartinit.txt";
    if (!WriteAsciiFile(sp, script)) {
        err = Tr(L"无法写入分区脚本（临时目录不可写）");
        return 1;
    }
    log += "partinit: diskpart script:\n" + script;
    std::string out;
    int rc = RunProcess(SysToolPath(L"diskpart.exe"),
                        L"/s \"" + sp + L"\"", out);
    DeleteFileW(sp.c_str());
    log += "partinit: diskpart rc=" + std::to_string(rc) + "\n" + out;
    if (rc != 0) {
        err = Tr(L"分区失败（diskpart 返回错误）。请打开日志查看详情。");
        return 1;
    }
    if (needBootsect) {
        // MBR 新盘必须写磁盘引导代码 + 活动分区，否则还原完黑屏。
        // ⚠️ bootsect 语法必须带 /nt60（2026-10-21 VM 实测踩到）：
        //   `bootsect /nt60 <盘符>: /mbr` —— 少了 /nt60 会被当成用法错误
        //   （返回非 0），以前误报成"bootsect 不可用"。
        const std::wstring bsexe = SysToolPath(L"bootsect.exe");
        if (GetFileAttributesW(bsexe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = Tr(L"PE 中缺少 bootsect.exe，无法写入 MBR 引导代码。"
                     L"请改用 GPT 方案，或在 PE 中手动运行："
                     L"bootsect /nt60 <系统盘>: /mbr");
            return 1;
        }
        std::string bsd;
        std::wstring args =
            L"/nt60 " + std::wstring(1, letters[1]) + L": /mbr";
        int brc = RunProcess(bsexe, args, bsd);
        log += "partinit: bootsect /nt60 " +
               std::string(1, (char)letters[1]) + ": /mbr rc=" +
               std::to_string(brc) + "\n" + bsd;
        if (brc != 0) {
            err = Tr(L"写入 MBR 引导代码失败（bootsect 返回错误）。"
                     L"请查看日志，或手动运行："
                     L"bootsect /nt60 <系统盘>: /mbr");
            return 1;
        }
    }
    // 回读校验：分区风格/数量/ESP/引导代码。
    const auto disks = EnumerateDisks();
    const DiskInfo* di = nullptr;
    for (const auto& d : disks)
        if (d.index == p.diskIndex) di = &d;
    if (!di) {
        err = Tr(L"分区后无法回读磁盘信息");
        return 1;
    }
    const PartitionStyle want = p.scheme == DiskScheme::Gpt
                                    ? PartitionStyle::GPT
                                    : PartitionStyle::MBR;
    if (di->style != want) {
        err = Tr(L"分区表类型校验失败");
        return 1;
    }
    int usable = 0;
    bool esp = false;
    for (const auto& q : di->parts) {
        if (q.isEsp) esp = true;
        if (!q.isEsp && !q.isMsr && !q.isRecovery) ++usable;
    }
    if (usable < (p.createData ? 2 : 1)) {
        err = Tr(L"分区数量校验失败（应至少有系统盘与数据盘）");
        return 1;
    }
    if (p.scheme == DiskScheme::Gpt && !esp) {
        err = Tr(L"GPT 方案缺少 ESP 分区，校验失败");
        return 1;
    }
    if (p.scheme == DiskScheme::Mbr && !HasBootCode(p.diskIndex)) {
        err = Tr(L"MBR 引导代码/活动分区校验失败");
        return 1;
    }
    log += "partinit: verify ok (style=" +
           std::string(StyleName(di->style)) + " parts=" +
           std::to_string(di->parts.size()) + ")\n";
    return 0;
}

}  // namespace sysrecover
