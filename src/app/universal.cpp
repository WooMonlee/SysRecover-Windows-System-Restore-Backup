// 异机还原（0.9）。见 universal.h。
#include "common/i18n.h"
#include "common/logger.h"
#include "common/process.h"
#include "common/relocate.h"
#include "universal.h"
#include "wim/wim.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace sysrecover {
namespace {

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (8 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(rd);
    return true;
}

bool PeMachineIs64(const std::wstring& exePath, bool* is64) {
    std::vector<unsigned char> b;
    if (!ReadFileBytes(exePath, b) || b.size() < 0x40) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(b.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const size_t off = (size_t)dos->e_lfanew;
    if (off + sizeof(IMAGE_NT_HEADERS) > b.size()) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(b.data() + off);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const WORD m = nt->FileHeader.Machine;
    *is64 = (m == IMAGE_FILE_MACHINE_AMD64 || m == IMAGE_FILE_MACHINE_ARM64);
    return true;
}

std::wstring LogsDir() {
    std::wstring d = LogBaseDir() + L"\\logs";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

}  // namespace

bool IsUniversalOsSupported(int major, int minor) {
    (void)minor;
    return major >= 10;  // Win10/11；Win7(6.1)/Win8.1(6.3) 不支持
}

bool IsHost64() {
    SYSTEM_INFO si = {};
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64;
}

int ProbeImageOs(const std::wstring& image, int index, int* major, int* minor,
                 bool* is64, std::wstring& err) {
    if (major) *major = 0;
    if (minor) *minor = 0;
    if (is64) *is64 = false;
    wchar_t tmp[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tmp)) {
        err = Tr(L"无法获取临时目录");
        return 1;
    }
    std::wstring dir = std::wstring(tmp) + L"zjuni-" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    WimEngine eng;
    if (!eng.ok()) {
        err = Tr(L"wimlib 初始化失败");
        return 1;
    }
    int rc = eng.ExtractPaths(
        image, index < 1 ? 1 : index,
        {L"\\Windows\\System32\\ntoskrnl.exe"}, dir);
    std::wstring kernel = dir + L"\\Windows\\System32\\ntoskrnl.exe";
    if (rc != 0 || GetFileAttributesW(kernel.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(kernel.c_str());
        RemoveDirectoryW((dir + L"\\Windows\\System32").c_str());
        RemoveDirectoryW((dir + L"\\Windows").c_str());
        RemoveDirectoryW(dir.c_str());
        err = Tr(L"镜像内缺少 ntoskrnl.exe");
        return 1;
    }
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(kernel.c_str(), &dummy);
    bool ok = false;
    if (sz) {
        std::vector<unsigned char> buf(sz);
        if (GetFileVersionInfoW(kernel.c_str(), 0, sz, buf.data())) {
            VS_FIXEDFILEINFO* fi = nullptr;
            UINT flen = 0;
            if (VerQueryValueW(buf.data(), L"\\",
                               reinterpret_cast<LPVOID*>(&fi), &flen) &&
                fi) {
                if (major) *major = HIWORD(fi->dwFileVersionMS);
                if (minor) *minor = LOWORD(fi->dwFileVersionMS);
                ok = true;
            }
        }
    }
    if (!ok) {
        err = Tr(L"无法读取镜像内系统版本");
    }
    PeMachineIs64(kernel, is64);
    DeleteFileW(kernel.c_str());
    RemoveDirectoryW((dir + L"\\Windows\\System32").c_str());
    RemoveDirectoryW((dir + L"\\Windows").c_str());
    RemoveDirectoryW(dir.c_str());
    return ok ? 0 : 1;
}

static bool CountInfRec(const std::wstring& dir, int* count, int depth) {
    if (depth > 8) return true;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CountInfRec(p, count, depth + 1);
        } else {
            std::wstring low = fd.cFileName;
            for (auto& ch : low) ch = (wchar_t)towlower(ch);
            if (low.size() > 4 && low.compare(low.size() - 4, 4, L".inf") == 0)
                ++*count;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}

bool ValidateDriverFolder(const std::wstring& dir, int* infCount,
                          std::wstring& err) {
    if (infCount) *infCount = 0;
    if (dir.empty()) {
        err = Tr(L"未选择驱动文件夹");
        return false;
    }
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"驱动文件夹不存在");
        return false;
    }
    int n = 0;
    CountInfRec(dir, &n, 0);
    if (infCount) *infCount = n;
    if (n == 0) {
        err = Tr(L"驱动文件夹里没有 .inf 文件");
        return false;
    }
    return true;
}

