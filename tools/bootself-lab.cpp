// 自足引导的离线实验工具（lab-only，不进产品；同 tools/reparse-lab.cpp 的定位）。
// 用途：对**已挂载的卷**跑 bootself 的真实实现，验证文件落点 / BCD 模板 GUID 替换，
// 再用 bcdedit /enum 或 QEMU 启动复核。2026-10-27（用户反馈 2）。
//
// 用法：
//   bootself-lab.exe pbr  <卷根，如 Y:\>
//   bootself-lab.exe bios <目标根，如 T:\>
//   bootself-lab.exe uefi <ESP 根，如 Y:\> <系统卷根，如 T:\>
//   bootself-lab.exe mbr|zerombr|dump <磁盘号>
//   bootself-lab.exe zero|zerophys <卷根>     （造"PBR 被写坏"的靶子）
//
// 编译（bootself 现在会调 bootsect，需要 process/logger；输出到 dist\ 才有 bootfiles 资产）：
//   g++ -O2 -std=c++17 -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE -Isrc \
//       tools/bootself-lab.cpp src/boot/bootself.cpp src/disk/mbrcode.cpp \
//       src/common/process.cpp src/common/logger.cpp src/common/selfarch.cpp \
//       -o dist/bootself-lab.exe -static -mconsole
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <winioctl.h>

#include "boot/bootself.h"
#include "common/selfarch.h"
#include "disk/mbrcode.h"

