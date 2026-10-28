// MBR 引导代码写入（2026-10-27）。见 mbrcode.h。
#include "mbrcode.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace sysrecover {
namespace {

const size_t kCodeBytes = 440;   // 引导代码区（0x000..0x1B7）
const size_t kMbrBytes = 512;    // 整扇区（含 446..509 分区表 + 510/511 签名）

std::wstring PhysicalDrive(unsigned idx) {
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"\\\\.\\PhysicalDrive%u", idx);
    return buf;
}

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

bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) != 0 && sz.QuadPart > 0 &&
              sz.QuadPart <= (1 << 20);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD rd = 0;
        ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr) != 0 &&
             rd == out.size();
    }
    CloseHandle(h);
    return ok;
}

bool ReadAt(HANDLE h, unsigned long long off, void* buf, DWORD n) {
    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD rd = 0;
    return ReadFile(h, buf, n, &rd, nullptr) != 0 && rd == n;
}

bool WriteAt(HANDLE h, unsigned long long off, const void* buf, DWORD n) {
    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD wr = 0;
    return WriteFile(h, buf, n, &wr, nullptr) != 0 && wr == n;
}

}  // namespace

bool EnsureDiskMbrCode(unsigned diskIndex, const std::wstring& exeDir,
                       std::string& log) {
    HANDLE h = CreateFileW(PhysicalDrive(diskIndex).c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log += "mbr: open PhysicalDrive" + std::to_string(diskIndex) +
               " failed err=" + std::to_string(GetLastError()) + "\n";
        return false;
    }
    unsigned char mbr[kMbrBytes] = {};
    if (!ReadAt(h, 0, mbr, (DWORD)kMbrBytes)) {
        log += "mbr: read sector 0 failed err=" + std::to_string(GetLastError()) +
               "\n";
        CloseHandle(h);
        return false;
    }
    const bool sig = mbr[510] == 0x55 && mbr[511] == 0xAA;
    bool nonzero = false;
    for (size_t i = 0; i < kCodeBytes; ++i)
        if (mbr[i]) {
            nonzero = true;
            break;
        }
    if (nonzero && sig) {
        // 已有别人的引导代码（GRUB / 厂商工具）→ 保留，别越俎代庖。
        log += "mbr: keep existing boot code (440B non-zero)\n";
        CloseHandle(h);
        return true;
    }

    std::vector<unsigned char> code;
    const std::wstring src = exeDir + L"\\bootfiles\\mbr-boot-code.bin";
    if (!ReadWholeFile(src, code) || code.size() < kCodeBytes) {
        log += "mbr: asset missing/short: " + A(src) + " (size=" +
               std::to_string(code.size()) + ")\n";
        CloseHandle(h);
        return false;
    }
    memcpy(mbr, code.data(), kCodeBytes);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (!WriteAt(h, 0, mbr, (DWORD)kMbrBytes)) {
        log += "mbr: write sector 0 failed err=" + std::to_string(GetLastError()) +
               "\n";
        CloseHandle(h);
        return false;
    }
    FlushFileBuffers(h);

    unsigned char back[kMbrBytes] = {};
    const bool rok = ReadAt(h, 0, back, (DWORD)kMbrBytes);
    CloseHandle(h);
    if (!rok || back[510] != 0x55 || back[511] != 0xAA ||
        memcmp(back, mbr, kCodeBytes) != 0) {
        log += "mbr: verify FAIL (readback mismatch)\n";
        return false;
    }
    log += "mbr: boot code written (440B, partition table + 0x55AA preserved)\n";
    return true;
}

}  // namespace sysrecover