static bool EnableInboxStorageDrivers(const std::wstring& root,
                                      std::string& log, std::wstring& err) {
    const std::wstring hive = root + L"\\Windows\\System32\\config\\SYSTEM";
    if (GetFileAttributesW(hive.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"目标里没有 SYSTEM 注册表（不是完整的 Windows）");
        return false;
    }
    std::string out;
    int rc = RunProcess(SysToolPath(L"reg.exe"),
                        L"load HKLM\\ZJUSYS \"" + hive + L"\"", out);
    log += "universal: reg load rc=" + std::to_string(rc) + "\n" + out;
    if (rc != 0) {
        err = Tr(L"无法加载离线注册表（reg load 失败）");
        return false;
    }
    auto unload = [&]() {
        std::string o;
        RunProcess(SysToolPath(L"reg.exe"), L"unload HKLM\\ZJUSYS", o);
        log += "universal: reg unload\n" + o;
    };
    // 读 Select\Current 决定 ControlSet
    DWORD cur = 1;
    HKEY sel = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"ZJUSYS\\Select", 0, KEY_READ,
                      &sel) == ERROR_SUCCESS) {
        DWORD v = 0, sz = sizeof(v), type = 0;
        if (RegQueryValueExW(sel, L"Current", nullptr, &type, (LPBYTE)&v,
                             &sz) == ERROR_SUCCESS &&
            type == REG_DWORD && v > 0 && v < 100)
            cur = v;
        RegCloseKey(sel);
    }
    wchar_t cs[32] = {};
    swprintf(cs, 32, L"ZJUSYS\\ControlSet%03lu", (unsigned long)cur);
    const wchar_t* kDrivers[] = {L"storahci", L"stornvme", L"pciide",
                                 L"atapi",    L"amdsata",  L"amdxata",
                                 L"usbstor"};
    int changed = 0;
    for (const wchar_t* name : kDrivers) {
        std::wstring key = std::wstring(cs) + L"\\Services\\" + name;
        HKEY h = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0,
                          KEY_SET_VALUE | KEY_QUERY_VALUE, &h) != ERROR_SUCCESS)
            continue;  // 该驱动在本镜像里不存在 → 跳过
        DWORD zero = 0;
        if (RegSetValueExW(h, L"Start", 0, REG_DWORD, (const BYTE*)&zero,
                           sizeof(zero)) == ERROR_SUCCESS) {
            ++changed;
            log += "universal: enable ";
            log += std::string(name, name + wcslen(name));
            log += " (Start=0)\n";
        }
        RegCloseKey(h);
    }
    unload();
    log += "universal: in-box drivers enabled=" + std::to_string(changed) + "\n";
    if (changed == 0) {
        err = Tr(L"没有找到可启用的内置启动存储驱动（镜像可能不完整）");
        return false;
    }
    return true;
}

static bool InjectDriverPack(const std::wstring& root,
                             const std::wstring& driverDir, std::string& log,
                             std::wstring& err) {
    const std::wstring logFile = LogsDir() + L"\\dism-universal.log";
    const std::wstring args =
        L"/English /Image:\"" + root + L"\" /Add-Driver /Driver:\"" +
        driverDir + L"\" /Recurse /LogPath:\"" + logFile + L"\"";
    std::string out;
    int rc = RunProcess(SysToolPath(L"dism.exe"), args, out);
    log += "universal: dism add-driver rc=" + std::to_string(rc) + "\n" + out;
    if (rc != 0) {
        err = Tr(L"驱动注入失败（DISM 返回错误）。请查看 logs\\dism-universal.log，"
                 L"确认驱动与镜像位数/签名匹配。");
        return false;
    }
    return true;
}

bool PrepareUniversal(const std::wstring& offlineRoot,
                      const std::wstring& driverDir, std::string& log,
                      std::wstring& err) {
    if (GetFileAttributesW((offlineRoot + L"\\Windows").c_str()) ==
        INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"异机还原：目标不是完整的 Windows 目录");
        return false;
    }
    if (!EnableInboxStorageDrivers(offlineRoot, log, err))
        return false;
    if (!driverDir.empty()) {
        if (!InjectDriverPack(offlineRoot, driverDir, log, err))
            return false;
    }
    LogInfo("universal: prepared (" +
            std::string(driverDir.empty() ? "in-box only" : "in-box + pack") +
            ")");
    return true;
}

}  // namespace sysrecover
