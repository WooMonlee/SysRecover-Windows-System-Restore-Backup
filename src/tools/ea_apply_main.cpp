// zj-ea-apply.exe — 首启 EA 补写器（PIT-122/123，2026-10-06）
//
// 由本地组策略「启动脚本」在**登录前、SYSTEM 身份**拉起（QEMU 全链实测）。
// 无第三方依赖（不链接 wimlib）：读 eapack.dat → NtSetEaFile 把 EA 写回 →
// 成功后自清理（策略/脚本/包/自身），失败保留以便下次引导重试（最多 3 次）。
// 日志：<SystemDrive>\ZJRESTORE\ea\ea-apply.log（UTF-8）。
#define _WIN32_WINNT 0x0601
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "../common/ea.h"
#include "../common/refscan.h"
#include "../common/version.h"

using namespace sysrecover;

namespace {

std::wstring g_logPath;

void logLine(const std::string& s) {
    if (g_logPath.empty()) return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char ts[48];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    // 裸字节追加（不走 CRT 的 ccs 模式：实测 `_wfopen("a, ccs=UTF-8")` 会把
    // 文本吃掉、只留 CRLF 的 UTF-8 形态 —— U+0A0D 乱码）。
    std::string out = std::string(ts) + s + "\r\n";
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(h, out.data(), (DWORD)out.size(), &wr, nullptr);
        CloseHandle(h);
    }
}

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? (char)c : '?';
    return s;
}

int ReadAttempts(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    return atoi(buf);
}

void WriteAttempts(const std::wstring& path, int n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", n);
    DWORD wr = 0;
    WriteFile(h, buf, (DWORD)len, &wr, nullptr);
    CloseHandle(h);
}

// ── 残留归纳（PIT-156，2026-10-18）的解析/复制助手 ──

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {  // 非 UTF-8（理论上不该发生）→ 按 ANSI 兜底
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), nullptr, 0);
    }
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(cp, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool ReadAllBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz > (1u << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(sz);
    DWORD rd = 0;
    BOOL ok = (sz == 0) ? TRUE : ReadFile(h, &out[0], sz, &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(rd);
    return true;
}

// 在 source.txt 文本里找 "<key>=<值>"（值到行尾；UTF-8 → 宽串）。
std::wstring SourceField(const std::string& text, const char* key) {
    const std::string pre = std::string(key) + "=";
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, pre.size(), pre) == 0)
            return Utf8ToWide(line.substr(pre.size()));
        pos = eol + 1;
    }
    return {};
}

bool CopyFileRaw(const std::wstring& src, const std::wstring& dst) {
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);  // PIT-067/068
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

