// SysRecover CLI（Phase 0）：version / diag。完整命令见 AGENTS.md §9。
// 退出码：0 成功，1 通用失败，2 参数错误（与 AGENTS.md §9 一致）。
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW（P11：从宽命令行取参数）
#include <shlobj.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wimlib.h"
#include "../common/version.h"
#include "../common/zip.h"
#include "../app/ops.h"
#include "../app/safety.h"
#include "../app/shortcut.h"
#include "../boot/bcd.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/logger.h"
#include "../common/process.h"
#include "../common/progress.h"
#include "../common/selfarch.h"
#include "../common/singleton.h"
#include "../disk/disk.h"
#include "../wim/exclude.h"
#include "../wim/wim.h"

namespace {

bool IsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                 &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

int CmdVersion() {
    std::printf("%s %s (contract v%d, MinGW %s)\n", SYSRECOVER_NAME,
                SYSRECOVER_VERSION, SYSRECOVER_CONTRACT_VERSION, __VERSION__);
    return 0;
}

// 收集诊断信息到文本 —— `diag` 打印它，`diag --zip` 把它打进诊断包。
// 返回 wimlib 初始化结果（0=OK），供调用方决定退出码。
int DiagText(std::string& out) {
    char buf[640];
    snprintf(buf, sizeof(buf), "[diag] admin=%s\n", IsAdmin() ? "yes" : "no");
    out += buf;
    snprintf(buf, sizeof(buf), "[diag] firmware=%s\n",
             sysrecover::IsUefiFirmware() ? "UEFI" : "BIOS");
    out += buf;
    if (sysrecover::IsUefiFirmware()) {
        snprintf(buf, sizeof(buf), "[diag] secureboot=%s\n",
                 sysrecover::IsSecureBootEnabled() ? "ON (需签名引导)" : "off");
        out += buf;
        std::string detail;
        bool installed = sysrecover::UefiBootEntryExists(detail);
        snprintf(buf, sizeof(buf), "[diag] uefi boot entry=%s (%s)\n",
                 installed ? "installed" : "not installed", detail.c_str());
        out += buf;
        // P6：固件信任哪张微软 UEFI CA（决定我们的救援环境能不能起来）
        int ca = sysrecover::FirmwareTrustedUefiCas();
        snprintf(buf, sizeof(buf), "[diag] firmware db: CA2011=%s CA2023=%s\n",
                 (ca & sysrecover::kFirmwareCa2011) ? "yes" : "no",
                 (ca & sysrecover::kFirmwareCa2023) ? "yes" : "no");
        out += buf;
        if (ca && !(ca & sysrecover::kFirmwareCa2011) &&
            (ca & sysrecover::kFirmwareCa2023))
            out += "[diag] WARN: 本机固件只信任 CA2023，而我们的救援环境用 CA2011 "
                   "签名 → 可能起不来（见 PLAN.md §11 备选 B/C）\n";
    }
    int rc = wimlib_global_init(0);
    snprintf(buf, sizeof(buf), "[diag] wimlib_global_init -> %d (%s)\n", rc,
             rc == 0 ? "OK" : "FAIL");
    out += buf;
    if (rc == 0)
        wimlib_global_cleanup();
    snprintf(buf, sizeof(buf), "[diag] version=%s\n", SYSRECOVER_VERSION);
    out += buf;
    // 工具路径自检：32 位 exe 在 64 位 Windows 上必须命中原生 System32（WOW64/Sysnative），
    // 否则写 BCD / 修引导会「找不到文件」。见 src/common/process.cpp::SysToolPath。
    {
        BOOL wow = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow);
        std::wstring bcdedit = sysrecover::SysToolPath(L"bcdedit.exe");
        std::wstring bcdboot = sysrecover::SysToolPath(L"bcdboot.exe");
        char p1[MAX_PATH * 2] = {}, p2[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_UTF8, 0, bcdedit.c_str(), -1, p1, sizeof(p1),
                            nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, bcdboot.c_str(), -1, p2, sizeof(p2),
                            nullptr, nullptr);
        snprintf(buf, sizeof(buf),
                 "[diag] proc=%s wow64=%s\n"
                 "[diag] tools: bcdedit=%s (%s), bcdboot=%s (%s)\n",
                 sizeof(void*) == 4 ? "x86" : "x64", wow ? "yes" : "no", p1,
                 GetFileAttributesW(bcdedit.c_str()) != INVALID_FILE_ATTRIBUTES
                     ? "ok" : "MISSING",
                 p2,
                 GetFileAttributesW(bcdboot.c_str()) != INVALID_FILE_ATTRIBUTES
                     ? "ok" : "MISSING");
        out += buf;
    }
    std::wstring exeDir = sysrecover::ExeDir();
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, exeDir.c_str(), -1, narrow, sizeof(narrow),
                        nullptr, nullptr);
    snprintf(buf, sizeof(buf), "[diag] exe_dir=%s\n", narrow);
    out += buf;
    return rc;
}

