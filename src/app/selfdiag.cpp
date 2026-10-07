#include "selfdiag.h"

#include <windows.h>
#include <shlobj.h>  // SHGetFolderPathW（桌面路径）

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <vector>

#include "wimlib.h"
#include "../boot/grub.h"  // MountEsp / UnmountEsp（ESP 日志收集）
#include "../boot/uefi.h"
#include "../common/crash.h"  // WriteProcessMiniDump / FindProcessIdsByName
#include "../common/i18n.h"
#include "../common/logger.h"
#include "../common/process.h"
#include "../common/refscan.h"
#include "../common/relocate.h"
#include "../common/rescue_decision.h"  // DecideRescueReport（纯逻辑，单测覆盖）
#include "../common/selfarch.h"
#include "../common/version.h"
#include "../common/zip.h"
#include "../disk/disk.h"

namespace {

const unsigned long long kCopyCapBytes = 32ull << 20;  // 单文件搬运上限（大件只进清单）

std::string ToUtf8(const std::wstring& w) {
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

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
    return ok && wr == data.size();
}

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

// 复制单个文件进收集目录：覆盖前/后都归零属性（grldr/menu.lst 是 Hidden+System，
// 别把隐藏属性带进日志，否则用户打包时看不见）。
bool CopyOneFile(const std::wstring& src, const std::wstring& dst) {
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
        return false;
    SetFileAttributesW(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
    return true;
}

// 递归搬运小目录（bootfix/scripts/logs）：>32MB 的文件跳过（只进清单）。
int CopySmallTree(const std::wstring& src, const std::wstring& dst, int depth) {
    if (depth > 6)
        return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    int n = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring s = src + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring d = dst + L"\\" + fd.cFileName;
            CreateDirectoryW(d.c_str(), nullptr);
            n += CopySmallTree(s, d, depth + 1);
        } else {
            LARGE_INTEGER li{};
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            if ((unsigned long long)li.QuadPart > kCopyCapBytes)
                continue;
            CreateDirectoryW(dst.c_str(), nullptr);
            if (CopyOneFile(s, dst + L"\\" + fd.cFileName))
                ++n;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// 目录清单（内核/initramfs 等大件不搬，但要让排查者知道"文件在不在、多大"）。
void ListingRec(const std::wstring& dir, const std::wstring& rel,
                std::string& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::wstring child = rel + fd.cFileName;
        if (isDir) {
            out += "[D] " + ToUtf8(child) + "\r\n";
            ListingRec(dir + L"\\" + fd.cFileName, child + L"\\", out);
        } else {
            LARGE_INTEGER li{};
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            char sz[32];
            snprintf(sz, sizeof(sz), " %llu", (unsigned long long)li.QuadPart);
            out += "    " + ToUtf8(child) + sz + "\r\n";
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int CollectOneDrive(wchar_t letter, const std::wstring& dstBase) {
    wchar_t rootBuf[5] = {letter, L':', L'\\', 0};
    std::wstring root = rootBuf;
    if (GetDriveTypeW(rootBuf) != DRIVE_FIXED)
        return 0;
    int n = 0;
    // 目录名带物理身份（_dNpM）：PE 与正常 Windows 盘符不一致（客户支持包里
    // collected\C 与 list.txt 的盘符互相打架）——配合 drive-map.txt 就能对上号。
    std::wstring identity;
    for (const auto& d : sysrecover::EnumerateDisks()) {
        for (const auto& p : d.parts) {
            if (!p.letter.empty() && towupper(p.letter[0]) == towupper(letter)) {
                identity = L"_d" + std::to_wstring(d.index) + L"p" +
                           std::to_wstring(p.partNumber);
                break;
            }
        }
        if (!identity.empty())
            break;
    }
    std::wstring dst = dstBase + L"\\" + std::wstring(1, letter) + identity;

    // 1) 盘根上我们放的文件（BIOS 链 + 契约 + 引导期日志 + 黑匣子三件套）
    const wchar_t* names[] = {L"grldr",         L"grldr.mbr",
                              L"menu.lst",       L"restore-task.conf",
                              L"restore-task.json", L"zjrestore-boot.log",
                              L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
                              L"ZJRESTORE-status.txt"};
    for (const wchar_t* nm : names) {
        std::wstring s = root + nm;
        if (GetFileAttributesW(s.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        CreateDirectoryW(dst.c_str(), nullptr);
        if (CopyOneFile(s, dst + L"\\" + nm))
            ++n;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"_zjresy*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            CreateDirectoryW(dst.c_str(), nullptr);
            if (CopyOneFile(root + fd.cFileName,
                            dst + L"\\" + fd.cFileName))
                ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // 2) <盘>\ZJRESTORE 里的补全包/救援脚本/旧日志 + 整树清单。
    //    ⚠️ 若这个 ZJRESTORE 就是当前日志根（软件在系统盘时 = <数据盘>\ZJRESTORE），
    //    必须跳过 —— 否则把 logs 自己搬进 logs\collected，会越滚越大。
    std::wstring zjr = root + L"ZJRESTORE";
    if (GetFileAttributesW(zjr.c_str()) != INVALID_FILE_ATTRIBUTES &&
        _wcsicmp(zjr.c_str(), sysrecover::LogBaseDir().c_str()) != 0) {
        std::wstring zdst = dst + L"\\ZJRESTORE";
        CreateDirectoryW(zdst.c_str(), nullptr);
        for (const wchar_t* sub : {L"bootfix", L"scripts", L"logs", L"ea"})
            n += CopySmallTree(zjr + L"\\" + sub, zdst + L"\\" + sub, 0);
        // 救援构建戳（A6）：核对盘上救援是哪一版
        std::wstring rb = zjr + L"\\rescue-build.txt";
        if (GetFileAttributesW(rb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryW(zdst.c_str(), nullptr);
            if (CopyOneFile(rb, zdst + L"\\rescue-build.txt"))
                ++n;
        }
        std::string listing;
        ListingRec(zjr, L"ZJRESTORE\\", listing);
        if (WriteUtf8File(dst + L"\\ZJRESTORE-listing.txt", listing))
            ++n;
    }

    // 3) 兜底日志窝：软件目录找不到时救援层把日志写到 <盘根>\ZJRESTORE-logs\
    //    （用户 2026-10-04 规格 3/4：先保证有地方放；收集后再从该分区清掉）。
    std::wstring fbr = root + L"ZJRESTORE-logs";
    if (GetFileAttributesW(fbr.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring fdst = dst + L"\\ZJRESTORE-logs";
        CreateDirectoryW(fdst.c_str(), nullptr);
        n += CopySmallTree(fbr, fdst, 0);
    }
    return n;
}

// ESP（UEFI 无盘符）：临时 mountvol 挂上，收 \EFI\ZJRESTORE\logs\*（黑匣子回执
// 与完整日志）以及意外落在 ESP 根的黑匣子三件套。失败静默返回 0。
int CollectEspFiles(const std::wstring& dstBase) {
    std::string log;
    std::wstring esp = sysrecover::MountEsp(log);
    bool temp = !esp.empty();
    if (esp.empty()) {
        sysrecover::PartitionInfo fb;
        if (sysrecover::FindEspPartitionFallback(fb) && !fb.letter.empty())
            esp = fb.letter + L":\\";
    }
    if (esp.empty())
        return 0;
    int n = 0;
    std::wstring logs = esp + L"EFI\\ZJRESTORE\\logs";
    if (GetFileAttributesW(logs.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring dst = dstBase + L"\\ESP_ZJRESTORE";
        CreateDirectoryW(dst.c_str(), nullptr);
        n += CopySmallTree(logs, dst, 0);
        std::string listing;
        ListingRec(esp + L"EFI\\ZJRESTORE", L"EFI\\ZJRESTORE\\", listing);
        if (WriteUtf8File(dstBase + L"\\ESP_ZJRESTORE-listing.txt", listing))
            ++n;
    }
    for (const wchar_t* nm : {L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
                              L"ZJRESTORE-status.txt"}) {
        std::wstring s = esp + nm;
        if (GetFileAttributesW(s.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        std::wstring dst = dstBase + L"\\ESP";
        CreateDirectoryW(dst.c_str(), nullptr);
        if (CopyOneFile(s, dst + L"\\" + nm))
            ++n;
    }
    if (temp)
        sysrecover::UnmountEsp(esp, log);
    return n;
}

// ── 支持包（「日志」按钮 / `support` 命令）用的小工具 ─────────────────────
const unsigned long long kZipFileCap = 64ull << 20;  // 单文件上限（防意外巨物）

std::wstring ZipStamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t b[32] = {};
    swprintf(b, 32, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return b;
}

bool RemoveTreeAny(const std::wstring& dir) {
    std::wstring pat = dir + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTreeAny(p);
            } else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
    return true;
}

// 递归收进 zip；skipFull = 输出包自身（避免把正在写的 zip 打进自己）。
int ZipAddTree(sysrecover::ZipWriter& zip, const std::wstring& dir,
               const std::wstring& prefix, int depth,
               const std::wstring& skipFull, int& added) {
    if (depth < 0)
        return added;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return added;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (!skipFull.empty() &&
            _wcsicmp(full.c_str(), skipFull.c_str()) == 0)
            continue;
        std::wstring childPrefix =
            prefix.empty() ? std::wstring(fd.cFileName)
                           : prefix + L"/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ZipAddTree(zip, full, childPrefix, depth - 1, skipFull, added);
        } else {
            LARGE_INTEGER li = {};
            li.LowPart = fd.nFileSizeLow;
            li.HighPart = fd.nFileSizeHigh;
            if ((unsigned long long)li.QuadPart > kZipFileCap)
                continue;
            if (zip.AddFile(full, ToUtf8(childPrefix)))
                ++added;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return added;
}

// 事件日志 → 文本（Application Hang/Error、System 存储错误、System 错误级）。
// wevtutil 在 PE 里可能缺失 → 返回 false，调用方照常出包。
bool ExportEventLogsTo(const std::wstring& dir, int& n) {
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring wevt = sysrecover::SysToolPath(L"wevtutil.exe");
    if (GetFileAttributesW(wevt.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    struct Q { const wchar_t* file; const wchar_t* args; };
    const Q q[] = {
        {L"application-hang.txt",
         L"qe Application \"/q:*[System[Provider[@Name='Application Hang']]]\" "
         L"/f:text /rd:true /c:50"},
        {L"application-error.txt",
         L"qe Application \"/q:*[System[Provider[@Name='Application Error']]]\" "
         L"/f:text /rd:true /c:100"},
        {L"system-storage.txt",
         L"qe System \"/q:*[System[Provider[@Name='Ntfs' or @Name='disk' or "
         L"@Name='volmgr' or @Name='volsnap' or @Name='storahci' or "
         L"@Name='stornvme']]]\" /f:text /rd:true /c:200"},
        {L"system-errors.txt",
         L"qe System \"/q:*[System[(Level=1 or Level=2)]]\" /f:text /rd:true "
         L"/c:200"},
    };
    for (const Q& e : q) {
        std::string out;
        int rc = sysrecover::RunProcess(wevt, e.args, out);
        if (rc < 0)
            continue;
        if (WriteUtf8File(dir + L"\\" + e.file, out))
            ++n;
    }
    return n > 0;
}

// ── 壳扩展审计（用户 2026-10-05 规格）────────────────────────────────────
// "还原后桌面右键转圈 / explorer 卡死"的头号原因是第三方 shell 扩展死锁或
// DLL 缺失。枚举右键菜单处理器 / 图标叠加 / ShellExecuteHooks，解析到
// InprocServer32 的 DLL 并检查文件是否存在（缺失标 *** MISSING ***）。
std::wstring ResolveClsidDll(const wchar_t* clsid) {
    if (!clsid || !clsid[0])
        return {};
    std::wstring c = clsid;
    if (c[0] != L'{')
        c = L"{" + c + L"}";
    HKEY k = nullptr;
    std::wstring sub = L"CLSID\\" + c + L"\\InprocServer32";
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub.c_str(), 0, KEY_READ, &k) !=
        ERROR_SUCCESS)
        return L"<no InprocServer32>";
    wchar_t p[1024] = {};
    DWORD cb = sizeof(p) - 2, t = 0;
    RegQueryValueExW(k, nullptr, nullptr, &t, (LPBYTE)p, &cb);
    RegCloseKey(k);
    std::wstring d = p;
    if (!d.empty() && d[0] == L'"') {
        size_t e = d.find(L'"', 1);
        if (e != std::wstring::npos)
            d = d.substr(1, e - 1);
    } else {
        size_t pos = d.find(L".dll");
        if (pos != std::wstring::npos)
            d = d.substr(0, pos + 4);
        else {
            size_t sp = d.find(L' ');
            if (sp != std::wstring::npos)
                d = d.substr(0, sp);
        }
    }
    if (!d.empty()) {
        wchar_t ex[1200] = {};
        ExpandEnvironmentStringsW(d.c_str(), ex, 1200);
        d = ex;
    }
    return d;
}

void AuditExtOne(const char* scope, const wchar_t* name, const wchar_t* clsid,
                 std::string& out) {
    std::wstring dll = ResolveClsidDll(clsid);
    // 只有"解析出真实路径但文件不存在"才算 MISSING；`<no InprocServer32>`
    // 是部分 Windows 内建处理器的正常形态（如 Taskband Pin），不当故障。
    bool missing = !dll.empty() && dll[0] != L'<' &&
                   GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES;
    char n1[512] = {}, c1[256] = {}, d1[2048] = {};
    WideCharToMultiByte(CP_UTF8, 0, name ? name : L"", -1, n1, sizeof(n1),
                        nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, clsid ? clsid : L"", -1, c1, sizeof(c1),
                        nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, dll.c_str(), -1, d1, sizeof(d1), nullptr,
                        nullptr);
    char buf[3200];
    snprintf(buf, sizeof(buf), "[diag] shellext %s\\%s clsid=%s dll=%s%s\n",
             scope, n1, c1[0] ? c1 : "-", d1[0] ? d1 : "(none)",
             missing ? "  *** MISSING ***" : "");
    out += buf;
}

// 子键形态（子键名 = 处理程序名，默认值 = CLSID）
void AuditExtSubkeys(HKEY root, const wchar_t* sub, const char* scope,
                     std::string& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        if (RegEnumKeyExW(k, i, name, &nl, nullptr, nullptr, nullptr, nullptr) !=
            ERROR_SUCCESS)
            break;
        HKEY h = nullptr;
        if (RegOpenKeyExW(k, name, 0, KEY_READ, &h) != ERROR_SUCCESS)
            continue;
        wchar_t clsid[128] = {};
        DWORD cb = sizeof(clsid) - 2, t = 0;
        RegQueryValueExW(h, nullptr, nullptr, &t, (LPBYTE)clsid, &cb);
        RegCloseKey(h);
        // 两种注册形态都要认：子键名=友好名、默认值=CLSID（常见）；
        // 或子键名=CLSID、默认值=显示名（如 Taskband Pin）→ 交换。
        if (name[0] == L'{')
            AuditExtOne(scope, clsid, name, out);
        else
            AuditExtOne(scope, name, clsid, out);
    }
    RegCloseKey(k);
}

// 值形态（值名 = CLSID，数据 = 描述；如 ShellExecuteHooks）
void AuditExtValues(HKEY root, const wchar_t* sub, const char* scope,
                    std::string& out) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256] = {};
        DWORD nl = 255;
        BYTE data[512] = {};
        DWORD dl = sizeof(data) - 2, t = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &t, data, &dl) !=
            ERROR_SUCCESS)
            break;
        // 值形态：值名 = CLSID，数据 = 描述（如 ShellExecuteHooks）
        AuditExtOne(scope, (const wchar_t*)data, name, out);
    }
    RegCloseKey(k);
}

// ── 救援层黑匣子回执（ZJRESTORE-status.txt，用户 2026-10-04 规格 2）────────
std::wstring Utf8ToW(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

void FindStatusFiles(const std::wstring& dir, int depth,
                     std::vector<std::wstring>& out) {
    if (depth < 0)
        return;
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            FindStatusFiles(full, depth - 1, out);
        else if (_wcsicmp(fd.cFileName, L"ZJRESTORE-status.txt") == 0)
            out.push_back(full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 屏幕截图（BMP，24bpp；用户 2026-10-05 规格）：支持包里带一张现场画面。
// GDI BitBlt 抓桌面 → 写标准 BMP（零依赖、PE 可用）。
bool CaptureScreenBmp(const std::wstring& path) {
    HWND desk = GetDesktopWindow();
    HDC dc = GetDC(desk);
    if (!dc)
        return false;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    bool written = false;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        if (BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY)) {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 24;
            bi.bmiHeader.biCompression = BI_RGB;
            DWORD stride = ((DWORD)w * 3 + 3) & ~3u;
            DWORD imgSize = stride * (DWORD)h;
            std::vector<unsigned char> pix(imgSize);
            if (GetDIBits(mem, bmp, 0, (UINT)h, pix.data(), &bi,
                          DIB_RGB_COLORS)) {
                BITMAPFILEHEADER fh = {};
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
                fh.bfSize = fh.bfOffBits + imgSize;
                HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
                if (hf != INVALID_HANDLE_VALUE) {
                    DWORD wr = 0;
                    written =
                        WriteFile(hf, &fh, sizeof(fh), &wr, nullptr) &&
                        WriteFile(hf, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr,
                                  nullptr) &&
                        WriteFile(hf, pix.data(), imgSize, &wr, nullptr);
                    CloseHandle(hf);
                }
            }
        }
        SelectObject(mem, old);
    }
    if (bmp)
        DeleteObject(bmp);
    if (mem)
        DeleteDC(mem);
    ReleaseDC(desk, dc);
    return written;
}

bool ReadSmallFile(const std::wstring& path, std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    char buf[8192] = {};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    text.assign(buf, rd);
    return true;
}

std::string StatusField(const std::string& text, const char* key) {
    std::string k = std::string(key) + "=";
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        std::string line = text.substr(
            p, e == std::string::npos ? std::string::npos : e - p);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.compare(0, k.size(), k) == 0)
            return line.substr(k.size());
        if (e == std::string::npos)
            break;
        p = e + 1;
    }
    return std::string();
}

std::wstring StatusStepText(const std::string& s) {
    if (s == "image-not-found")
        return sysrecover::Tr(L"找不到镜像文件");
    if (s == "target-not-found")
        return sysrecover::Tr(L"找不到目标分区");
    if (s == "mkntfs-failed")
        return sysrecover::Tr(L"格式化目标分区失败");
    if (s == "apply-failed")
        return sysrecover::Tr(L"应用镜像失败");
    if (s == "contract-mismatch")
        return sysrecover::Tr(L"程序与救援层版本不匹配");
    if (s == "no-task")
        return sysrecover::Tr(L"没有找到还原任务");
    if (s == "interrupted")
        return sysrecover::Tr(L"执行中断");
    return Utf8ToW(s);
}

}  // namespace

namespace sysrecover {

int DiagReportText(std::string& out) {
    char buf[640];
    snprintf(buf, sizeof(buf), "[diag] admin=%s\n", IsAdmin() ? "yes" : "no");
    out += buf;
    snprintf(buf, sizeof(buf), "[diag] firmware=%s\n",
             sysrecover::IsUefiFirmware() ? "UEFI" : "BIOS");
    out += buf;
    if (sysrecover::IsUefiFirmware()) {
        snprintf(buf, sizeof(buf), "[diag] secureboot=%s\n",
                 sysrecover::IsSecureBootEnabled() ? Tr("ON (需签名引导)") : "off");
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
            out += Tr("[diag] WARN: 本机固件只信任 CA2023，而我们的救援环境用 CA2011 " "签名 → 可能起不来（见 PLAN.md §11 备选 B/C）\n");
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
    std::wstring exeDir = sysrecover::AppDir();
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, exeDir.c_str(), -1, narrow, sizeof(narrow),
                        nullptr, nullptr);
    snprintf(buf, sizeof(buf), "[diag] exe_dir=%s\n", narrow);
    out += buf;
    // 日志根（PIT-105）：软件在系统盘时是 <数据盘>\ZJRESTORE（还原系统盘后仍在）
    std::wstring logBase = sysrecover::LogBaseDir();
    char nl[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, logBase.c_str(), -1, nl, sizeof(nl),
                        nullptr, nullptr);
    snprintf(buf, sizeof(buf), "[diag] log_dir=%s\n", nl);
    out += buf;
    // 存储/USB 控制器清单（用户 2026-10-04 需求，A5）：救援层排查"驱动覆盖/行为
    // 差异"时，必须能知道这台机器用的什么控制器。⚠️ `Enum\PCI` 受 ACL 保护
    //（管理员也读不到，实测），改走 `Control\Class\{设备类GUID}`（可读）：
    // 记录 驱动描述 / 服务 / 匹配的 PCI ID（MatchingDeviceId）。
    {
        const wchar_t* clsKeys[] = {
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4d36e97b-e325-11ce-bfc1-08002be10318}",  // SCSIAdapter
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{4d36e96a-e325-11ce-bfc1-08002be10318}",  // HDC
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
            L"{36fc9e60-c465-11cf-8056-444553540000}",  // USB
        };
        const char* clsName[] = {"SCSIAdapter", "HDC", "USB"};
        for (int ci = 0; ci < 3; ++ci) {
            HKEY k = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, clsKeys[ci], 0, KEY_READ,
                              &k) != ERROR_SUCCESS)
                continue;
            for (DWORD i = 0; i < 200; ++i) {
                wchar_t sub[32] = {};
                swprintf(sub, 32, L"%04u", i);
                HKEY d = nullptr;
                if (RegOpenKeyExW(k, sub, 0, KEY_READ, &d) != ERROR_SUCCESS)
                    continue;
                wchar_t desc[160] = {}, svc[128] = {}, mid[200] = {};
                for (int f = 0; f < 3; ++f) {
                    const wchar_t* nm =
                        f == 0 ? L"DriverDesc"
                               : (f == 1 ? L"Service" : L"MatchingDeviceId");
                    wchar_t* b = f == 0 ? desc : (f == 1 ? svc : mid);
                    DWORD cb = f == 0 ? sizeof(desc) - 2
                                      : (f == 1 ? sizeof(svc) - 2
                                                : sizeof(mid) - 2);
                    DWORD t = 0;
                    RegQueryValueExW(d, nm, nullptr, &t, (LPBYTE)b, &cb);
                }
                RegCloseKey(d);
                if (!desc[0] && !mid[0])
                    continue;
                char b1[320] = {}, b2[400] = {}, b3[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc, -1, b1, sizeof(b1), nullptr,
                                    nullptr);
                WideCharToMultiByte(CP_UTF8, 0, mid, -1, b2, sizeof(b2), nullptr,
                                    nullptr);
                WideCharToMultiByte(CP_UTF8, 0, svc, -1, b3, sizeof(b3), nullptr,
                                    nullptr);
                snprintf(buf, sizeof(buf),
                         "[diag] pci-ctrl class=%s desc=\"%s\" service=%s id=%s\n",
                         clsName[ci], b1, b3[0] ? b3 : "-", b2[0] ? b2 : "-");
                out += buf;
            }
            RegCloseKey(k);
        }
    }
    // 整机/主板/BIOS 标识 + 关键环境变量（用户 2026-10-05 规格）：硬件兼容排查
    //（"是不是缺/错驱动、什么机型"）第一步要看的东西，全部零依赖（注册表/env）。
    {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ,
                          &k) == ERROR_SUCCESS) {
            const wchar_t* vals[] = {
                L"SystemManufacturer", L"SystemProductName",
                L"BaseBoardManufacturer", L"BaseBoardProduct",
                L"BIOSVendor", L"BIOSVersion", L"BIOSReleaseDate"};
            for (const wchar_t* v : vals) {
                wchar_t b[256] = {};
                DWORD cb = sizeof(b) - 2, t = 0;
                if (RegQueryValueExW(k, v, nullptr, &t, (LPBYTE)b, &cb) !=
                        ERROR_SUCCESS ||
                    !b[0])
                    continue;
                char vn[64] = {}, u[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, v, -1, vn, sizeof(vn), nullptr,
                                    nullptr);
                WideCharToMultiByte(CP_UTF8, 0, b, -1, u, sizeof(u), nullptr,
                                    nullptr);
                snprintf(buf, sizeof(buf), "[diag] bios %s=%s\n", vn, u);
                out += buf;
            }
            RegCloseKey(k);
        }
        const wchar_t* evs[] = {L"PROCESSOR_ARCHITECTURE", L"SystemRoot",
                                L"windir", L"TEMP"};
        for (const wchar_t* e : evs) {
            wchar_t b[512] = {};
            if (!GetEnvironmentVariableW(e, b, 512) || !b[0])
                continue;
            char vn[64] = {}, u[640] = {};
            WideCharToMultiByte(CP_UTF8, 0, e, -1, vn, sizeof(vn), nullptr,
                                nullptr);
            WideCharToMultiByte(CP_UTF8, 0, b, -1, u, sizeof(u), nullptr,
                                nullptr);
            snprintf(buf, sizeof(buf), "[diag] env %s=%s\n", vn, u);
            out += buf;
        }
        snprintf(buf, sizeof(buf), "[diag] locale=%lu\n",
                 (unsigned long)GetUserDefaultUILanguage());
        out += buf;
    }
    // 壳扩展审计（用户 2026-10-05 规格）：还原后"桌面右键转圈/explorer 卡死"
    // 排查用 —— 列全右键菜单处理器/图标叠加/ShellExecuteHooks 及其 DLL 是否缺失。
    {
        static const wchar_t* ctxKeys[] = {
            L"*\\shellex\\ContextMenuHandlers",
            L"AllFilesystemObjects\\shellex\\ContextMenuHandlers",
            L"Directory\\shellex\\ContextMenuHandlers",
            L"Directory\\Background\\shellex\\ContextMenuHandlers",
            L"Drive\\shellex\\ContextMenuHandlers",
            L"Folder\\shellex\\ContextMenuHandlers",
            L"CLSID\\{20D04FE0-3AEA-1069-A2D8-08002B30309D}\\shellex\\"
            L"ContextMenuHandlers",
        };
        for (const wchar_t* s : ctxKeys)
            AuditExtSubkeys(HKEY_CLASSES_ROOT, s, "ctx", out);
        AuditExtSubkeys(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                        L"Explorer\\ShellIconOverlayIdentifiers",
                        "overlay", out);
        AuditExtValues(HKEY_LOCAL_MACHINE,
                       L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                       L"Explorer\\ShellExecuteHooks",
                       "exec-hook", out);
        HKEY bk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                          L"Shell Extensions\\Blocked",
                          0, KEY_READ, &bk) == ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t vn[128] = {};
                DWORD nl = 127;
                BYTE data[512] = {};
                DWORD dl = sizeof(data) - 2, t = 0;
                if (RegEnumValueW(bk, i, vn, &nl, nullptr, &t, data, &dl) !=
                    ERROR_SUCCESS)
                    break;
                char n1[256] = {}, d1[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, vn, -1, n1, sizeof(n1), nullptr,
                                    nullptr);
                WideCharToMultiByte(CP_UTF8, 0, (const wchar_t*)data, -1, d1,
                                    sizeof(d1), nullptr, nullptr);
                snprintf(buf, sizeof(buf), "[diag] shellext blocked %s = %s\n",
                         n1, d1);
                out += buf;
            }
            RegCloseKey(bk);
        }
    }
    // 悬空引用（PIT-132）：注册表引用指向"备份排除的易失目录"（Temp 等）且
    // 文件缺失 —— 还原后必然损坏（实测：豆包便携版把右键扩展注册在 Temp →
    // 每次右键卡死）；备份会并入修复包、目标系统首启自动清理。
    {
        std::vector<refscan::DanglingRef> refs =
            refscan::ScanDanglingRefs(true);
        int miss = 0;
        for (auto& r : refs)
            if (r.missingNow) miss++;
        out += "[diag] volatile-ref: " + std::to_string(refs.size()) +
               " (missing now=" + std::to_string(miss) + "; at-risk=" +
               std::to_string((int)refs.size() - miss) +
               "; first-boot auto-clean)\n";
        for (size_t i = 0; i < refs.size() && i < 20; ++i) {
            char d1[2048] = {};
            WideCharToMultiByte(CP_UTF8, 0, refs[i].display.c_str(), -1, d1,
                                sizeof(d1), nullptr, nullptr);
            out += std::string("[diag] volatile-ref: ") + d1 + "\n";
        }
    }
    // 磁盘健康（PIT-086）：SMART 能读时打印关键属性；读不到（NVMe/RAID/USB 桥）明确写
    // unavailable（不是错误，是能力边界）。CAUTION = 我们判定"建议先换盘"。
    for (const auto& d : sysrecover::EnumerateDisks()) {
        sysrecover::DiskHealth h = sysrecover::QueryDiskHealth(d.index);
        char nm[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.model.c_str(), -1, nm, sizeof(nm),
                            nullptr, nullptr);
        if (!h.smartKnown)
            snprintf(buf, sizeof(buf),
                     "[diag] disk%u health=%s smart=unavailable\n", d.index, nm);
        else if (h.isNvme)
            snprintf(buf, sizeof(buf),
                     "[diag] disk%u health=%s smart=nvme%s crit=0x%02x media=%lu "
                     "errlog=%lu used=%d%% temp=%dC\n",
                     d.index, nm, h.caution ? " **CAUTION**" : "",
                     h.criticalWarning, (unsigned long)h.mediaErrors,
                     (unsigned long)h.errorLogCount, h.usedPercent, h.tempC);
        else
            snprintf(buf, sizeof(buf),
                     "[diag] disk%u health=%s smart=ata%s%s st=%s srcl=0x%02x "
                     "srch=0x%02x realloc=%lu pending=%lu uncorrect=%lu\n",
                     d.index, nm, h.failing ? " FAILING" : "",
                     h.caution ? " **CAUTION**" : "",
                     h.failingKnown ? (h.failing ? "over" : "ok") : "unk",
                     h.srCl, h.srCh,
                     (unsigned long)h.reallocatedSectors,
                     (unsigned long)h.pendingSectors,
                     (unsigned long)h.uncorrectableSectors);
        out += buf;
    }
    return rc;
}

