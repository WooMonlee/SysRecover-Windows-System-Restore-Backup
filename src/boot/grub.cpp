// GRUB4DOS 部署实现。
#include "grub.h"

#include <windows.h>

#include <cstdio>

#include "bcd.h"
#include "uefi.h"
#include "../disk/disk.h"
#include "../common/process.h"

namespace sysrecover {
namespace {

bool WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return ok && written == utf8.size();
}

bool CopyOne(const std::wstring& src, const std::wstring& dst,
             std::string& log) {
    char s[512] = {}, d[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, s, 512, nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, dst.c_str(), -1, d, 512, nullptr, nullptr);
    char line[1100];
    // 目标若已存在且带只读/隐藏属性，先归零，保证能覆盖（PIT-037 每次重装）
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        snprintf(line, sizeof(line), "copy OK %s\n", s);
        log += line;
        return true;
    }
    snprintf(line, sizeof(line), "copy FAIL %s (err=%lu)\n", s,
             GetLastError());
    log += line;
    return false;
}

// 引导文件平时不该在资源管理器里晃眼（还原完成后会被清掉；失败时也尽量不碍眼）
void Hide(const std::wstring& path) {
    SetFileAttributesW(path.c_str(),
                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
}

// 递归删除文件/目录（先清只读/隐藏属性，否则删不掉）。
bool RemoveTree(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return true;  // 不存在 = 已达成目的
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        return DeleteFileW(path.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 ||
                wcscmp(fd.cFileName, L"..") == 0)
                continue;
            RemoveTree(path + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return RemoveDirectoryW(path.c_str()) != 0;
}

}  // namespace

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

std::wstring FindDataDrive() {
    UINT type = GetDriveTypeW(L"D:\\");
    if (type == DRIVE_FIXED)
        return L"D:\\";
    wchar_t sysDir[MAX_PATH] = {};
    GetWindowsDirectoryW(sysDir, MAX_PATH);
    wchar_t sysLetter = sysDir[0];
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        if (c == sysLetter)
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(root) == DRIVE_FIXED)
            return root;
    }
    return L"";
}

std::wstring SystemDrive() {
    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, MAX_PATH))
        return L"C:\\";
    return std::wstring(winDir).substr(0, 3);  // "C:\"
}

// 部署引导链（PIT-058）：`grldr.mbr` 与 `grldr` 都放在数据盘**根目录**——
// GRUB4DOS 的 grldr.mbr 只会在分区根目录找 grldr（硬限制）。根目录因此有
// grldr + grldr.mbr + menu.lst 三个隐藏文件，还原成功后全部删除。
// 返回成功数（0-2）。
int DeployGrldr(const std::wstring& dataDrive, const std::wstring& exeDir,
                std::string& log) {
    int ok = 0;
    const wchar_t* files[] = {L"grldr.mbr", L"grldr"};
    for (const wchar_t* f : files) {
        std::wstring src = exeDir + L"\\bootfiles\\" + f;
        std::wstring dst = dataDrive + f;
        if (CopyOne(src, dst, log)) {
            Hide(dst);
            ++ok;
        }
    }
    return ok;
}

bool WriteMenuLst(const std::wstring& dataDrive, std::string& log) {
    // 菜单文本必须为 ASCII：GRUB4DOS 显示中文需额外加载字体文件（fontfile +
    // GBK 码表），本项目未随包分发字体，写中文会显示为乱码（PIT-036）。
    const char* content =
        "# SysRecover GRUB4DOS menu (auto-generated)\n"
        "timeout 0\n"
        "default 0\n"
        "\n"
        "title SysRecover Restore Environment\n"
        "kernel /ZJRESTORE/boot/vmlinuz-zjrestore nvme_core.io_timeout=1\n"
        "initrd /ZJRESTORE/boot/initramfs-zjrestore.cpio.gz\n";
    bool ok = WriteTextFile(dataDrive + L"menu.lst", content);
    if (ok)
        Hide(dataDrive + L"menu.lst");
    log += ok ? "write menu.lst OK\n" : "write menu.lst FAIL\n";
    return ok;
}

bool CopyBootFiles(const std::wstring& dataDrive, const std::wstring& exeDir,
                   std::string& log) {
    std::wstring rec = dataDrive + kRecoveryDir;
    std::wstring boot = rec + L"\\boot";
    std::wstring scripts = rec + L"\\scripts";
    CreateDirectoryW(rec.c_str(), nullptr);
    CreateDirectoryW(boot.c_str(), nullptr);
    CreateDirectoryW(scripts.c_str(), nullptr);
    Hide(rec);  // 整个 ZJRESTORE 隐藏，不在资源管理器里晃眼
    bool ok = true;
    ok &= CopyOne(exeDir + L"\\bootfiles\\vmlinuz-zjrestore",
                  boot + L"\\vmlinuz-zjrestore", log);
    ok &= CopyOne(exeDir + L"\\bootfiles\\initramfs-zjrestore.cpio.gz",
                  boot + L"\\initramfs-zjrestore.cpio.gz", log);
    ok &= CopyOne(exeDir + L"\\bootfiles\\zjrestore-lite.sh",
                  scripts + L"\\zjrestore-lite.sh", log);
    return ok;
}