int CmdDiag() {
    std::string t;
    int rc = DiagText(t);
    std::fputs(t.c_str(), stdout);
    return rc == 0 ? 0 : 1;
}

namespace {

// Ctrl+C/Break 取消：只置标志，由进度回调返回 true → wimlib 以
// WIMLIB_ERR_ABORTED_BY_PROGRESS(76) 干净中止（不硬杀进程、不破坏正在写的文件）。
volatile LONG g_cancel = 0;
BOOL WINAPI OnCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&g_cancel, 1);
        return TRUE;
    }
    return FALSE;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty())
        return L"";
    int n =
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// 宽 → UTF-8（参数统一走 UTF-8）
std::string W2U8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr,
                            nullptr);
    return s;
}

// 取命令行参数（**UTF-8**）—— P11。
// 为什么不用 argv：C 运行时给的 argv 是 **ANSI**（当前代码页），而下游的 ToWide()
// 是按 UTF-8 解的 → 中文路径被弄乱（实测 `images --file 中文.esd` 报
// "Failed to open a file"）。改用 GetCommandLineW + CommandLineToArgvW 拿宽字符，
// 再统一转成 UTF-8，整条链路就自洽了。
std::vector<std::string> Utf8Args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w)
        return out;
    for (int i = 1; i < n; ++i)  // 跳过 argv[0]（exe 路径）
        out.push_back(W2U8(w[i]));
    LocalFree(w);
    return out;
}

std::string Opt(const std::vector<std::string>& a, const char* key,                const std::string& def = "") {
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == key)
            return a[i + 1];
    return def;
}

bool Has(const std::vector<std::string>& a, const char* key) {
    for (const auto& x : a)
        if (x == key)
            return true;
    return false;
}

// 单行进度（\r 刷新）+ 进度文件，供外部工具读取。返回 true 暂不支持取消。
std::string g_phase = "idle";
std::chrono::steady_clock::time_point g_start =
    std::chrono::steady_clock::now();

std::string FmtElapsed() {
    auto s = std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::steady_clock::now() - g_start)
                 .count();
    char b[32];
    if (s >= 3600)
        snprintf(b, sizeof(b), "%ld:%02ld:%02ld", (long)(s / 3600),
                 (long)((s % 3600) / 60), (long)(s % 60));
    else
        snprintf(b, sizeof(b), "%02ld:%02ld", (long)(s / 60), (long)(s % 60));
    return b;
}

bool ConsoleProgress(int pct, const std::string& stage) {
    if (InterlockedCompareExchange(&g_cancel, 0, 0) != 0)
        return true;  // 用户 Ctrl+C → 干净中止
    static int last = -1;
    static std::string lastStage;
    static std::string lastElapsed;
    std::string el = FmtElapsed();
    if (pct == last && stage == lastStage && el == lastElapsed)
        return false;
    last = pct;
    lastStage = stage;
    lastElapsed = el;
    std::printf("\r  %3d%%  %s   已用 %s   ", pct, stage.c_str(), el.c_str());
    fflush(stdout);
    sysrecover::ProgressUpdate(g_phase, pct, stage);
    return false;
}