void DiskListText(std::string& out) {
    const auto disks = sysrecover::EnumerateDisks();
    const double gb = 1024.0 * 1024 * 1024;
    char buf[720];
    for (const auto& d : disks) {
        snprintf(buf, sizeof(buf), "Disk %u  %s  %.0fGB  %s%s\r\n", d.index,
                 ToUtf8(d.model.empty() ? std::wstring(L"?") : d.model).c_str(),
                 d.sizeBytes / gb, sysrecover::StyleName(d.style),
                 d.isRemovable ? "  [Removable]" : "");
        out += buf;
        for (const auto& p : d.parts) {
            snprintf(buf, sizeof(buf),
                     "  Part %u  %s  %s  %.0fGB free %.0fGB%s%s%s\r\n",
                     p.partNumber,
                     ToUtf8(p.letter.empty() ? std::wstring(L"-") : p.letter)
                         .c_str(),
                     ToUtf8(p.fs.empty() ? std::wstring(L"?") : p.fs).c_str(),
                     p.sizeBytes / gb, p.freeBytes / gb,
                     p.isSystem ? "  [System]" : "",
                     p.isEsp ? "  [ESP]" : "",
                     p.isRecovery ? "  [Recovery]" : "");
            out += buf;
        }
    }
}

void WriteDiagFiles(const std::wstring& logsDir) {
    CreateDirectoryW(logsDir.c_str(), nullptr);
    std::string diag;
    DiagReportText(diag);
    if (!WriteUtf8File(logsDir + L"\\diag.txt", diag))
        LogWarn("auto diag.txt write failed");
    std::string list;
    DiskListText(list);
    if (!WriteUtf8File(logsDir + L"\\list.txt", list))
        LogWarn("auto list.txt write failed");
    // 用户 2026-10-04 规格 1：盘/分区表**同时进主日志**（LOG 里就能看到本机
    // 检测到的所有驱动器与分区），排错不用再翻 list.txt。
    {
        size_t p = 0;
        while (p < list.size()) {
            size_t e = list.find("\r\n", p);
            std::string line = (e == std::string::npos)
                                   ? list.substr(p)
                                   : list.substr(p, e - p);
            p = (e == std::string::npos) ? list.size() : e + 2;
            if (!line.empty())
                LogFileInfo("inventory: " + line);
        }
    }
}

