// 引导补全实现（BIOS/MBR）。见 bootfix.h 与 AGENTS.md §7。
#include "bootfix.h"

#include <windows.h>

#include <cstdio>

#include "../common/process.h"
#include "bcd.h"

namespace sysrecover {
namespace {

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr,
                                nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr,
                            nullptr);
    return s;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDir(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// 递归复制目录（只做文件与子目录；不处理 ACL，引导文件不需要）。
// skipBcd=true 时跳过 BCD/BCD.LOG*（运行中被锁，另用 bcdedit /export 生成）。
bool CopyTree(const std::wstring& src, const std::wstring& dst,
              std::string& log, bool skipBcd) {
    if (!IsDir(src))
        return false;
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    std::wstring pat = src + L"\\*";
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        std::wstring d = dst + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok &= CopyTree(s, d, log, skipBcd);
        } else if (skipBcd && (_wcsicmp(fd.cFileName, L"BCD") == 0 ||
                               _wcsnicmp(fd.cFileName, L"BCD.LOG", 7) == 0)) {
            // 运行中的 Windows 把 BCD 当注册表 hive 挂着（HKLM\BCD00000000），
            // 文件被独占 → CopyFile 必失败。跳过，稍后用 bcdedit /export 生成。
        } else if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
            log += "copy FAIL " + W2U(s) + " (err=" +
                   std::to_string(GetLastError()) + ")\n";
            ok = false;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

bool RunBcdEdit(const std::wstring& store, const std::wstring& args,
                std::string& log) {
    std::string out;
    std::wstring cmd = L"/store \"" + store + L"\" " + args;
    int rc = RunProcess(SysToolPath(L"bcdedit.exe"), cmd, out);
    if (rc != 0)
        log += "bcdedit " + W2U(args) + " rc=" + std::to_string(rc) + ": " + out + "\n";
    else
        log += "bcdedit " + W2U(args) + " OK\n";
    return rc == 0;
}

}  // namespace

bool PrepareBootFixFiles(const std::wstring& recoveryDir, std::string& log) {
    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH)) {
        log += "no windows dir\n";
        return false;
    }
    std::wstring win = winDir;                  // C:\Windows
    std::wstring sysRoot = win.substr(0, 3);    // 系统分区根 "C:\"
    std::wstring srcBootDir = sysRoot + L"Boot";
    std::wstring srcBcd = srcBootDir + L"\\BCD";

    // 本机没有 BIOS 启动文件（如 UEFI 安装）→ 先用 bcdboot 在系统分区补一份。
    if (!Exists(srcBcd)) {
        log += "no C:\\Boot\\BCD -> bcdboot\n";
        std::string out;
        RunProcess(SysToolPath(L"bcdboot.exe"),
                   win + L" /s " + sysRoot.substr(0, 2) + L" /f BIOS", out);
        log += out;
    }

    std::wstring bf = recoveryDir + L"\\bootfix";
    CreateDirectoryW(recoveryDir.c_str(), nullptr);
    CreateDirectoryW(bf.c_str(), nullptr);

    bool ok = true;
    if (Exists(sysRoot + L"bootmgr")) {
        ok &= CopyFileW((sysRoot + L"bootmgr").c_str(),
                        (bf + L"\\bootmgr").c_str(), FALSE) != 0;
    } else {
        log += "no \\bootmgr\n";
        ok = false;
    }
    // 复制 \Boot 下的其它文件（字体/资源/memtest 等），跳过被锁的 BCD。
    if (!CopyTree(srcBootDir, bf + L"\\Boot", log, /*skipBcd=*/true)) {
        log += "copy \\Boot FAIL\n";
        ok = false;
    }

    // BCD：不能直接复制（运行时被锁），用官方导出得到一份完整 store，
    // 再把设备改成可移植的 "boot"（= bootmgr 所在分区），这样 apply 到任意
    // 分区都能启动，不依赖原机的 disk/partition 引用。
    std::wstring store = bf + L"\\Boot\\BCD";
    {
        std::string out;
        std::wstring cmd = L"/export \"" + store + L"\"";
        int rc = RunProcess(SysToolPath(L"bcdedit.exe"), cmd, out);
        if (rc != 0) {
            log += "bcdedit /export rc=" + std::to_string(rc) + ": " + out + "\n";
            ok = false;
        } else {
            log += "bcdedit /export OK\n";
        }
    }
    if (Exists(store)) {
        ok &= RunBcdEdit(store, L"/set {bootmgr} device boot", log);
        ok &= RunBcdEdit(store, L"/set {bootmgr} path \\bootmgr", log);
        // 启动菜单不等待（用户要求：没必要停留）
        RunBcdEdit(store, L"/set {bootmgr} timeout 0", log);
        ok &= RunBcdEdit(store, L"/set {default} device boot", log);
        ok &= RunBcdEdit(store, L"/set {default} osdevice boot", log);
        ok &= RunBcdEdit(store, L"/set {default} path \\Windows\\system32\\winload.exe", log);
        // 内存诊断项在目标机上通常没有 memtest.exe，删掉免得菜单里点到报错
        RunBcdEdit(store, L"/delete {memdiag} /f", log);
        // 清掉单次启动序列：export 时若暂存阶段已设 bootsequence，会被一并带上，
        // 目标机重启后又进 GRUB4DOS 恢复环境（"还原完成却回到 Linux"的元凶之一）。
        RunBcdEdit(store, L"/deletevalue {bootmgr} bootsequence", log);
        // 删掉 GRUB4DOS 恢复条目（export 连它一起导出了）：先摘 displayorder 引用，
        // 再删条目。否则目标机多出一个无效恢复菜单项（device=partition=D: 在目标
        // 机上不存在），BCD 有多个条目时 timeout=0 仍可能弹菜单。
        RunBcdEdit(store,
                   L"/displayorder " + std::wstring(RecoveryGuid()) + L" /remove",
                   log);
        RunBcdEdit(store, L"/delete " + std::wstring(RecoveryGuid()) + L" /f",
                   log);
    } else {
        log += "no bootfix BCD\n";
        ok = false;
    }
    log += ok ? "bootfix prepared\n" : "bootfix INCOMPLETE\n";
    return ok;
}

}  // namespace sysrecover