int CmdBackup(const std::vector<std::string>& a) {
    std::string dest = Opt(a, "--dest");
    if (dest.empty()) {
        std::printf("缺少 --dest <wim路径>\n");
        return 2;
    }
    sysrecover::BackupRequest req;
    req.source = ToWide(Opt(a, "--source", "C:/"));
    req.dest = ToWide(dest);
    req.compress = Opt(a, "--compress", "fast");
    req.name = ToWide(Opt(a, "--name", "Backup"));
    req.append = Has(a, "--append");
    req.verify = Has(a, "--verify");
    req.snapshot = Has(a, "--snapshot");
    DWORD attr = GetFileAttributesW(req.dest.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !req.append && !Has(a, "--yes") &&
        !Has(a, "-y")) {
        std::printf("目标文件已存在，覆盖需加 --yes 确认（追加请用 --append）\n");
        return 2;
    }
    std::string err;
    g_start = std::chrono::steady_clock::now();
    int rc = sysrecover::RunBackup(req, ConsoleProgress, err);
    std::printf("\n");
    if (rc != 0) {
        std::printf("%s\n", err.c_str());
        std::string advice = sysrecover::ErrorAdvice(rc, err);  // P8
        if (!advice.empty())
            std::printf("%s\n", advice.c_str());
        if (g_cancel) {
            std::printf("已取消\n");
            return 6;  // §9 退出码 6=取消
        }
        return rc == 5 ? 5 : 1;
    }
    std::printf("%s（用时 %s）\n",
                req.verify ? "备份并校验完成" : "备份完成",
                FmtElapsed().c_str());
    return 0;
}

int CmdRestore(const std::vector<std::string>& a) {
    std::string image = Opt(a, "--image");
    std::string diskS = Opt(a, "--disk");
    std::string partS = Opt(a, "--part");
    if (image.empty() || diskS.empty() || partS.empty()) {
        std::printf("缺少 --image/--disk/--part\n");
        return 2;
    }
    if (!Has(a, "--yes") && !Has(a, "-y")) {
        std::printf("还原将覆盖目标分区，需加 --yes 确认\n");
        return 2;
    }
    int disk = std::atoi(diskS.c_str());
    int part = std::atoi(partS.c_str());
    int index = std::atoi(Opt(a, "--index", "1").c_str());
    if (index < 1)
        index = 1;
    bool repairBoot = !Has(a, "--no-repair-boot");
    if (Has(a, "--apply")) {
        // 直接应用模式（Phase 2 遗留，用于非系统分区验证）
        sysrecover::PartitionInfo target;
        bool found = false;
        for (const auto& d : sysrecover::EnumerateDisks()) {
            if ((int)d.index != disk)
                continue;
            for (const auto& p : d.parts) {
                if ((int)p.partNumber != part)
                    continue;
                target = p;
                found = true;
            }
        }
        if (!found) {
            std::printf("找不到目标分区 磁盘%d 分区%d\n", disk, part);
            return 1;
        }
        if (!sysrecover::AcquireOpLock()) {
            std::printf("已有备份/还原实例在运行，本次退出\n");
            return 1;
        }
        g_phase = "restore";
        sysrecover::ProgressUpdate(g_phase, 0, "start");
        g_start = std::chrono::steady_clock::now();
        if (target.letter.empty()) {
            std::printf("目标分区无盘符，无法直接应用\n");
            return 1;
        }
        if (target.isSystem)
            std::wprintf(L"警告：目标是系统分区，还原后需重启\n");
        sysrecover::WimEngine engine;
        if (!engine.ok()) {
            std::printf("wimlib 初始化失败\n");
            return 1;
        }
        int rc = engine.Apply(ToWide(image), index, target.letter + L":/",
                              ConsoleProgress);
        std::printf("\n");
        if (rc != 0) {
            std::wprintf(L"还原失败(rc=%d)：%ls\n", rc,
                         sysrecover::WimEngine::ErrorString(rc));
            sysrecover::ProgressDone("restore", "failed");
            return 1;
        }
        std::printf("还原完成\n");
        sysrecover::ProgressDone("restore", "done");
        return 0;
    }
    // 默认：暂存模式（重启进 Linux 执行；ops 层：安检→_zjresy 日志→
    // 任务文件→引导层→BCD bootsequence，契约对齐旧 C# StageRestoreAsync）
    sysrecover::RestoreRequest req;
    req.image = ToWide(image);
    req.disk = disk;
    req.part = part;
    req.index = index;
    req.repairBoot = repairBoot;
    std::string err;
    bool needReboot = true;
    // BitLocker 提醒（同 GUI 规格）：有加密卷就醒目提示（脚本场景不阻塞，只提示）
    {
        auto bl = sysrecover::BitLockerVolumes();
        if (!bl.empty()) {
            std::string list;
            for (const auto& v : bl) {
                if (!list.empty())
                    list += " ";
                list += W2U8(v);
            }
            std::printf(
                "警告：本机存在 BitLocker 加密卷（%s）——\n"
                "      如果没有密码 / 恢复密钥，还原后这些卷的数据将无法恢复。\n",
                list.c_str());
        }
    }
    int rc = sysrecover::StageRestore(req, err, &needReboot);
    if (rc != 0) {
        std::printf("%s\n", err.c_str());
        std::string advice = sysrecover::ErrorAdvice(rc, err);  // P8
        if (!advice.empty())
            std::printf("%s\n", advice.c_str());
        return rc == 4 ? 4 : (rc == 5 ? 5 : 1);
    }
    if (needReboot)
        std::printf("已暂存还原任务，重启后由引导层执行。\n");
    else
        std::printf("还原已完成（目标分区未被占用，直接就地还原，无需重启）。\n");
    return 0;
}