// 救援层失败回执（黑匣子）：见 selfdiag.h。回执由救援层写到每个分区根的
// ZJRESTORE-status.txt，启动时已随 CollectDeployArtifacts 收进 logs\collected。
std::wstring CheckLastRescueFailure() {
    std::wstring base = LogBaseDir() + L"\\logs";
    std::vector<std::wstring> files;
    FindStatusFiles(base + L"\\collected", 4, files);
    // 日志根直下的回执（救援层"成功"会把 ZJRESTORE-status.txt 写进软件目录
    // 的 logs\ —— BIOS 机器没有 ESP，这是成功唯一的回执落点）。
    FindStatusFiles(base, 1, files);
    std::string bestTime, bestStep, bestBuild;  // 最新一条 FAILED 回执
    std::string lastAnyTime;                    // 最新一条回执（OK/FAILED 都算）
    for (const auto& f : files) {
        std::string t;
        if (!ReadSmallFile(f, t))
            continue;
        std::string tm = StatusField(t, "time");
        if (lastAnyTime.empty() || tm > lastAnyTime)
            lastAnyTime = tm;
        if (StatusField(t, "result") != "FAILED")
            continue;
        // 同一回执出现在多个分区且内容一致；取时间最大的一次
        if (bestTime.empty() || tm > bestTime) {
            bestTime = tm;
            bestStep = StatusField(t, "step");
            bestBuild = StatusField(t, "build");
        }
    }
    // 待执行标记（只有"暂存+重启"路径写；菜单安装不写）。判定抽成纯函数
    //（common/rescue_decision.h，单测覆盖）：标记比所有回执新（或压根没回执）
    // → 救援层**从未执行**（引导失败/断电/被跳过），同样必须提示（2026-10-05 补洞）。
    std::wstring pendFile = base + L"\\pending-restore.txt";
    std::string pend;
    bool pendExists = ReadSmallFile(pendFile, pend);
    RescueRunTimes times;
    times.latestStatusTime = lastAnyTime;
    times.latestFailedTime = bestTime;
    times.pendingTime = pendExists ? StatusField(pend, "time") : std::string();
    RescueReport rep = DecideRescueReport(times);
    if (rep == RescueReport::TaskNotExecuted) {
        DeleteFileW(pendFile.c_str());
        std::wstring msg =
            Tr(L"上次系统还原任务未执行（可能未进入恢复环境）。\n");
        msg += Tr(L"时间 ") + Utf8ToW(times.pendingTime) + Tr(L"，目标 ") +
               Utf8ToW(StatusField(pend, "target")) + L"\n";
        msg += Tr(L"请重试；若反复如此，把 logs 文件夹发给技术支持。");
        LogWarn("restore task staged but never executed: time=" +
                times.pendingTime);
        return msg;
    }
    if (pendExists)
        DeleteFileW(pendFile.c_str());  // 救援层跑过（有更新回执）：标记完成使命
    if (rep != RescueReport::RestoreFailed)
        return std::wstring();
    // 去重：同一回执只提示一次；再次失败/换版本会换 key → 再提示
    std::string key = bestTime + "|" + bestBuild + "|" + bestStep;
    std::wstring marker = base + L"\\.last-rescue-status";
    std::string old;
    if (ReadSmallFile(marker, old)) {
        while (!old.empty() && (old.back() == '\n' || old.back() == '\r' ||
                                old.back() == ' '))
            old.pop_back();
        if (old == key)
            return std::wstring();
    }
    WriteUtf8File(marker, key);
    std::wstring msg =
        Tr(L"上次系统还原未成功（") + StatusStepText(bestStep) + Tr(L"）\n");
    msg += Tr(L"时间 ") + Utf8ToW(bestTime) + Tr(L"，版本 ") +
           Utf8ToW(bestBuild) + L"\n";
    msg += Tr(L"日志已收集到 logs\\collected，请把它发给技术支持。");
    LogWarn("rescue failure receipt: step=" + bestStep + " time=" + bestTime +
            " build=" + bestBuild);
    return msg;
}

