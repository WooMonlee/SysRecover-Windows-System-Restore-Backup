// 磁盘/分区枚举实现。只用 Win32 API，禁用 WMI（PE 兼容）。
#include "disk.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <vector>

namespace sysrecover {
namespace {

// RAII 句柄
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = INVALID_HANDLE_VALUE; }
    bool valid() const { return h != nullptr && h != INVALID_HANDLE_VALUE; }
    void reset() {
        if (valid()) {
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
};

// 已知 GPT 分区类型 GUID
const GUID kGuidEsp = {0xc12a7328, 0xf81f, 0x11d2,
                       {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};
const GUID kGuidMsr = {0xe3c9e316, 0x0b5c, 0x4db8,
                       {0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae}};
const GUID kGuidRecovery = {0xde94bba4, 0x06d1, 0x4d40,
                            {0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac}};

std::wstring GuidToString(const GUID& g) {
    wchar_t buf[40] = {};
    StringFromGUID2(g, buf, 40);
    std::wstring s = buf;  // "{xxxxxxxx-...}"
    if (s.size() == 38 && s.front() == L'{' && s.back() == L'}')
        s = s.substr(1, 36);
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

wchar_t SystemDriveLetter() {
    wchar_t dir[MAX_PATH] = {};
    if (GetWindowsDirectoryW(dir, MAX_PATH) > 1 && dir[1] == L':')
        return static_cast<wchar_t>(towupper(dir[0]));
    return 0;
}

// 卷记录（用于把盘符/卷标/文件系统映射到磁盘分区）
struct VolumeRec {
    std::vector<std::wstring> letters;  // L"C"（无冒号）
    std::wstring label;
    std::wstring fs;
    struct Extent {
        uint32_t disk = 0;
        uint64_t offset = 0;
    };
    std::vector<Extent> extents;
};

std::vector<VolumeRec> EnumerateVolumes() {
    std::vector<VolumeRec> out;
    wchar_t volName[MAX_PATH] = {};
    Handle finder(FindFirstVolumeW(volName, MAX_PATH));
    if (!finder.valid())
        return out;
    do {
        UINT type = GetDriveTypeW(volName);
        if (type == DRIVE_CDROM || type == DRIVE_REMOTE ||
            type == DRIVE_NO_ROOT_DIR)
            continue;

        VolumeRec rec;
        // 盘符/挂载点
        DWORD need = 0;
        GetVolumePathNamesForVolumeNameW(volName, nullptr, 0, &need);
        if (need > 1) {
            std::vector<wchar_t> paths(need);
            if (GetVolumePathNamesForVolumeNameW(volName, paths.data(), need,
                                                 &need)) {
                for (const wchar_t* p = paths.data(); *p;
                     p += wcslen(p) + 1) {
                    // 只取 "X:\" 形盘符挂载
                    if (p[0] && p[1] == L':' && p[2] == L'\\' && p[3] == 0)
                        rec.letters.emplace_back(1, p[0]);
                }
            }
        }
        // 卷标/文件系统
        wchar_t label[MAX_PATH] = {}, fs[32] = {};
        if (GetVolumeInformationW(volName, label, MAX_PATH, nullptr, nullptr,
                                  nullptr, fs, 32)) {
            rec.label = label;
            rec.fs = fs;
        }
        // 磁盘区段（去掉末尾反斜杠后打开）
        std::wstring dev = volName;
        if (!dev.empty() && dev.back() == L'\\')
            dev.pop_back();
        Handle vh(CreateFileW(dev.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr));
        if (vh.valid()) {
            std::vector<BYTE> buf(sizeof(VOLUME_DISK_EXTENTS) +
                                  7 * sizeof(DISK_EXTENT));
            DWORD ret = 0;
            if (DeviceIoControl(vh.h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                nullptr, 0, buf.data(), (DWORD)buf.size(),
                                &ret, nullptr)) {
                auto* e = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buf.data());
                for (DWORD i = 0; i < e->NumberOfDiskExtents; ++i)
                    rec.extents.push_back(
                        {e->Extents[i].DiskNumber,
                         static_cast<uint64_t>(
                             e->Extents[i].StartingOffset.QuadPart)});
            }
        }
        out.push_back(std::move(rec));
    } while (FindNextVolumeW(finder.h, volName, MAX_PATH));
    return out;
}

bool ReadLayout(HANDLE h, std::vector<BYTE>& buf) {
    buf.assign(8192, 0);
    for (;;) {
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0,
                            buf.data(), (DWORD)buf.size(), &ret, nullptr))
            return true;
        DWORD err = GetLastError();
        if (err != ERROR_INSUFFICIENT_BUFFER && err != ERROR_MORE_DATA)
            return false;
        if (buf.size() >= (1u << 20))
            return false;
        buf.resize(buf.size() * 2);
    }
}

void FillDiskProps(HANDLE h, DiskInfo& d) {
    // 容量
    GET_LENGTH_INFORMATION len = {};
    DWORD ret = 0;
    if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len,
                        sizeof(len), &ret, nullptr))
        d.sizeBytes = len.Length.QuadPart;
    // 型号/序列号/可移动
    BYTE propBuf[1024] = {};
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                        propBuf, sizeof(propBuf), &ret, nullptr)) {
        auto* desc =
            reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(propBuf);
        d.model.clear();
        if (desc->VendorIdOffset) {
            char tmp[128] = {};
            strncpy_s(tmp, reinterpret_cast<char*>(propBuf) +
                               desc->VendorIdOffset,
                      sizeof(tmp) - 1);
            // 去尾空格
            for (int i = (int)strlen(tmp) - 1; i >= 0 && tmp[i] == ' '; --i)
                tmp[i] = 0;
            wchar_t w[128] = {};
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, w, 128);
            d.model = w;
        }
        if (desc->ProductIdOffset) {
            char tmp[128] = {};
            strncpy_s(tmp, reinterpret_cast<char*>(propBuf) +
                               desc->ProductIdOffset,
                      sizeof(tmp) - 1);
            for (int i = (int)strlen(tmp) - 1; i >= 0 && tmp[i] == ' '; --i)
                tmp[i] = 0;
            wchar_t w[128] = {};
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, w, 128);
            if (!d.model.empty())
                d.model += L" ";
            d.model += w;
        }
        if (desc->SerialNumberOffset) {
            char tmp[128] = {};
            strncpy_s(tmp, reinterpret_cast<char*>(propBuf) +
                               desc->SerialNumberOffset,
                      sizeof(tmp) - 1);
            wchar_t w[128] = {};
            MultiByteToWideChar(CP_ACP, 0, tmp, -1, w, 128);
            d.serial = w;
        }
        if (desc->RemovableMedia)
            d.isRemovable = true;
        if (desc->BusType == BusTypeUsb)
            d.isRemovable = true;
    }
}

}  // namespace