int CmdVerify(const std::vector<std::string>& a) {
    std::string image = Opt(a, "--image");
    if (image.empty()) {
        std::printf("缺少 --image\n");
        return 2;
    }
    sysrecover::WimEngine engine;
    if (!engine.ok()) {
        std::printf("wimlib 初始化失败\n");
        return 1;
    }
    int rc = engine.Verify(ToWide(image));
    std::printf(rc == 0 ? "校验通过\n" : "校验失败\n");
    return rc == 0 ? 0 : 5;
}

int CmdImages(const std::vector<std::string>& a) {
    std::string file = Opt(a, "--file");
    if (file.empty()) {
        std::printf("缺少 --file\n");
        return 2;
    }
    sysrecover::WimEngine engine;
    if (!engine.ok()) {
        std::printf("wimlib 初始化失败\n");
        return 1;
    }
    std::vector<sysrecover::ImageDesc> list;
    int rc = engine.ListImages(ToWide(file), list);
    if (rc != 0) {
        std::printf("读取失败：%ls\n",
                    sysrecover::WimEngine::ErrorString(rc));
        return 1;
    }
    for (const auto& img : list) {
        // 大小 = 实际占用（TOTALBYTES−HARDLINKBYTES，见 PIT-073）；日期 = FILETIME→本地时间
        wchar_t dateBuf[40] = L"";
        if (img.creationTime) {
            FILETIME ft, lft;
            SYSTEMTIME st;
            ft.dwLowDateTime = (DWORD)(img.creationTime & 0xFFFFFFFFull);
            ft.dwHighDateTime = (DWORD)(img.creationTime >> 32);
            if (FileTimeToLocalFileTime(&ft, &lft) &&
                FileTimeToSystemTime(&lft, &st))
                swprintf(dateBuf, 40, L"%04u-%02u-%02u %02u:%02u", st.wYear,
                         st.wMonth, st.wDay, st.wHour, st.wMinute);
        }
        std::wprintf(L"%d | %ls | %.2f GB | %ls\n", img.index,
                     img.name.c_str(), img.sizeBytes / 1073741824.0,
                     dateBuf[0] ? dateBuf : L"-");
        if (!img.description.empty())
            std::wprintf(L"      %ls\n", img.description.c_str());
    }
    return 0;
}

}  // namespace

int CmdExtract(const std::vector<std::string>& a) {
    std::string file = Opt(a, "--file");
    if (file.empty())
        file = Opt(a, "--image");  // images 用 --file、restore 用 --image，这里两个都收
    std::string dest = Opt(a, "--dest");
    int index = std::atoi(Opt(a, "--index", "1").c_str());
    // --path 可重复（Opt 只取第一个匹配，所以自己扫一遍）
    std::vector<std::wstring> paths;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == "--path")
            paths.push_back(ToWide(a[i + 1]));
    if (file.empty() || dest.empty() || paths.empty()) {
        std::printf(
            "用法: SysRecover.exe extract --file <镜像> [--index N] "
            "--path <镜像内路径> [--path ...] --dest <输出目录>\n"
            "  路径用 Windows 风格、以 \\ 开头，支持通配符，例如：\n"
            "    --path \"\\Windows\\win.ini\"\n"
            "    --path \"\\Users\\*\\Desktop\\*.txt\"\n");
        return 2;
    }
    // 输出目录：不存在就建（父目录需已存在）
    if (!CreateDirectoryW(ToWide(dest).c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
        std::printf("无法创建输出目录（父目录需已存在）\n");
        return 1;
    }
    sysrecover::WimEngine engine;
    if (!engine.ok()) {
        std::printf("wimlib 初始化失败\n");
        return 1;
    }
    int rc = engine.ExtractPaths(ToWide(file), index, paths, ToWide(dest));
    if (rc != 0) {
        std::printf("提取失败：%ls\n", sysrecover::WimEngine::ErrorString(rc));
        return 1;
    }
    std::printf("已提取 %zu 个路径到 %s\n", paths.size(), dest.c_str());
    return 0;
}