// 收集后清理散落日志（用户 2026-10-04 规格 4：不该到处放这种东西）。
int CleanupStrayLogs() {
    int n = 0;
    std::wstring activeBase = LogBaseDir();
    DWORD mask = GetLogicalDrives();
    for (wchar_t c = L'C'; c <= L'Z'; ++c) {
        if (!(mask & (1u << (c - L'A'))))
            continue;
        wchar_t rootBuf[5] = {c, L':', L'\\', 0};
        if (GetDriveTypeW(rootBuf) != DRIVE_FIXED)
            continue;
        std::wstring root = rootBuf;
        for (const wchar_t* nm :
             {L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
              L"ZJRESTORE-status.txt", L"zjrestore-boot.log"}) {
            std::wstring p = root + nm;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()))
                ++n;
        }
        std::wstring fb = root + L"ZJRESTORE-logs";
        if (GetFileAttributesW(fb.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveTreeAny(fb);
            ++n;
        }
        std::wstring zjr = root + L"ZJRESTORE";
        if (_wcsicmp(zjr.c_str(), activeBase.c_str()) != 0) {
            std::wstring zl = zjr + L"\\logs";
            if (GetFileAttributesW(zl.c_str()) != INVALID_FILE_ATTRIBUTES) {
                RemoveTreeAny(zl);
                ++n;
            }
        }
    }
    // ESP：清 \EFI\ZJRESTORE\logs\*（救援模块本身保留）
    std::string log;
    std::wstring esp = sysrecover::MountEsp(log);
    if (!esp.empty()) {
        for (const wchar_t* nm :
             {L"ZJRESTORE-last.log", L"ZJRESTORE-probe.txt",
              L"ZJRESTORE-status.txt", L"ZJRESTORE-mkntfs.out",
              L"ZJRESTORE-apply.out", L"ZJRESTORE-esp.out"}) {
            std::wstring p = esp + L"EFI\\ZJRESTORE\\logs\\" + nm;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()))
                ++n;
        }
        sysrecover::UnmountEsp(esp, log);
    }
    if (n)
        LogInfo("stray logs cleaned after collect: " + std::to_string(n));
    return n;
}