const char* StyleName(PartitionStyle s) {
    switch (s) {
        case PartitionStyle::MBR:
            return "MBR";
        case PartitionStyle::GPT:
            return "GPT";
        default:
            return "Unknown";
    }
}

std::vector<DiskInfo> EnumerateDisks() {
    std::vector<DiskInfo> disks;
    const wchar_t sysLetter = SystemDriveLetter();
    const auto volumes = EnumerateVolumes();

    for (uint32_t idx = 0; idx < 16; ++idx) {
        wchar_t path[64] = {};
        swprintf_s(path, L"\\\\.\\PhysicalDrive%u", idx);
        Handle h(CreateFileW(path, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr));
        if (!h.valid())
            continue;

        DiskInfo d;
        d.index = idx;
        FillDiskProps(h.h, d);
        if (d.isRemovable)
            continue;  // 跳过可移动盘（与旧原型一致）

        std::vector<BYTE> buf;
        if (!ReadLayout(h.h, buf))
            continue;
        auto* layout =
            reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
        d.style = layout->PartitionStyle == PARTITION_STYLE_GPT
                      ? PartitionStyle::GPT
                      : (layout->PartitionStyle == PARTITION_STYLE_MBR
                             ? PartitionStyle::MBR
                             : PartitionStyle::Unknown);

        for (DWORD i = 0; i < layout->PartitionCount; ++i) {
            const auto& e = layout->PartitionEntry[i];
            if (e.PartitionLength.QuadPart == 0)
                continue;  // 跳过空槽/容器
            PartitionInfo p;
            p.diskIndex = idx;
            p.partNumber = e.PartitionNumber;
            p.offsetBytes = e.StartingOffset.QuadPart;
            p.sizeBytes = e.PartitionLength.QuadPart;
            if (d.style == PartitionStyle::GPT &&
                e.PartitionStyle == PARTITION_STYLE_GPT) {
                p.guid = GuidToString(e.Gpt.PartitionId);
                if (IsEqualGUID(e.Gpt.PartitionType, kGuidEsp))
                    p.isEsp = true;
                else if (IsEqualGUID(e.Gpt.PartitionType, kGuidMsr))
                    p.isMsr = true;
                else if (IsEqualGUID(e.Gpt.PartitionType, kGuidRecovery))
                    p.isRecovery = true;
            }
            // 卷映射（盘符/卷标/文件系统）
            for (const auto& v : volumes) {
                for (const auto& ex : v.extents) {
                    if (ex.disk == idx && ex.offset == p.offsetBytes) {
                        if (!v.letters.empty())
                            p.letter = v.letters[0];
                        p.label = v.label;
                        p.fs = v.fs;
                        goto matched;
                    }
                }
            }
        matched:
            if (!p.letter.empty()) {
                if (p.letter[0] == sysLetter)
                    p.isSystem = true;
                ULARGE_INTEGER freeAvail = {}, total = {}, free = {};
                std::wstring root = p.letter + L":\\";
                if (GetDiskFreeSpaceExW(root.c_str(), &freeAvail, &total,
                                        &free))
                    p.freeBytes = free.QuadPart;
            }
            d.parts.push_back(std::move(p));
        }
        disks.push_back(std::move(d));
    }
    return disks;
}

bool IsUefiFirmware() {
    // Win8+: GetFirmwareType（Win7 的 headers 没有该枚举，自己定义一个）
    enum { FW_UNKNOWN = 0, FW_BIOS = 1, FW_UEFI = 2 };
    typedef BOOL(WINAPI * GetFirmwareTypeFn)(int*);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        auto fn = (GetFirmwareTypeFn)GetProcAddress(k32, "GetFirmwareType");
        if (fn) {
            int t = FW_UNKNOWN;
            if (fn(&t))
                return t == FW_UEFI;
        }
    }
    // Win7 回退：BIOS 固件下 GetFirmwareEnvironmentVariable 直接返回
    // ERROR_INVALID_FUNCTION；UEFI 下可读（变量不存在也是别的错误码）。
    char buf[16] = {};
    GetFirmwareEnvironmentVariableA(
        "BootCurrent", "{8be4df61-93ca-11d2-aa0d-00e098032b8c}", buf,
        sizeof(buf));
    return GetLastError() != ERROR_INVALID_FUNCTION;
}

bool FindEspPartition(PartitionInfo& out) {
    for (const auto& d : EnumerateDisks())
        for (const auto& p : d.parts)
            if (p.isEsp) {
                out = p;
                return true;
            }
    return false;
}

}  // namespace sysrecover
