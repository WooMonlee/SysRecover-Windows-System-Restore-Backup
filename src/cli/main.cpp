// SysRecover CLI（Phase 0）：version / diag。完整命令见 AGENTS.md §9。
// 退出码：0 成功，1 通用失败，2 参数错误（与 AGENTS.md §9 一致）。
#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "wimlib.h"
#include "../common/version.h"
#include "../app/ops.h"
#include "../app/safety.h"
#include "../app/shortcut.h"
#include "../boot/bcd.h"
#include "../boot/grub.h"
#include "../boot/task.h"
#include "../boot/uefi.h"
#include "../common/logger.h"
#include "../common/progress.h"
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

int CmdDiag() {
    std::printf("[diag] admin=%s\n", IsAdmin() ? "yes" : "no");
    std::printf("[diag] firmware=%s\n",
                sysrecover::IsUefiFirmware() ? "UEFI" : "BIOS");
    if (sysrecover::IsUefiFirmware()) {
        std::printf("[diag] secureboot=%s\n",
                    sysrecover::IsSecureBootEnabled() ? "ON (需签名引导)" : "off");
        std::string detail;
        bool installed = sysrecover::UefiBootEntryExists(detail);
        std::printf("[diag] uefi boot entry=%s (%s)\n",
                    installed ? "installed" : "not installed", detail.c_str());
    }
    int rc = wimlib_global_init(0);
    std::printf("[diag] wimlib_global_init -> %d (%s)\n", rc,
                rc == 0 ? "OK" : "FAIL");
    if (rc == 0)
        wimlib_global_cleanup();
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

std::string Opt(const std::vector<std::string>& a, const char* key,
                const std::string& def = "") {
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
    int rc = sysrecover::StageRestore(req, err, &needReboot);
    if (rc != 0) {
        std::printf("%s\n", err.c_str());
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

int CmdList() {
    const auto disks = sysrecover::EnumerateDisks();
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

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
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
    if (args[0] == "diag")
        return CmdDiag();
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
    if (args[0] == "shortcut")
        return CmdShortcut(args);
    return Usage();
}