int CollectDeployArtifacts(const std::wstring& dstDir, wchar_t onlyDrive) {
    // 先建 collected 这一级：否则 CreateDirectoryW("<logs>\\collected\\C") 会因
    // 父目录不存在而失败 → 收集静默全废（实测踩到）。
    CreateDirectoryW(dstDir.c_str(), nullptr);
    int n = 0;
    // 盘符 ↔ 物理身份对照表：PE 与正常 Windows 盘符不一致，靠这张表对号
    //（用户 2026-10-04 规格 1/6：LOG 里能查到本机所有盘/分区）。
    {
        std::string map;
        char buf[512];
        for (const auto& d : sysrecover::EnumerateDisks()) {
            snprintf(buf, sizeof(buf), "Disk %u  %s  serial=%s  %.1fGB  %s\n",
                     d.index, ToUtf8(d.model).c_str(),
                     ToUtf8(d.serial.empty() ? std::wstring(L"?") : d.serial)
                         .c_str(),
                     d.sizeBytes / (1024.0 * 1024 * 1024),
                     sysrecover::StyleName(d.style));
            map += buf;
            for (const auto& p : d.parts) {
                snprintf(buf, sizeof(buf),
                         "  p%-2u off=%-12llu size=%-12llu letter=%-3s fs=%-7s "
                         "label=%s%s%s%s%s\n",
                         p.partNumber, (unsigned long long)p.offsetBytes,
                         (unsigned long long)p.sizeBytes,
                         p.letter.empty() ? "-" : ToUtf8(p.letter).c_str(),
                         ToUtf8(p.fs).c_str(), ToUtf8(p.label).c_str(),
                         p.isSystem ? " [System]" : "", p.isEsp ? " [ESP]" : "",
                         p.isMsr ? " [MSR]" : "",
                         p.isRecovery ? " [Recovery]" : "");
                map += buf;
            }
        }
        if (WriteUtf8File(dstDir + L"\\drive-map.txt", map))
            ++n;
    }
    if (onlyDrive) {
        n += CollectOneDrive((wchar_t)towupper(onlyDrive), dstDir);
    } else {
        DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'C'; c <= L'Z'; ++c)
            if (mask & (1u << (c - L'A')))
                n += CollectOneDrive(c, dstDir);
    }
    // ESP 无盘符：mountvol 临时挂上收（含黑匣子回执）
    n += CollectEspFiles(dstDir);
    if (n)
        LogInfo("deploy artifacts collected: " + std::to_string(n) +
                " file(s) -> " + ToUtf8(dstDir));
    return n;
}

