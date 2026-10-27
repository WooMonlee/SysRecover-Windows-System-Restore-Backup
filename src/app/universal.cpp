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

bool PeMachineIs64(const std::wstring& exePath, bool* is64) {
    if (!is64) return false;
    *is64 = false;
    HANDLE h = CreateFileW(exePath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos = {};
    DWORD rd = 0;
    bool ok = ReadFile(h, &dos, sizeof(dos), &rd, nullptr) &&
              rd == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (ok) {
        LARGE_INTEGER pos = {};
        pos.QuadPart = dos.e_lfanew;
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != FALSE;
    }
    struct NtHead {
        DWORD signature;
        IMAGE_FILE_HEADER file;
    } nt = {};
    if (ok) {
        ok = ReadFile(h, &nt, sizeof(nt), &rd, nullptr) &&
             rd == sizeof(nt) && nt.signature == IMAGE_NT_SIGNATURE;
    }
    CloseHandle(h);
    if (!ok) return false;
    const WORD m = nt.file.Machine;
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

struct FileVersion {
    int major = 0;
    int minor = 0;
    unsigned long build = 0;
    bool ok = false;
};

bool ReadPeFileVersion(const std::wstring& path, FileVersion* out) {
    if (out) *out = FileVersion{};
    if (path.empty() ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return false;
    std::vector<unsigned char> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return false;

    // 关键（0.9.33 实测定因）：Windows 10 的 ntoskrnl.exe / winload.exe
    // 的 VS_FIXEDFILEINFO 里 dwFileVersionMS 仍是兼容版本 6.2，而真正的
    // 10.0.19041 在 StringFileInfo 的 FileVersion 字符串里。旧代码读
    // fixed info，把 Win10 误判成 6.2。先读字符串版本，失败才回退 fixed。
    struct LangCp {
        WORD lang;
        WORD cp;
    };
    LangCp* tr = nullptr;
    UINT trLen = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                       reinterpret_cast<LPVOID*>(&tr), &trLen) &&
        tr && trLen >= sizeof(LangCp)) {
        const UINT n = trLen / sizeof(LangCp);
        for (UINT i = 0; i < n; ++i) {
            for (const wchar_t* field :
                 {L"FileVersion", L"ProductVersion"}) {
                wchar_t sub[160] = {};
                swprintf(sub, 160, L"\\StringFileInfo\\%04x%04x\\%ls",
                         (unsigned)tr[i].lang, (unsigned)tr[i].cp, field);
                wchar_t* val = nullptr;
                UINT vlen = 0;
                if (!VerQueryValueW(buf.data(), sub,
                                    reinterpret_cast<LPVOID*>(&val), &vlen) ||
                    !val)
                    continue;
                int a = 0, b = 0, c = 0;
                if (swscanf(val, L"%d.%d.%d", &a, &b, &c) == 3) {
                    out->major = a;
                    out->minor = b;
                    out->build = (unsigned long)c;
                    out->ok = true;
                    return true;
                }
            }
        }
    }

    VS_FIXEDFILEINFO* fi = nullptr;
    UINT flen = 0;
    if (!VerQueryValueW(buf.data(), L"\\",
                        reinterpret_cast<LPVOID*>(&fi), &flen) ||
        !fi)
        return false;
    if (out) {
        out->major = HIWORD(fi->dwFileVersionMS);
        out->minor = LOWORD(fi->dwFileVersionMS);
        out->build = HIWORD(fi->dwFileVersionLS);
        out->ok = true;
    }
    return true;
}

bool IsNewerFileVersion(const FileVersion& a, const FileVersion& b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.build > b.build;
}

std::string VersionText(const FileVersion& v) {
    if (!v.ok) return "unknown";
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
           std::to_string(v.build);
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
    // 0.9.33（用户 2026-10-26 实测）：只读 ntoskrnl.exe 在部分 PE/定制镜像上
    // 会得到 6.2（内核文件版本被改过/提取异常），而同一镜像的 winload.exe 是
    // 10.0。两个都取、选更高者，既保留 Win7/8.1 的正确拒绝，也不会把 Win10
    // 误判成 6.2。日志同时打印两边的版本，便于现场定位。
    const std::wstring kernelRel = L"\\Windows\\System32\\ntoskrnl.exe";
    const std::wstring loaderRel = L"\\Windows\\System32\\winload.exe";
    const int imageIndex = index < 1 ? 1 : index;
    bool kernelInImage = false, loaderInImage = false;
    const int kernelMeta = eng.ImagePathExists(image, imageIndex, kernelRel,
                                               &kernelInImage);
    eng.ImagePathExists(image, imageIndex, loaderRel, &loaderInImage);
    LogInfo("universal: image metadata ntoskrnl=" +
            std::string(kernelInImage ? "yes" : "no") + " winload=" +
            std::string(loaderInImage ? "yes" : "no"));
    int rc = eng.ExtractPaths(image, imageIndex, {kernelRel, loaderRel}, dir);
    if (rc != 0)
        LogWarn("universal: extract version probes rc=" +
                std::to_string(rc) + " (checking extracted files anyway)");
    const std::wstring kernel = dir + kernelRel;
    const std::wstring loader = dir + loaderRel;
    bool kernelExists =
        GetFileAttributesW(kernel.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool loaderExists =
        GetFileAttributesW(loader.c_str()) != INVALID_FILE_ATTRIBUTES;
    // 某些镜像/PE 上一次提取两个路径会因其中一个缺失而整体失败；分别补取，
    // 只要 winload 或 ntoskrnl 有一个能读到就能判版本（用户 2026-10-26 的
    // “镜像内缺少 ntoskrnl.exe”正是这种单文件提取异常）。
    if (!loaderExists) {
        int r2 = eng.ExtractPaths(image, imageIndex, {loaderRel}, dir);
        loaderExists =
            GetFileAttributesW(loader.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (r2 != 0 && loaderExists)
            LogWarn("universal: winload single extract rc=" +
                    std::to_string(r2) + " (file present)");
    }
    if (!kernelExists) {
        int r2 = eng.ExtractPaths(image, imageIndex, {kernelRel}, dir);
        kernelExists =
            GetFileAttributesW(kernel.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (r2 != 0 && kernelExists)
            LogWarn("universal: ntoskrnl single extract rc=" +
                    std::to_string(r2) + " (file present)");
    }
    // 元数据也只作为诊断参考：某些自定义/精简镜像的 WIM 路径与提取器匹配
    // 不一致，不能仅凭它把“还能从 winload 判版本”的镜像判死。若确实缺
    // ntoskrnl，后面的 winload 兜底仍会继续；还原完成后的启动检查会兜底。
    if (!kernelExists && kernelMeta == 0 && !kernelInImage)
        LogWarn("universal: metadata says ntoskrnl is absent; using winload "
                "version probe");
    if (!kernelExists && !loaderExists) {
        DeleteFileW(kernel.c_str());
        DeleteFileW(loader.c_str());
        RemoveDirectoryW((dir + L"\\Windows\\System32").c_str());
        RemoveDirectoryW((dir + L"\\Windows").c_str());
        RemoveDirectoryW(dir.c_str());
        err = Tr(L"镜像内缺少 ntoskrnl.exe");
        return 1;
    }
    FileVersion kv, lv;
    const bool kok = kernelExists && ReadPeFileVersion(kernel, &kv);
    const bool lok = loaderExists && ReadPeFileVersion(loader, &lv);
    std::wstring pickedPath;
    FileVersion picked;
    if (kok && (!lok || !IsNewerFileVersion(lv, kv))) {
        picked = kv;
        pickedPath = kernel;
    } else if (lok) {
        picked = lv;
        pickedPath = loader;
    }
    LogInfo("universal: image version probes ntoskrnl=" +
            VersionText(kv) + " winload=" + VersionText(lv) + " -> " +
            VersionText(picked));
    bool ok = picked.ok;
    if (ok) {
        if (major) *major = picked.major;
        if (minor) *minor = picked.minor;
        PeMachineIs64(pickedPath, is64);
    } else {
        err = Tr(L"无法读取镜像内系统版本");
    }
    DeleteFileW(kernel.c_str());
    DeleteFileW(loader.c_str());
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
    const std::wstring dism = SysToolPath(L"dism.exe");
    if (GetFileAttributesW(dism.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"当前恢复环境没有 dism.exe，无法注入驱动包。"
                 L"请使用带 DISM 的完整 PE/Windows，或只做同机/就地还原。");
        log += "universal: dism.exe missing\n";
        return false;
    }
    const std::wstring args =
        L"/English /Image:\"" + root + L"\" /Add-Driver /Driver:\"" +
        driverDir + L"\" /Recurse /LogPath:\"" + logFile + L"\"";
    std::string out;
    int rc = RunProcess(dism, args, out);
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
                      std::wstring& err, ProgressFn progress) {
    auto tick = [&](int pct, const char* stage) {
        if (progress) progress(pct, std::string(stage));
    };
    tick(0, "universal-start");
    if (GetFileAttributesW((offlineRoot + L"\\Windows").c_str()) ==
        INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"异机还原：目标不是完整的 Windows 目录");
        return false;
    }
    tick(20, "universal-inbox");
    if (!EnableInboxStorageDrivers(offlineRoot, log, err))
        return false;
    tick(45, "universal-inbox-done");
    if (!driverDir.empty()) {
        tick(55, "universal-drivers");
        if (!InjectDriverPack(offlineRoot, driverDir, log, err))
            return false;
    }
    tick(100, "universal-done");
    LogInfo("universal: prepared (" +
            std::string(driverDir.empty() ? "in-box only" : "in-box + pack") +
            ")");
    return true;
}

}  // namespace sysrecover