// 把 <目标>\ZJRESTORE 里的诊断文本复制到 <软件目录>\logs\collected 下的
// ZJRESTORE-ea 子目录（固定目录名、覆盖写 —— 失败重试不增殖）。失败只记日志。
// **不删** ZJRESTORE 文件夹本身（菜单安装时 boot/scripts 在里面；删除由
// Windows 侧 ClassifyZjrResidue 决策）。
void GatherResidueToSoftwareDir(const std::wstring& root) {
    const std::wstring zjr = root + L"\\ZJRESTORE";
    const std::wstring srcFile = zjr + L"\\source.txt";
    std::string text;
    if (!ReadAllBytes(srcFile, text)) {
        logLine("gather: no source.txt (nothing to gather)");
        return;
    }
    std::wstring soft = SourceField(text, "software_dir");
    if (soft.empty()) {
        // 回退：software_path 去掉文件名（与救援脚本同一规则）
        std::wstring sp = SourceField(text, "software_path");
        size_t slash = sp.find_last_of(L"\\/");
        if (slash != std::wstring::npos) soft = sp.substr(0, slash);
    }
    if (soft.empty()) {
        logLine("gather: software_dir unknown in source.txt");
        return;
    }
    if (GetFileAttributesW(soft.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logLine("gather: software dir missing: " + Ascii(soft));
        return;
    }
    const std::wstring dest = soft + L"\\logs\\collected\\ZJRESTORE-ea";
    std::wstring cur = soft;  // CreateDirectoryW 不递归 → 逐级建
    for (const wchar_t* part : {L"logs", L"collected", L"ZJRESTORE-ea"}) {
        cur += L"\\";
        cur += part;
        CreateDirectoryW(cur.c_str(), nullptr);
    }
    if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logLine("gather: cannot create dest (logs stay on target)");
        return;
    }
    int copied = 0;
    if (CopyFileRaw(srcFile, dest + L"\\source.txt")) ++copied;
    // ea\ 下的文本日志（跳过二进制载荷 *.exe/*.dat 与 >8MB 的文件）
    WIN32_FIND_DATAW fd = {};
    const std::wstring ea = zjr + L"\\ea";
    HANDLE h = FindFirstFileW((ea + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring low = fd.cFileName;
            for (auto& ch : low) ch = (wchar_t)towlower(ch);
            auto ends = [&](const wchar_t* suf) {
                size_t n = wcslen(suf);
                return low.size() > n &&
                       low.compare(low.size() - n, n, suf) == 0;
            };
            if (ends(L".exe") || ends(L".dat")) continue;
            if (fd.nFileSizeHigh != 0 || fd.nFileSizeLow > (8u << 20)) continue;
            if (CopyFileRaw(ea + L"\\" + fd.cFileName, dest + L"\\" + fd.cFileName))
                ++copied;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    logLine("gather: copied " + std::to_string(copied) +
            " file(s) -> " + Ascii(dest));
}

// 成功后的残留清理（用户 2026-10-20 规格）：<目标>\ZJRESTORE 不管是什么，
// 诊断先由 GatherResidueToSoftwareDir 归集到软件目录 logs，随后整目录删除，
// 让目标盘根目录保持干净。保护两类（功能必须保留）：
//   · 目录里有 SysRecover(.UI).exe → "程序的家"（SettleHome 副本）→ 不动；
//   · 目录里有 boot/scripts → 已安装启动菜单的救援载荷 → 不动。
// 正在运行的补写器 exe/cmd 不能当场删 → 起分离 cmd 等本进程退出后反复 rmdir。
void RemoveResidueFolder(const std::wstring& root) {
    const std::wstring zjr = root + L"\\ZJRESTORE";
    if (GetFileAttributesW((zjr + L"\\SysRecover.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((zjr + L"\\SysRecoverUI.exe").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
        logLine("residue: program home detected, keep");
        return;
    }
    if (GetFileAttributesW((zjr + L"\\boot").c_str()) !=
            INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW((zjr + L"\\scripts").c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
        logLine("residue: installed boot menu detected, keep");
        return;
    }
    std::wstring cmdline =
        L"cmd.exe /c ping -n 5 127.0.0.1 >nul & for /l %i in (1,1,30) do "
        L"@(rmdir /s /q \"" + zjr + L"\" 2>nul & if not exist \"" + zjr +
        L"\" exit /b)";
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, mut.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                       &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        logLine("residue: cleanup scheduled");
    } else {
        logLine("residue: cleanup spawn FAILED err=" +
                std::to_string(GetLastError()));
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // ⚠️ WOW64 文件系统重定向（PIT-082 家族坑，2026-10-06 实测踩到）：
    // 本 exe 是 **32 位**（x86/x64 目标都要能跑），在 64 位 Windows 上访问
    // `C:\Windows\System32\...` 会被**重定向到 SysWOW64** —— 组策略文件在真实的
    // System32\GroupPolicy（SysWOW64 里没有）→ 清理全部静默跳过；EA 目标若在
    // System32 下也会写错文件。进程级关掉重定向（本程序不需要它）。
    PVOID oldRedir = nullptr;
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
        using Wow64DisableFn = BOOL(WINAPI*)(PVOID*);
        // 两步 cast（经 void*）规避 -Wcast-function-type
        auto fn = reinterpret_cast<Wow64DisableFn>(reinterpret_cast<void*>(
            GetProcAddress(k32, "Wow64DisableWow64FsRedirection")));
        if (fn) fn(&oldRedir);
    }
    std::wstring pack;
    std::wstring rootOverride;
    bool noCleanup = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--pack") && i + 1 < argc) {
            pack = argv[++i];
        } else if (!wcscmp(argv[i], L"--root") && i + 1 < argc) {
            rootOverride = argv[++i];  // 实验室/自测：EA 路径按此根解析
        } else if (!wcscmp(argv[i], L"--no-cleanup")) {
            noCleanup = true;  // 实验室/自测：跳过钩子清理与自删
        }
    }
    std::wstring root;
    if (!rootOverride.empty()) {
        root = rootOverride;
        while (!root.empty() &&
               (root.back() == L'/' || root.back() == L'\\'))
            root.pop_back();
    } else {
        wchar_t winDir[MAX_PATH] = {};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        root.assign(winDir, wcslen(winDir) >= 2 ? 2 : wcslen(winDir));  // "C:"
    }
    const std::wstring eaDir = root + L"\\ZJRESTORE\\ea";
    CreateDirectoryW(eaDir.c_str(), nullptr);
    if (pack.empty()) pack = eaDir + L"\\eapack.dat";
    const bool havePack =
        GetFileAttributesW(pack.c_str()) != INVALID_FILE_ATTRIBUTES;

    g_logPath = eaDir + L"\\ea-apply.log";
    logLine("ea-apply start build=" SYSRECOVER_VERSION);
    // 诊断：本进程在"登录前"还是"登录后"跑（Shell_TrayWnd = 任务栏）
    logLine(std::string("shell_started=") +
            (FindWindowW(L"Shell_TrayWnd", nullptr) ? "yes" : "no (pre-logon)"));

    // ── 非微软重解析点写回（PIT-136，用户 2026-10-08 规格）──
    // 备份时打包（reppack.dat，主镜像已排除该文件）；本进程（SYSTEM，登录前）
    // 用原生 FSCTL_SET_REPARSE_POINT 写回 —— Linux 侧 wimlib 写不了这类文件
    // （libntfs-3g 已知 bug），Windows 原生 API 则任何 tag 都能设。
    // ⚠️ 必须在 ApplyPack **之前**（1019 案修订）：被排除的重解析点文件由本步
    // 重建，它们的 EA 在 eapack 里 —— 先 pack 会把它们记成 missing（客户 e2e
    // 实测 missing=1 即此类）。
    {
        const std::wstring repPack = eaDir + L"\\reppack.dat";
        if (GetFileAttributesW(repPack.c_str()) != INVALID_FILE_ATTRIBUTES) {
            logLine("reppack: applying");
            unsigned long long rok = 0, rbad = 0;
            int rrc = ea::ApplyRepack(repPack, root, &rok, &rbad, logLine);
            char rsum[128];
            snprintf(rsum, sizeof(rsum),
                     "reppack: done ok=%llu failed=%llu rc=%d", rok, rbad,
                     rrc);
            logLine(rsum);
        } else {
            logLine("reppack: none");
        }
    }

    int rc = 0;
    if (havePack) {
        logLine("pack=" + Ascii(pack));
        ea::ApplyStats st;
        rc = ea::ApplyPack(pack, root, st, logLine);
        char sum[256];
        snprintf(sum, sizeof(sum),
                 "result files=%llu ok=%llu missing=%llu failed=%llu lastStatus=0x%08lX",
                 st.files, st.ok, st.missing, st.failed, st.lastStatus);
        logLine(sum);
        if (!st.firstErrors.empty())
            logLine(std::string("error samples:\r\n") + st.firstErrors);
        // 结果一行（支持包/收集器直接读）
        SYSTEMTIME stw = {};
        GetLocalTime(&stw);
        char line[256];
        int len = snprintf(line, sizeof(line),
                           "time=%04d-%02d-%02d %02d:%02d:%02d ok=%llu missing=%llu failed=%llu rc=%d\r\n",
                           stw.wYear, stw.wMonth, stw.wDay, stw.wHour, stw.wMinute,
                           stw.wSecond, st.ok, st.missing, st.failed, rc);
        std::vector<unsigned char> one(line, line + len);
        ea::WriteBytes(eaDir + L"\\ea-result.txt", one);
        if (rc != 0) {
            const std::wstring attemptsPath = eaDir + L"\\attempts.txt";
            int n = ReadAttempts(attemptsPath) + 1;
            WriteAttempts(attemptsPath, n);
            logLine("failed; attempt " + std::to_string(n) + "/3");
            if (n >= 3) {
                std::wstring failed = pack + L".failed";
                if (MoveFileExW(pack.c_str(), failed.c_str(),
                                MOVEFILE_REPLACE_EXISTING))
                    logLine("give up after 3 attempts; pack renamed .failed");
                else
                    logLine("give up after 3 attempts; rename FAILED");
            } else {
                logLine("keeping files for retry on next boot");
            }
        }
    } else {
        // 无 EA 包（纯"悬空引用清理"任务）：EA 补写跳过，继续做引用清理。
        logLine("no pack (EA apply skipped)");
    }

    // ── 悬空引用清理（PIT-132）──
    // 注册表引用指向"备份排除的易失目录"（如 Temp）且文件已缺失 → 还原后必然
    // 损坏（实测：豆包便携版右键扩展 DLL 在 Temp → 每次右键卡死）。保守规则：
    // 只删"易失目录 + 文件缺失"的项；删前落 refs-backup.txt 供恢复。
    {
        logLine("refs: scan start");
        std::string backupText;
        refscan::CleanStats cs = refscan::CleanAllDanglingRefs(
            logLine, backupText, root + L"\\Users");
        char line[192];
        snprintf(line, sizeof(line),
                 "refs: done found=%d cleaned=%d failed=%d at-risk=%d",
                 cs.found, cs.cleaned, cs.failed, cs.atRisk);
        logLine(line);
        if (!backupText.empty())
            ea::WriteBytes(eaDir + L"\\refs-backup.txt",
                           std::vector<unsigned char>(backupText.begin(),
                                                      backupText.end()));
        SYSTEMTIME stw = {};
        GetLocalTime(&stw);
        char rline[240];
        int len = snprintf(rline, sizeof(rline),
                           "time=%04d-%02d-%02d %02d:%02d:%02d found=%d cleaned=%d failed=%d atRisk=%d\r\n",
                           stw.wYear, stw.wMonth, stw.wDay, stw.wHour, stw.wMinute,
                           stw.wSecond, cs.found, cs.cleaned, cs.failed,
                           cs.atRisk);
        ea::WriteBytes(eaDir + L"\\refs-result.txt",
                       std::vector<unsigned char>(rline, rline + len));
    }

    // ── 残留归纳（PIT-156，2026-10-18）──
    // 救援层在 <目标>\ZJRESTORE\source.txt 留了软件原始路径；这里把标记 + ea
    // 诊断文本复制进 <软件目录>\logs\collected\ZJRESTORE-ea\（首启自动一次）。
    // 无论 rc 都做（失败时的日志更值得归纳）。不删文件夹（见上方注释）。
    GatherResidueToSoftwareDir(root);
    // 用户 2026-10-20 规格：成功后目标盘不留 ZJRESTORE（诊断已归集到软件目录
    // logs\collected\ZJRESTORE-ea）；失败保留以便重试。
    if (rc == 0 && !noCleanup)
        RemoveResidueFolder(root);

    if (rc == 0) {
        if (noCleanup) {
            logLine("cleanup skipped (--no-cleanup)");
        } else {
            logLine("cleanup start");
            ea::CleanupHookFiles(root, logLine);
            logLine("cleanup done");
        }
    }
    logLine("ea-apply end rc=" + std::to_string(rc));
    return rc;
}