SupportBundle BuildSupportBundle(const std::wstring& outZip) {
    SupportBundle r;
    std::wstring logsDir = LogBaseDir() + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);

    // 1) 先把各盘部署痕迹收进 logs\collected（0.6.18 收集器，幂等）
    CollectDeployArtifacts(logsDir + L"\\collected", 0);

    // 1b) 屏幕截图（用户 2026-10-05 规格）：现场画面进 logs\screens\（随 logs
    //     递归进包；BMP 零依赖）。抓不到只记警告，不影响出包。
    {
        std::wstring scr = logsDir + L"\\screens";
        CreateDirectoryW(scr.c_str(), nullptr);
        std::wstring shot = scr + L"\\screen-" + ZipStamp() + L".bmp";
        if (CaptureScreenBmp(shot))
            LogInfo("screen shot: " + ToUtf8(shot));
        else
            LogWarn("screen shot failed");
    }

    // 2) 暂存目录（dumps/events）放 %TEMP%，避免被打进 logs 自身
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring stage = std::wstring(tmp) + L"zj-support-" +
                         std::to_wstring(GetCurrentProcessId()) + L"-" +
                         std::to_wstring(GetTickCount64());
    CreateDirectoryW(stage.c_str(), nullptr);

    // 3) explorer 转储（含全部线程栈 → 看等待链卡在哪）
    std::wstring dumpDir = stage + L"\\dumps";
    CreateDirectoryW(dumpDir.c_str(), nullptr);
    for (unsigned long pid : FindProcessIdsByName(L"explorer.exe")) {
        wchar_t nm[64] = {};
        swprintf(nm, 64, L"explorer-%lu.dmp", pid);
        if (WriteProcessMiniDump(pid, dumpDir + L"\\" + nm))
            ++r.dumps;
    }

    // 4) 事件日志（Application Hang/Error、System 存储/错误级）
    int ev = 0;
    bool evOk = ExportEventLogsTo(stage + L"\\events", ev);
    r.eventFiles = ev;

    // 5) 输出路径：桌面；拿不到桌面则回退日志目录
    std::wstring zipPath = outZip;
    if (zipPath.empty()) {
        wchar_t desk[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr,
                                       0, desk)) &&
            desk[0])
            zipPath = std::wstring(desk) + L"\\SysRecover-logs-" + ZipStamp() +
                      L".zip";
        else
            zipPath = logsDir + L"\\SysRecover-logs-" + ZipStamp() + L".zip";
    }
    {  // --out 指向的目录可能不存在：建一层（尽力）
        size_t p = zipPath.find_last_of(L'\\');
        if (p != std::wstring::npos && p > 3)
            CreateDirectoryW(zipPath.substr(0, p).c_str(), nullptr);
    }

    sysrecover::ZipWriter zip(zipPath);
    if (!zip.ok()) {
        r.error = Tr(L"无法创建诊断包文件");
        RemoveTreeAny(stage);
        return r;
    }
    int n = 0;
    {
        std::string t;
        DiagReportText(t);
        if (zip.AddData(t.data(), t.size(), "diag.txt"))
            ++n;
    }
    {
        std::string t;
        DiskListText(t);
        if (zip.AddData(t.data(), t.size(), "list.txt"))
            ++n;
    }
    {
        std::wstring v = AppDir() + L"\\version.json";
        if (GetFileAttributesW(v.c_str()) != INVALID_FILE_ATTRIBUTES &&
            zip.AddFile(v, "version.json"))
            ++n;
    }

    // 当前日志目录（递归；跳过输出包自身，防自吞）
    ZipAddTree(zip, logsDir, L"logs", 8, zipPath, n);
    // 其它固定盘的 ZJRESTORE\logs（覆盖"单文件/temp 运行、日志落数据盘"的情况）
    {
        DWORD mask = GetLogicalDrives();
        for (wchar_t c = L'C'; c <= L'Z'; ++c) {
            if (!(mask & (1u << (c - L'A'))))
                continue;
            wchar_t root[4] = {c, L':', L'\\', 0};
            if (GetDriveTypeW(root) != DRIVE_FIXED)
                continue;
            std::wstring fl = std::wstring(root) + L"ZJRESTORE\\logs";
            if (GetFileAttributesW(fl.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            if (_wcsicmp(fl.c_str(), logsDir.c_str()) == 0)
                continue;
            ZipAddTree(zip, fl,
                       std::wstring(L"other/") + c + L"/ZJRESTORE/logs", 8,
                       zipPath, n);
        }
    }
    // 暂存目录（dumps/events）
    ZipAddTree(zip, stage, L"", 8, zipPath, n);

    // 包说明（人/机都好读）
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        char tbuf[64] = {};
        snprintf(tbuf, sizeof(tbuf), "%04u-%02u-%02u %02u:%02u:%02u", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::string info;
        info += "SysRecover support bundle\n";
        info += "version  : " SYSRECOVER_VERSION "\n";
        info += "time     : ";
        info += tbuf;
        info += "\n";
        info += "exe_dir  : " + ToUtf8(AppDir()) + "\n";
        info += "log_dir  : " + ToUtf8(logsDir) + "\n";
        info += "zip      : " + ToUtf8(zipPath) + "\n";
        info += "dumps    : " + std::to_string(r.dumps) + "\n";
        info += "events   : " + std::to_string(r.eventFiles) +
                (evOk ? "" : " (wevtutil unavailable)") + "\n";
        info += "logfiles : " + std::to_string(n) + "\n";
        zip.AddData(info.data(), info.size(), "bundle-info.txt");
    }

    if (!zip.Close()) {
        r.error = Tr(L"写诊断包失败");
        RemoveTreeAny(stage);
        return r;
    }
    RemoveTreeAny(stage);
    r.logFiles = n;
    r.zipPath = zipPath;
    LogInfo("support bundle: " + ToUtf8(zipPath) +
            " logs=" + std::to_string(n) + " dumps=" + std::to_string(r.dumps) +
            " events=" + std::to_string(r.eventFiles));
    // 出包成功 → 清掉散落在各盘的日志（用户 2026-10-04 规格 4：收集后不该
    // 到处放）。只删已知日志文件/目录，契约与引导文件不动。
    CleanupStrayLogs();
    return r;
}

// ── EA（NTFS 扩展属性）扫描（用户 2026-10-05）────────────────────────────
// 背景：客户 Linux 侧还原的 apply.out 报 `Ignoring extended attributes of
// 804 files` —— EA 只在 Linux 还原路径丢失，Windows（PE 就地）保留。此命令
// 列出"哪些文件带 EA、EA 叫什么"，供取证（在 PE 还原后的系统上跑一次）。
extern "C" long __stdcall NtQueryEaFile(HANDLE, void*, void*, unsigned long,
                                        unsigned char, void*, unsigned long,
                                        unsigned long*, unsigned char);
namespace {
const long kStatusBufferOverflow = (long)0x80000005;

void EaScanRec(const std::wstring& dir, std::string& out,
               unsigned long long& files, unsigned long long& hit,
               bool& capped) {
    static std::vector<unsigned char> buf;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                EaScanRec(p, out, files, hit, capped);
            continue;
        }
        ++files;
        HANDLE f = CreateFileW(p.c_str(), FILE_READ_EA | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        if (buf.empty())
            buf.assign(256 * 1024, 0);
        struct {
            long status;
            void* info;
        } iosb = {};
        long st = NtQueryEaFile(f, &iosb, buf.data(), (unsigned long)buf.size(),
                                FALSE, nullptr, 0, nullptr, FALSE);
        CloseHandle(f);
        if (st != 0 && st != kStatusBufferOverflow)
            continue;  // 没有 EA / 无权限
        ++hit;
        if (capped)
            continue;
        if (out.size() > (8u << 20)) {  // 输出上限 8MB（防爆内存）
            capped = true;
            continue;
        }
        out += ToUtf8(p);
        if (st == kStatusBufferOverflow) {
            out += " | (EA too large, names truncated)\r\n";
            continue;
        }
        unsigned long off = 0;
        for (;;) {
            unsigned char* e = buf.data() + off;
            unsigned long next = *(unsigned long*)e;
            unsigned char nameLen = e[5];
            unsigned short valLen = *(unsigned short*)(e + 6);
            out += " | ";
            out.append((char*)e + 8, nameLen);
            out += "=";
            size_t vn = valLen < 32 ? valLen : 32;
            for (size_t i = 0; i < vn; ++i) {
                char b[4];
                snprintf(b, sizeof(b), "%02X", e[8 + nameLen + 1 + i]);
                out += b;
            }
            if (valLen > 32)
                out += "..";
            if (next == 0 || (off + next + 8) >= buf.size())
                break;
            off += next;
        }
        out += "\r\n";
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
}  // namespace

int ScanEaFiles(const std::wstring& root, const std::wstring& outPath,
                std::string& err) {
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
        err = "root not found";
        return -1;
    }
    std::string out;
    out += "# SysRecover scan-ea " SYSRECOVER_VERSION " root=" + ToUtf8(root) +
           "\r\n";
    unsigned long long files = 0, hit = 0;
    bool capped = false;
    EaScanRec(root, out, files, hit, capped);
    out += "# files=" + std::to_string(files) +
           " withEA=" + std::to_string(hit) +
           (capped ? " (listing capped)" : "") + "\r\n";
    std::wstring dst =
        outPath.empty() ? (LogBaseDir() + L"\\logs\\ea-scan.txt") : outPath;
    CreateDirectoryW((LogBaseDir() + L"\\logs").c_str(), nullptr);
    if (!WriteUtf8File(dst, out)) {
        err = "write failed";
        return -1;
    }
    LogInfo("ea-scan: files=" + std::to_string(files) +
            " withEA=" + std::to_string(hit) + " -> " + ToUtf8(dst));
    return (int)hit;
}

}  // namespace sysrecover