// 导出诊断包：diag 文本 + logs/ 下的文件 + 契约文件 + version.json → 一个 zip。
// 用自写的 ZipWriter（store 模式，零依赖；见 src/common/zip.h）。
int CmdDiagZip(const std::vector<std::string>& a) {
    std::wstring exeDir = sysrecover::ExeDir();
    std::wstring logsDir = exeDir + L"\\logs";
    std::wstring outPath;
    std::string outOpt = Opt(a, "--out");
    if (!outOpt.empty()) {
        outPath = ToWide(outOpt);
    } else {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t nm[128];
        swprintf(nm, 128, L"\\SysRecover-diag-%04u%02u%02u-%02u%02u%02u.zip",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond);
        CreateDirectoryW(logsDir.c_str(), nullptr);
        outPath = logsDir + nm;
    }
    sysrecover::ZipWriter zip(outPath);
    if (!zip.ok()) {
        std::printf("无法创建诊断包：%ls\n", outPath.c_str());
        return 1;
    }
    int n = 0;
    {  // 1) diag 文本
        std::string t;
        DiagText(t);
        if (zip.AddData(t.data(), t.size(), "diag.txt"))
            ++n;
    }
    {  // 2) logs 目录下的文件（不递归）
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((logsDir + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                std::wstring full = logsDir + L"\\" + fd.cFileName;
                char u8[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u8,
                                    sizeof(u8), nullptr, nullptr);
                std::string zn = std::string("logs/") + u8;
                if (zip.AddFile(full, zn))
                    ++n;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    {  // 3) 契约文件与版本（有才加）
        const wchar_t* extra[] = {L"\\restore-task.conf", L"\\restore-task.json",
                                  L"\\version.json"};
        for (const wchar_t* f : extra) {
            std::wstring full = exeDir + f;
            if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            char u8[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, f + 1, -1, u8, sizeof(u8), nullptr,
                                nullptr);
            if (zip.AddFile(full, u8))
                ++n;
        }
    }
    if (!zip.Close()) {
        std::printf("写诊断包失败\n");
        return 1;
    }
    std::printf("已导出诊断包（%d 个文件）：%ls\n", n, outPath.c_str());
    return 0;
}

// 历史记录（P7）：打印 logs\history.jsonl 的最后 N 行
int CmdHistory(const std::vector<std::string>& a) {
    int n = std::atoi(Opt(a, "--last", "20").c_str());
    if (n <= 0)
        n = 20;
    std::wstring path = sysrecover::ExeDir() + L"\\logs\\history.jsonl";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("还没有历史记录（备份或还原成功后会写 %ls）\n", path.c_str());
        return 0;
    }
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    std::string all(static_cast<size_t>(sz.QuadPart), 0);
    DWORD got = 0;
    if (!all.empty())
        ReadFile(h, &all[0], static_cast<DWORD>(all.size()), &got, nullptr);
    CloseHandle(h);
    std::vector<std::string> lines;
    for (size_t p = 0; p < all.size();) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos)
            break;
        lines.push_back(all.substr(p, e - p));
        p = e + 1;
    }
    size_t start = lines.size() > static_cast<size_t>(n)
                       ? lines.size() - static_cast<size_t>(n)
                       : 0;
    for (size_t i = start; i < lines.size(); ++i)
        std::printf("%s\n", lines[i].c_str());
    return 0;
}

