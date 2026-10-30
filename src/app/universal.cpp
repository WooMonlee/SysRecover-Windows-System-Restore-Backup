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

// ── 驱动包扫描与过滤（2026-10-30）────────────────────────────────────
// 用户 2026-10-29 实测的教训：`dism /Add-Driver /Driver:<文件夹> /Recurse`
// 一把梭有三大问题 ——
//   ① 105 个包里 90 个是网络/打印/蓝牙/显卡，对"能不能开机"毫无作用，
//      DISM 要跑 5 分钟；
//   ② 其中 2 个是 DISM 拒绝的"未签名启动关键驱动"（hidi2c/netvwifibus），
//      返回 rc=50 → 我们当成整单失败，21 分钟白跑；
//   ③ 包里还混着 Windows 11 驱动（往 19044 镜像里灌）。
// 现在：只收集启动关键类 + 跳过 Microsoft 自带 + 跳过 OS 不匹配，逐个注入，
// 单个失败只记日志（除非一个都没成功）。
struct InfInfo {
    std::wstring path;
    std::wstring cls;       // [Version] Class=
    std::wstring provider;  // [Version] Provider=
};

// 读文本文件（INF 可能是 UTF-16LE/BE 或 ANSI，AutoIt/厂商工具都有）
std::wstring ReadTextFileAuto(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (4 << 20)) {
        CloseHandle(h);
        return {};
    }
    std::vector<char> buf((size_t)sz.QuadPart);
    DWORD rd = 0;
    const bool ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) &&
                    rd == buf.size();
    CloseHandle(h);
    if (!ok)
        return {};
    const unsigned char* b = (const unsigned char*)buf.data();
    const size_t n = buf.size();
    if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) {  // UTF-16LE BOM
        return std::wstring((const wchar_t*)(b + 2), (n - 2) / 2);
    }
    if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) {  // UTF-16BE BOM
        std::wstring w((n - 2) / 2, 0);
        for (size_t k = 0; k + 1 < n - 1; k += 2)
            w[k / 2] = (wchar_t)((b[k + 2] << 8) | b[k + 3]);
        return w;
    }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {
        int need = MultiByteToWideChar(CP_UTF8, 0, (const char*)b + 3,
                                       (int)(n - 3), nullptr, 0);
        std::wstring w(need > 0 ? need : 0, 0);
        if (need > 0)
            MultiByteToWideChar(CP_UTF8, 0, (const char*)b + 3, (int)(n - 3),
                                &w[0], need);
        return w;
    }
    // 无 BOM：前 64 字节里有大量 0x00 → 当作 UTF-16LE；否则按 ANSI
    size_t zeros = 0;
    for (size_t k = 1; k < n && k < 64; k += 2)
        if (b[k] == 0)
            ++zeros;
    if (zeros >= 8)
        return std::wstring((const wchar_t*)b, n / 2);
    int need = MultiByteToWideChar(CP_ACP, 0, (const char*)b, (int)n, nullptr, 0);
    std::wstring w(need > 0 ? need : 0, 0);
    if (need > 0)
        MultiByteToWideChar(CP_ACP, 0, (const char*)b, (int)n, &w[0], need);
    return w;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r'))
        --b;
    return s.substr(a, b - a);
}

// 只解析 [Version] 段里的 Class=/Provider=（其它段里有同名键，不能乱抓）
void ParseInfVersion(const std::wstring& text, std::wstring* cls,
                     std::wstring* prov) {
    size_t pos = 0;
    bool inVer = false;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos)
            eol = text.size();
        std::wstring line = Trim(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (!line.empty() && line[0] == L'[') {
            std::wstring sec = line;
            for (auto& c : sec)
                c = (wchar_t)towlower(c);
            inVer = (sec.find(L"[version]") == 0);
            continue;
        }
        if (!inVer)
            continue;
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos)
            continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        for (auto& c : key)
            c = (wchar_t)towlower(c);
        if (key == L"class" && cls && cls->empty())
            *cls = val;
        else if (key == L"provider" && prov && prov->empty())
            *prov = val;
    }
}

bool IsBootCriticalClass(const std::wstring& c) {
    static const wchar_t* kKeep[] = {
        L"SCSIAdapter", L"HDC", L"System",   L"DiskDrive", L"Volume",
        L"USB",         L"HIDClass", L"Keyboard", L"Mouse", L"SDHost"};
    for (const wchar_t* k : kKeep)
        if (_wcsicmp(c.c_str(), k) == 0)
            return true;
    return false;
}