using namespace sysrecover;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: bootself-lab pbr|bios|uefi <root> [target]\n");
        return 2;
    }
    const std::string mode = argv[1];
    auto widen = [](const char* s) {
        wchar_t buf[1024] = {};
        MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 1024);
        return std::wstring(buf);
    };
    const std::wstring a = widen(argv[2]);
    std::string log, err;
    int rc = 1;
    if (mode == "dump") {
        const unsigned disk = (unsigned)atoi(argv[2]);
        wchar_t dev[64] = {};
        swprintf(dev, 64, L"\\\\.\\PhysicalDrive%u", disk);
        HANDLE h = CreateFileW(dev, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("open failed err=%lu\n", GetLastError());
            return 1;
        }
        unsigned char b[512] = {};
        OVERLAPPED ov{};
        DWORD rn = 0;
        bool rd = ReadFile(h, b, 512, &rn, &ov) && rn == 512;
        CloseHandle(h);
        std::printf("dump read=%d head=", (int)rd);
        for (int i = 0; i < 8; ++i) std::printf("%02X ", b[i]);
        std::printf("pbr54=");
        for (int i = 0x54; i < 0x5C; ++i) std::printf("%02X ", b[i]);
        std::printf("sig=%02X%02X\n", b[510], b[511]);
        return rd ? 0 : 1;
    }
    if (mode == "zerophys") {
        // 经物理盘把分区引导区代码清零（绕开火绒的「写磁盘保留扇区」卷保护），
        // 用来造"PBR 被写坏"的靶子，验证 EnsureNtfsBootRegion 的物理盘兜底。
        const std::wstring dev = L"\\\\.\\" + a.substr(0, 2);
        HANDLE hv = CreateFileW(dev.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (hv == INVALID_HANDLE_VALUE) {
            std::printf("open %ls failed err=%lu\n", dev.c_str(), GetLastError());
            return 1;
        }
        VOLUME_DISK_EXTENTS ext{};
        DWORD ret = 0;
        int disk = -1;
        if (DeviceIoControl(hv, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                            &ext, sizeof(ext), &ret, nullptr) &&
            ext.NumberOfDiskExtents >= 1)
            disk = (int)ext.Extents[0].DiskNumber;
        PARTITION_INFORMATION_EX pi{};
        unsigned long long base = 0;
        if (DeviceIoControl(hv, IOCTL_DISK_GET_PARTITION_INFO_EX, nullptr, 0, &pi,
                            sizeof(pi), &ret, nullptr))
            base = (unsigned long long)pi.StartingOffset.QuadPart;
        CloseHandle(hv);
        if (disk < 0 || base == 0) {
            std::printf("disk/offset unavailable (disk=%d base=%llu)\n", disk,
                        base);
            return 1;
        }
        wchar_t pd[64] = {};
        swprintf(pd, 64, L"\\\\.\\PhysicalDrive%d", disk);
        HANDLE h = CreateFileW(pd, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("open %ls failed err=%lu\n", pd, GetLastError());
            return 1;
        }
        auto ovAt = [&](unsigned long long off) {
            OVERLAPPED ov{};
            const unsigned long long abs = base + off;
            ov.Offset = (DWORD)(abs & 0xFFFFFFFFull);
            ov.OffsetHigh = (DWORD)(abs >> 32);
            return ov;
        };
        unsigned char sec0[512] = {};
        OVERLAPPED ov0 = ovAt(0);
        DWORD rd = 0;
        bool rok = ReadFile(h, sec0, 512, &rd, &ov0) && rd == 512;
        memset(sec0 + 0x54, 0, 426);
        OVERLAPPED ov1 = ovAt(0);
        DWORD wr = 0;
        SetLastError(0);
        bool w1 = rok && WriteFile(h, sec0, 512, &wr, &ov1) && wr == 512;
        const DWORD e1 = GetLastError();
        std::printf("zerophys: plain phys write w1=%d(e=%lu)\n", (int)w1,
                    (unsigned long)e1);
        if (!w1) {
            // 试：先把卷 LOCK + DISMOUNT，再经物理盘写（部分安全软件/存储栈
            // 只保护"已挂载卷"的元数据区）。
            HANDLE hv2 = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
            bool lk = false, dm = false;
            if (hv2 != INVALID_HANDLE_VALUE) {
                DWORD r2 = 0;
                lk = DeviceIoControl(hv2, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr,
                                     0, &r2, nullptr) != 0;
                dm = DeviceIoControl(hv2, FSCTL_DISMOUNT_VOLUME, nullptr, 0,
                                     nullptr, 0, &r2, nullptr) != 0;
                CloseHandle(hv2);
            }
            OVERLAPPED ov3 = ovAt(0);
            SetLastError(0);
            w1 = rok && WriteFile(h, sec0, 512, &wr, &ov3) && wr == 512;
            std::printf("zerophys: after lock=%d dismount=%d -> w1=%d(e=%lu)\n",
                        (int)lk, (int)dm, (int)w1, (unsigned long)GetLastError());
        }
        std::vector<unsigned char> zeros(4096, 0);
        OVERLAPPED ov2 = ovAt(512);
        SetLastError(0);
        bool w2 = WriteFile(h, zeros.data(), 4096, &wr, &ov2) && wr == 4096;
        const DWORD e2 = GetLastError();
        FlushFileBuffers(h);
        CloseHandle(h);
        std::printf("zerophys: disk=%d base=%llu cont w2=%d(e=%lu)\n", disk, base,
                    (int)w2, (unsigned long)e2);
        return (w1 && w2) ? 0 : 1;
    }
    if (mode == "mbr" || mode == "zerombr") {
        // 磁盘 MBR 引导代码（前 440B）：zero 先清零（模拟"空盘"），再 mbr 验证写入。
        const unsigned disk = (unsigned)atoi(argv[2]);
        std::wstring dev;
        {
            wchar_t buf[64] = {};
            swprintf(buf, 64, L"\\\\.\\PhysicalDrive%u", disk);
            dev = buf;
        }
        HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("open PhysicalDrive%u failed err=%lu\n", disk,
                        GetLastError());
            return 1;
        }
        if (mode == "zerombr") {
            unsigned char mbr[512] = {};
            OVERLAPPED ov{};
            DWORD rn = 0;
            bool rd = ReadFile(h, mbr, 512, &rn, &ov) && rn == 512;
            memset(mbr, 0, 440);
            OVERLAPPED ov2{};
            DWORD wr = 0;
            bool okw = rd && WriteFile(h, mbr, 512, &wr, &ov2) && wr == 512;
            std::printf("zerombr: read=%d write=%d err=%lu\n", (int)rd, (int)okw,
                        GetLastError());
            CloseHandle(h);
            return okw ? 0 : 1;
        }
        CloseHandle(h);
        const bool ok = EnsureDiskMbrCode(disk, AppDir(), log);
        std::printf("%s\nrc=%d\n", log.c_str(), (int)ok);
        return ok ? 0 : 1;
    }
    if (mode == "zero") {
        // 把分区引导区代码清零（模拟 format.com / mkntfs 留下的非微软代码），
        // 用于验证 EnsureNtfsBootRegion 真的写回了微软引导区。
        const std::wstring dev = L"\\\\.\\" + a.substr(0, 2);
        HANDLE h = CreateFileW(dev.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("open %ls failed err=%lu\n", dev.c_str(), GetLastError());
            return 1;
        }
        DWORD ret = 0;
        bool locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0,
                                      &ret, nullptr) != 0;
        DWORD lockErr = GetLastError();
        // 卷句柄裸读写：偏移用 OVERLAPPED 显式给、必须扇区对齐（2026-10-27 实测）
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
        bool r0 = volRead(0, sec0, (DWORD)sizeof(sec0));
        memset(sec0 + 0x54, 0, 426);
        SetLastError(0);
        bool w1 = r0 && volWrite(0, sec0, (DWORD)sizeof(sec0));
        DWORD e1 = GetLastError();
        // 卷仍挂载时 Windows 可能拒写**第一个扇区**（VBR）→ 先 dismount 再写
        bool dism = false;
        if (!w1 && locked) {
            dism = DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0,
                                   &ret, nullptr) != 0;
            DWORD dErr = GetLastError();
            SetLastError(0);
            w1 = volWrite(0, sec0, (DWORD)sizeof(sec0));
            std::printf("zero pbr: dismount=%d (err=%lu) retry write0=%d "
                        "err=%lu\n",
                        (int)dism, (unsigned long)dErr, (int)w1,
                        (unsigned long)GetLastError());
        }
        std::printf("zero pbr: locked=%d lockErr=%lu read=%d write0=%d err=%lu\n",
                    (int)locked, (unsigned long)lockErr, (int)r0, (int)w1,
                    (unsigned long)e1);
        std::vector<unsigned char> zeros(4096, 0);
        bool w2 = volWrite(512, zeros.data(), 4096);
        FlushFileBuffers(h);
        if (locked)
            DeviceIoControl(h, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &ret,
                            nullptr);
        CloseHandle(h);
        std::printf("zero pbr: w1=%d w2=%d locked=%d\n", (int)w1, (int)w2,
                    (int)locked);
        return (w1 && w2) ? 0 : 1;
    }
    if (mode == "pbr") {
        rc = EnsureNtfsBootRegion(a, AppDir(), log) ? 0 : 1;
    } else if (mode == "bios") {
        const std::wstring win = a + L"Windows";
        rc = EnsureBiosBootFiles(a, win, AppDir(), /*overwriteBcd=*/true, log, err)
                 ? 0
                 : 1;
    } else if (mode == "uefi") {
        if (argc < 4) {
            std::printf("uefi needs <espRoot> <targetRoot>\n");
            return 2;
        }
        const std::wstring t = widen(argv[3]);
        const std::wstring win = t + L"Windows";
        rc = EnsureUefiBootFiles(a, t, win, AppDir(), /*overwriteBcd=*/true, log,
                                 err)
                 ? 0
                 : 1;
    } else {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }
    std::printf("%s\n", log.c_str());
    if (!err.empty()) std::printf("ERR: %s\n", err.c_str());
    std::printf("rc=%d\n", rc);
    return rc;
}