int CmdList() {    const auto disks = sysrecover::EnumerateDisks();
    const double gb = 1024.0 * 1024 * 1024;
    for (const auto& d : disks) {
        std::wprintf(L"Disk %u  %ls  %.0fGB  %hs%s\n", d.index,
                     d.model.empty() ? L"?" : d.model.c_str(),
                     d.sizeBytes / gb,
                     sysrecover::StyleName(d.style),
                     d.isRemovable ? "  [Removable]" : "");
        for (const auto& p : d.parts) {
            std::wprintf(
                L"  Part %u  %ls  %ls  %.0fGB free %.0fGB%ls%ls%ls\n",
                p.partNumber, p.letter.empty() ? L"-" : p.letter.c_str(),
                p.fs.empty() ? L"?" : p.fs.c_str(), p.sizeBytes / gb,
                p.freeBytes / gb, p.isSystem ? L"  [System]" : L"",
                p.isEsp ? L"  [ESP]" : L"",
                p.isRecovery ? L"  [Recovery]" : L"");
        }
    }
    return 0;
}

int CmdShortcut(const std::vector<std::string>& a) {
    std::string name = Opt(a, "--name", "一键还原");
    std::string target = Opt(a, "--target");
    std::string targetArgs = Opt(a, "--args");
    if (target.empty()) {
        std::printf("缺少 --target <exe路径>\n");
        return 2;
    }
    wchar_t folder[MAX_PATH] = {};
    if (Has(a, "--startmenu")) {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0,
                                    folder))) {
            std::printf("取开始菜单目录失败\n");
            return 1;
        }
    } else {
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOP, nullptr, 0,
                                    folder))) {
            std::printf("取桌面目录失败\n");
            return 1;
        }
    }
    std::wstring out;
    if (!sysrecover::CreateShortcut(folder, ToWide(name), ToWide(target),
                                    ToWide(targetArgs), out)) {
        std::printf("创建快捷方式失败\n");
        return 1;
    }
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, narrow, sizeof(narrow),
                        nullptr, nullptr);
    std::printf("已创建：%s\n", narrow);
    return 0;
}

int Usage() {
    std::printf(
        "Usage: SysRecover.exe "
        "<version|diag|list|backup|restore|verify|images|extract|shortcut> [opts]\n");
    return 2;
}

}  // namespace

int main() {
    // 位数自举：32 位程序在 64 位 Windows 上换成 <ExeDir>\x64\<同名> 再跑
    // （Windows 侧位数跟随系统；见 src/common/selfarch.h）。返回 false = 无需切换。
    {
        int reexecCode = 0;
        if (sysrecover::ReexecX64IfNeeded(&reexecCode))
            return reexecCode;
    }
    // 控制台输出切到 UTF-8（否则中文提示在 GBK 控制台是乱码）；退出时还原原代码页。
    struct CpGuard {
        UINT out;
        ~CpGuard() { SetConsoleOutputCP(out); }
    } cpGuard{GetConsoleOutputCP()};
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args = Utf8Args();
    if (args.empty())
        return Usage();
    std::wstring exeDir = sysrecover::ExeDir();
    std::wstring logsDir = exeDir + L"\\logs";
    sysrecover::LogInit(logsDir);
    sysrecover::ProgressInit(logsDir);
    SetConsoleCtrlHandler(OnCtrl, TRUE);  // Ctrl+C → 干净取消（备份/还原应用）
    std::string cmdline = "cmd:";
    for (const auto& a : args) {
        cmdline += " ";
        cmdline += a;
    }
    sysrecover::LogInfo(cmdline);
    if (args[0] == "version")
        return CmdVersion();
    if (args[0] == "diag") {
        for (const auto& s : args)
            if (s == "--zip")
                return CmdDiagZip(args);  // diag --zip [--out x.zip]
        return CmdDiag();
    }
    if (args[0] == "list")
        return CmdList();
    if (args[0] == "backup")
        return CmdBackup(args);
    if (args[0] == "restore")
        return CmdRestore(args);
    if (args[0] == "verify")
        return CmdVerify(args);
    if (args[0] == "images")
        return CmdImages(args);
    if (args[0] == "extract")
        return CmdExtract(args);
    if (args[0] == "history")
        return CmdHistory(args);
    if (args[0] == "shortcut")
        return CmdShortcut(args);
    return Usage();
}