// pathLower 用来做 OS 版本判断（Windows11-x64\22000 这类目录）
bool PathLooksNewerOs(const std::wstring& lowPath) {
    return lowPath.find(L"windows11") != std::wstring::npos ||
           lowPath.find(L"win11") != std::wstring::npos;
}
bool PathLooksOlderOs(const std::wstring& lowPath) {
    return lowPath.find(L"windows7") != std::wstring::npos ||
           lowPath.find(L"win7") != std::wstring::npos ||
           lowPath.find(L"windows8") != std::wstring::npos ||
           lowPath.find(L"win8") != std::wstring::npos ||
           lowPath.find(L"vista") != std::wstring::npos;
}

// 递归收集 INF（深度上限 32：Dell WinPE 包最深 11 层，旧上限 8 会漏数）
bool ScanInfs(const std::wstring& dir, std::vector<InfInfo>& out, int depth) {
    if (depth > 32)
        return true;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return true;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanInfs(p, out, depth + 1);
        } else {
            std::wstring low = fd.cFileName;
            for (auto& ch : low)
                ch = (wchar_t)towlower(ch);
            if (low.size() > 4 && low.compare(low.size() - 4, 4, L".inf") == 0) {
                InfInfo info;
                info.path = p;
                std::wstring text = ReadTextFileAuto(p);
                if (!text.empty())
                    ParseInfVersion(text, &info.cls, &info.provider);
                out.push_back(std::move(info));
            }
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
    std::vector<InfInfo> all;
    ScanInfs(dir, all, 0);
    if (infCount) *infCount = (int)all.size();
    if (all.empty()) {
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

// 逐个注入（每个 INF 一次 DISM）：
//   · 成功 → ok++（记录是否覆盖了存储类）
//   · 失败 → 带 /ForceUnsigned 重试一次；仍失败只记日志并继续
//   · 只有"有候选但一个都没成功"才判失败（用户案例：103 成功、2 个非存储失败
//     不该整单失败）
static bool InjectDriverPack(const std::wstring& root,
                             const std::wstring& driverDir,
                             unsigned long imageBuild, std::string& log,
                             std::wstring& err, ProgressFn progress) {
    const std::wstring dism = SysToolPath(L"dism.exe");
    if (GetFileAttributesW(dism.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"当前恢复环境没有 dism.exe，无法注入驱动包。"
                 L"请使用带 DISM 的完整 PE/Windows，或只做同机/就地还原。");
        log += "universal: dism.exe missing\n";
        return false;
    }
    std::vector<InfInfo> all;
    ScanInfs(driverDir, all, 0);
    std::vector<InfInfo> keep;
    int skipClass = 0, skipMs = 0, skipOs = 0;
    for (const auto& d : all) {
        std::wstring low = d.path;
        for (auto& c : low)
            c = (wchar_t)towlower(c);
        if (!IsBootCriticalClass(d.cls)) {
            ++skipClass;
            continue;
        }
        {
            std::wstring plow = d.provider;
            for (auto& c : plow)
                c = (wchar_t)towlower(c);
            // INF 里 Microsoft 通常写成 %Msft% / %Microsoft% 变量
            if (plow == L"microsoft" || plow == L"%msft%" ||
                plow.find(L"microsoft") != std::wstring::npos) {
                ++skipMs;  // 镜像自带（且 DISM 会拒绝其中未签名项），不注入
                continue;
            }
        }
        if (imageBuild > 0 && imageBuild < 22000 && PathLooksNewerOs(low)) {
            ++skipOs;  // Win10 镜像不灌 Win11 驱动
            continue;
        }
        if (imageBuild >= 10240 && PathLooksOlderOs(low)) {
            ++skipOs;  // 也不灌 Win7/8 驱动
            continue;
        }
        keep.push_back(d);
    }
    log += "universal: drivers scanned=" + std::to_string(all.size()) +
           " kept=" + std::to_string(keep.size()) +
           " skip(class)=" + std::to_string(skipClass) +
           " skip(microsoft)=" + std::to_string(skipMs) +
           " skip(os)=" + std::to_string(skipOs) + "\n";
    LogInfo("universal: drivers scanned=" + std::to_string(all.size()) +
            " kept=" + std::to_string(keep.size()) +
            " skipClass=" + std::to_string(skipClass) +
            " skipMS=" + std::to_string(skipMs) +
            " skipOS=" + std::to_string(skipOs));
    if (keep.empty()) {
        LogInfo("universal: no boot-critical drivers to inject (in-box only)");
        return true;
    }
    int ok = 0, failed = 0;
    bool storageOk = false;
    std::string failedList;
    for (size_t i = 0; i < keep.size(); ++i) {
        const std::wstring& inf = keep[i].path;
        std::wstring stem = inf;
        {   // 只留文件名，日志更短
            size_t sl = stem.find_last_of(L"\\/");
            if (sl != std::wstring::npos)
                stem = stem.substr(sl + 1);
        }
        if (progress) {
            const int pct = 50 + (int)(40 * i / keep.size());
            progress(pct, "universal-driver " + std::to_string(i + 1) + "/" +
                              std::to_string(keep.size()));
        }
        std::wstring args = L"/English /Image:\"" + root +
                            L"\" /Add-Driver /Driver:\"" + inf + L"\"";
        std::string out;
        int rc = RunProcess(dism, args, out);
        if (rc != 0) {
            // ⚠️ 不重试 /ForceUnsigned：强行安装"未签名的启动关键驱动"会让目标
            // 系统在 Secure Boot 下起不来，风险远大于收益。DISM 拒绝的就跳过
            // （用户案例里被拒的 hidi2c/netvwifibus 与启动无关）。
            ++failed;
            if (failedList.size() < 1200) {
                std::string nm(stem.begin(), stem.end());
                std::string why;
                // 取 DISM 输出里带 Error 的那一行（或最后一行）
                size_t ep = out.find("Error");
                if (ep == std::string::npos)
                    ep = out.rfind('\n', out.size() > 2 ? out.size() - 2 : 0);
                if (ep != std::string::npos)
                    why = out.substr(ep, 160);
                for (auto& c : why)
                    if (c == '\r' || c == '\n')
                        c = ' ';
                failedList += nm + " (" + why + "); ";
            }
            log += "universal: skipped " + std::string(stem.begin(), stem.end()) +
                   "\n" + out;
            continue;
        }
        ++ok;
        if (_wcsicmp(keep[i].cls.c_str(), L"SCSIAdapter") == 0 ||
            _wcsicmp(keep[i].cls.c_str(), L"HDC") == 0 ||
            _wcsicmp(keep[i].cls.c_str(), L"DiskDrive") == 0 ||
            _wcsicmp(keep[i].cls.c_str(), L"Volume") == 0)
            storageOk = true;
        log += "universal: added " + std::string(stem.begin(), stem.end()) +
               " (class=" + std::string(keep[i].cls.begin(),
                                        keep[i].cls.end()) + ")\n";
    }
    log += "universal: inject ok=" + std::to_string(ok) +
           " failed=" + std::to_string(failed) + "\n";
    if (!failedList.empty())
        log += "universal: failed list: " + failedList + "\n";
    LogInfo("universal: inject ok=" + std::to_string(ok) +
            " failed=" + std::to_string(failed) +
            (storageOk ? " (storage ok)" : " (no storage driver added)"));
    if (failed > 0)
        LogWarn("universal: " + std::to_string(failed) +
                " driver package(s) skipped (non-fatal); see log");
    if (ok == 0) {
        err = Tr(L"驱动注入全部失败（") + std::to_wstring(keep.size()) +
              Tr(L" 个候选都没装上）。请确认驱动与镜像位数匹配，"
                 L"或只做同机/就地还原。详见日志。");
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
        // 镜像 build（决定要不要跳过 Windows11/Windows7 驱动目录）
        unsigned long build = 0;
        FileVersion v;
        if (ReadPeFileVersion(offlineRoot + L"\\Windows\\System32\\ntoskrnl.exe",
                              &v) && v.ok)
            build = v.build;
        LogInfo("universal: target build=" + std::to_string(build));
        tick(50, "universal-drivers");
        if (!InjectDriverPack(offlineRoot, driverDir, build, log, err,
                              progress))
            return false;
    }
    tick(100, "universal-done");
    LogInfo("universal: prepared (" +
            std::string(driverDir.empty() ? "in-box only" : "in-box + pack") +
            ")");
    return true;
}

// ── 导出本机启动关键驱动（2026-10-30）────────────────────────────────
// 用 Class GUID 过滤而不是类名字符串：pnputil 的标签会随系统语言变化，
// GUID 不会。只导"能不能开机"相关的类，避免 1.8GB 全量导出。
int ExportBootCriticalDrivers(const std::wstring& destDir, int* exported,
                              std::wstring& err) {
    if (exported) *exported = 0;
    const std::wstring pnputil = SysToolPath(L"pnputil.exe");
    if (GetFileAttributesW(pnputil.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = Tr(L"当前系统没有 pnputil.exe，无法导出驱动");
        return 1;
    }
    std::string out;
    int rc = RunProcess(pnputil, L"/enum-drivers", out);
    if (rc != 0) {
        err = Tr(L"pnputil /enum-drivers 失败（rc=") + std::to_wstring(rc) +
              Tr(L"）");
        return 1;
    }
    // Class GUID 白名单（与系统语言无关）
    static const char* kKeepGuid[] = {
        "{4d36e97b-e325-11ce-bfc1-08002be10318}",  // SCSIAdapter
        "{4d36e96a-e325-11ce-bfc1-08002be10318}",  // HDC
        "{4d36e97d-e325-11ce-bfc1-08002be10318}",  // System
        "{4d36e967-e325-11ce-bfc1-08002be10318}",  // DiskDrive
        "{71a27cdd-812a-11d0-bec7-08002be2092f}",  // Volume
        "{36fc9e60-c465-11cf-8056-444553540000}",  // USB
        "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",  // HIDClass
        "{4d36e96b-e325-11ce-bfc1-08002be10318}",  // Keyboard
        "{4d36e96f-e325-11ce-bfc1-08002be10318}",  // Mouse
        "{a0a588a4-c46f-4b37-b7ea-c82fe89870c6}",  // SDHost
    };
    auto lowerCopy = [](const std::string& in) {
        std::string o = in;
        for (auto& c : o)
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        return o;
    };
    std::vector<std::string> oem;
    {
        const std::string low = lowerCopy(out);
        size_t pos = 0;
        std::string curOem;
        bool keep = false;
        while (pos <= low.size()) {
            size_t eol = low.find('\n', pos);
            if (eol == std::string::npos) eol = low.size();
            const std::string line = low.substr(pos, eol - pos);
            // 找 oemNN.inf
            size_t o = line.find("oem");
            if (o != std::string::npos) {
                size_t e = line.find(".inf", o);
                if (e != std::string::npos && e > o) {
                    bool digits = true;
                    for (size_t k = o + 3; k < e; ++k)
                        if (line[k] < '0' || line[k] > '9') { digits = false; break; }
                    if (digits && e > o + 3)
                        curOem = line.substr(o, e + 4 - o);
                }
            }
            for (const char* g : kKeepGuid)
                if (line.find(g) != std::string::npos) { keep = true; break; }
            // 空行 = 一条记录结束
            if (line.find_first_not_of(" \t\r") == std::string::npos) {
                if (!curOem.empty() && keep) oem.push_back(curOem);
                curOem.clear();
                keep = false;
            }
            if (eol == low.size()) break;
            pos = eol + 1;
        }
        if (!curOem.empty() && keep) oem.push_back(curOem);
    }
    if (oem.empty()) {
        err = Tr(L"没有找到可导出的启动关键驱动（可能本机用的是标准 AHCI/NVMe）");
        return 1;
    }
    if (!destDir.empty())
        CreateDirectoryW(destDir.c_str(), nullptr);
    int ok = 0;
    for (const auto& name : oem) {
        std::wstring wname(name.begin(), name.end());
        std::string o2;
        int r2 = RunProcess(pnputil,
                            L"/export-driver " + wname + L" \"" + destDir + L"\"",
                            o2);
        if (r2 == 0) {
            ++ok;
        } else {
            LogWarn("export-driver failed: " +
                    std::string(name.begin(), name.end()));
        }
    }
    if (exported) *exported = ok;
    LogInfo("export boot-critical drivers: " + std::to_string(ok) + "/" +
            std::to_string(oem.size()) + " -> " +
            std::string(destDir.begin(), destDir.end()));
    if (ok == 0) {
        err = Tr(L"驱动导出全部失败（权限不足或驱动被占用）");
        return 1;
    }
    return 0;
}

}  // namespace sysrecover