bool NeedsInstall(std::string& detail) {
    char line[256];
    std::wstring drive = SystemDrive();
    bool hasRec = false, hasGrldr = false, hasMenu = false;
    if (!drive.empty()) {
        DWORD a = GetFileAttributesW((drive + kRecoveryDir).c_str());
        hasRec = (a != INVALID_FILE_ATTRIBUTES) &&
                 (a & FILE_ATTRIBUTE_DIRECTORY);
        a = GetFileAttributesW((drive + L"grldr.mbr").c_str());
        hasGrldr = (a != INVALID_FILE_ATTRIBUTES);
        a = GetFileAttributesW((drive + L"menu.lst").c_str());
        hasMenu = (a != INVALID_FILE_ATTRIBUTES);
    }
    bool hasBcd = BcdEntryExists(RecoveryGuid());
    snprintf(line, sizeof(line),
             "recovery=%d grldr.mbr=%d menu.lst=%d bcd=%d\n", (int)hasRec,
             (int)hasGrldr, (int)hasMenu, (int)hasBcd);
    detail = line;
    return !(hasRec && hasGrldr && hasMenu && hasBcd);
}

bool InstallBootLayer(const std::wstring& deployDrive,
                      const std::wstring& exeDir, std::string& log) {
    std::wstring drive = deployDrive;
    if (drive.empty()) {
        log += "no deploy drive\n";
        return false;
    }
    char line[128];
    snprintf(line, sizeof(line), "deploy drive=%lc\n", drive[0]);
    log += line;
    bool ok = true;
    ok &= CopyBootFiles(drive, exeDir, log);
    ok &= (DeployGrldr(drive, exeDir, log) == 2);
    ok &= WriteMenuLst(drive, log);
    std::string bcdLog;
    wchar_t dl = drive[0];
    if (!BcdCreateBootsector(RecoveryGuid(), L"一键还原恢复环境", dl,
                             bcdLog))
        ok = false;
    log += bcdLog;
    return ok;
}

bool RemoveBootLayer(std::string& log) {
    log += "remove boot layer\n";
    // 0) UEFI 固件启动项（Boot#### + BootOrder + BootNext），装了才删
    RemoveUefiBootEntry(log);
    // 1) BCD：删恢复条目 + 清除单次启动
    std::string out;
    RunProcess(SysToolPath(L"bcdedit.exe"),
               L"/delete " + std::wstring(RecoveryGuid()) + L" /f", out);
    log += "delete entry: " + out + "\n";
    RunProcess(SysToolPath(L"bcdedit.exe"),
               L"/deletevalue {bootmgr} bootsequence", out);
    // 2) 引导文件可能落在系统盘（新布局）或数据盘（旧布局）——两处都清
    bool ok = true;
    const std::wstring drives[] = {SystemDrive(), FindDataDrive()};
    for (const std::wstring& drive : drives) {
        if (drive.empty())
            continue;
        ok &= RemoveTree(drive + kRecoveryDir);
        ok &= RemoveTree(drive + L"grldr");
        ok &= RemoveTree(drive + L"grldr.mbr");
        ok &= RemoveTree(drive + L"menu.lst");
    }
    log += ok ? "removed files OK\n" : "removed some files FAILED\n";
    // 3) UEFI：ESP 上的常驻救援文件 <ESP>\EFI\ZJRESTORE（约 51MB）
    if (IsUefiFirmware()) {
        std::wstring espRoot = MountEsp(log);
        if (!espRoot.empty()) {
            RemoveTree(espRoot + L"EFI\\ZJRESTORE");
            UnmountEsp(espRoot, log);
            log += "removed ESP rescue dir\n";
        }
    }
    return ok;
}

// ── UEFI（GPT）分支 ──────────────────────────────────────────────
std::wstring MountEsp(std::string& log) {
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'S'; c <= L'Z'; ++c) {
        if (mask & (1u << (c - L'A')))
            continue;
        wchar_t root[4] = {c, L':', L'\\', 0};
        std::string out;
        std::wstring args = std::wstring(1, c) + L": /s";
        if (RunProcess(SysToolPath(L"mountvol.exe"), args, out) != 0)
            continue;
        if (GetFileAttributesW(root) == INVALID_FILE_ATTRIBUTES)
            continue;
        char line[64];
        snprintf(line, sizeof(line), "ESP mounted at %lc:\\\n", c);
        log += line;
        return root;
    }
    log += "mount ESP failed (mountvol X: /s)\n";
    return L"";
}

void UnmountEsp(const std::wstring& espRoot, std::string& log) {
    if (espRoot.size() < 2)
        return;
    std::string out;
    std::wstring args = espRoot.substr(0, 2) + L" /d";
    RunProcess(SysToolPath(L"mountvol.exe"), args, out);
    log += "ESP unmounted\n";
}

}  // namespace sysrecover
