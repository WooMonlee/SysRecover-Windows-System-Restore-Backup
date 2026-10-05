// ea-scan.cpp — enumerate files carrying NTFS extended attributes (EA).
// Investigation tool for the 2026-10-05 customer case: the Linux-side
// wimlib apply printed "[WARNING] Ignoring extended attributes of 804 files";
// Windows-side wimlib does preserve EAs. This tool finds EA-bearing files
// locally and can also *create* one for lab tests.
//
// Usage:
//   ea-scan.exe "C:\Program Files" "C:\Program Files (x86)"   (scan)
//   ea-scan.exe --set <file>                                   (create EA)
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

// MinGW headers lack FileExtendedAttributesInfo (SDK value 22).
// Only the first two fields are needed for "does it have any EA".
#define FileExtendedAttributesInfo ((FILE_INFO_BY_HANDLE_CLASS)22)
typedef struct _FILE_EXTENDED_ATTRIBUTES_INFORMATION {
    ULONG Version;
    ULONG AttributeCount;
} FILE_EXTENDED_ATTRIBUTES_INFORMATION;

// NtSetEaFile is the only way to WRITE EAs (SetFileInformationByHandle does
// not support FileExtendedAttributesInfo for set operations).
extern "C" long __stdcall NtSetEaFile(HANDLE, void*, void*, unsigned long);

static bool set_ea(const wchar_t* path, const char* name, const char* value) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"open failed %ls err=%lu\n", path, GetLastError());
        return false;
    }
    unsigned char buf[512] = {};
    size_t nl = strlen(name), vl = strlen(value);
    ULONG* next = (ULONG*)buf;
    *next = 0;
    buf[4] = 0;                           // Flags
    buf[5] = (unsigned char)nl;           // EaNameLength (no NUL)
    buf[6] = (unsigned char)(vl & 0xFF);  // EaValueLength
    buf[7] = (unsigned char)((vl >> 8) & 0xFF);
    memcpy(buf + 8, name, nl + 1);
    memcpy(buf + 8 + nl + 1, value, vl);
    unsigned long len = (unsigned long)(8 + nl + 1 + vl);
    struct {
        long status;
        void* info;
    } iosb = {};
    long st = NtSetEaFile(h, &iosb, buf, len);
    CloseHandle(h);
    if (st != 0)
        wprintf(L"NtSetEaFile failed ntstatus=0x%08X\n", (unsigned)st);
    return st == 0;
}

static unsigned long long g_files = 0, g_ea = 0;
static int g_printed = 0;
static unsigned long long g_attrTotal = 0;

static void walk(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        std::wstring p = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            walk(p);
            continue;
        }
        ++g_files;
        HANDLE f = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        unsigned char buf[1024];
        if (GetFileInformationByHandleEx(f, FileExtendedAttributesInfo, buf,
                                         sizeof(buf))) {
            auto* ei = (FILE_EXTENDED_ATTRIBUTES_INFORMATION*)buf;
            if (ei->AttributeCount > 0) {
                ++g_ea;
                g_attrTotal += ei->AttributeCount;
                if (g_printed < 60) {
                    wprintf(L"[EA x%u] %ls\n", ei->AttributeCount, p.c_str());
                    ++g_printed;
                }
            }
        } else if (GetLastError() == ERROR_MORE_DATA) {
            ++g_ea;  // too many attributes to fit: certainly has EAs
            if (g_printed < 60) {
                wprintf(L"[EA many] %ls\n", p.c_str());
                ++g_printed;
            }
        }
        CloseHandle(f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && !wcscmp(argv[1], L"--set")) {
        bool ok = set_ea(argv[2], "ZJTEST", "1");
        printf("set-ea %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }
    for (int i = 1; i < argc; ++i)
        walk(argv[i]);
    printf("files=%llu withEA=%llu attrs=%llu\n", g_files, g_ea, g_attrTotal);
    return 0;
}
