#include "imgsearch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>

#include "../boot/grub.h"  // sysrecover::ExeDir()

namespace {

struct Hit {
    std::wstring path;
    unsigned long long mtime;  // ftLastWriteTime 转 64 位，比较用
};

constexpr size_t kCap = 500;  // 结果上限（列表过长没有意义，防误扫）

unsigned long long ToUll(const FILETIME& ft) {
    return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool IsImageFile(const std::wstring& p) {
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return _wcsicmp(p.c_str() + dot, L".esd") == 0 ||
           _wcsicmp(p.c_str() + dot, L".wim") == 0;
}

// 去尾 '\'; 取父目录；已在盘根（如 D:\ ）返回空（没有"上一级"可扫）。
std::wstring ParentDirOf(std::wstring p) {
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos) return L"";          // 形如 "D:"（盘根本身）
    std::wstring parent = p.substr(0, pos);
    if (parent.size() == 2 && parent[1] == L':') parent += L'\\';  // "C:" → "C:\"
    return parent;
}

// 扫 dir 一层（不递归），收集 .esd/.wim（排除隐藏/系统项）。
void AppendDir(const std::wstring& dirIn, std::vector<Hit>& out) {
    if (out.size() >= kCap || dirIn.empty()) return;
    std::wstring dir = dirIn;
    if (dir.back() != L'\\') dir += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW((dir + L'*').c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (out.size() >= kCap) break;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = dir + fd.cFileName;
        if (!IsImageFile(full)) continue;
        out.push_back({std::move(full), ToUll(fd.ftLastWriteTime)});
    } while (::FindNextFileW(h, &fd));
    ::FindClose(h);
}

// 父进程 exe 的完整路径；取不到（系统进程/权限）返回空。
std::wstring ParentProcessExePath() {
    DWORD myPid = ::GetCurrentProcessId();
    DWORD ppid = 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (::Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == myPid) {
                    ppid = pe.th32ParentProcessID;
                    break;
                }
            } while (::Process32NextW(snap, &pe));
        }
        ::CloseHandle(snap);
    }
    if (!ppid) return L"";
    HANDLE hp = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ppid);
    if (!hp) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = _countof(buf);
    BOOL ok = ::QueryFullProcessImageNameW(hp, 0, buf, &n);
    ::CloseHandle(hp);
    return ok ? std::wstring(buf, n) : L"";
}

// exe 是否位于 %TEMP% 下（单EXE 解压运行的特征）。
bool ExeUnderTemp(const std::wstring& exeDir) {
    wchar_t temp[MAX_PATH + 1] = {};
    if (::GetTempPathW(MAX_PATH, temp) == 0) return false;
    std::wstring t(temp);
    for (auto& c : t)
        if (c == L'/') c = L'\\';
    if (t.empty()) return false;
    return _wcsnicmp(exeDir.c_str(), t.c_str(), t.size()) == 0;
}

// exe 是否装在 Program Files / Program Files (x86) 体系下（任一盘符）。
bool ExeUnderProgramFiles(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    for (auto& c : dir) c = static_cast<wchar_t>(towlower(c));
    auto has = [&dir](const std::wstring& key) {
        size_t pos = dir.find(key);
        while (pos != std::wstring::npos) {
            size_t end = pos + key.size();
            if (end == dir.size() || dir[end] == L'\\') return true;
            pos = dir.find(key, end);
        }
        return false;
    };
    return has(L"\\program files (x86)") || has(L"\\program files");
}

// 规则 2：所有非系统分区，盘根一层 + 每个一级子目录内一层（不再往下）。
void AppendNonSystemDrives(std::vector<Hit>& out) {
    wchar_t win[MAX_PATH] = {};
    ::GetWindowsDirectoryW(win, MAX_PATH);
    wchar_t sysLetter = win[0];
    DWORD mask = ::GetLogicalDrives();
    for (wchar_t c = L'A'; c <= L'Z' && out.size() < kCap; ++c) {
        if (!(mask & (1u << (c - L'A')))) continue;
        if (c == sysLetter) continue;
        std::wstring root = {c, L':', L'\\'};
        UINT type = ::GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        AppendDir(root, out);
        WIN32_FIND_DATAW fd;
        HANDLE h = ::FindFirstFileW((root + L'*').c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (out.size() >= kCap) break;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
                continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            AppendDir(root + fd.cFileName, out);
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
}

}  // namespace

std::vector<std::wstring> imgsearch::Search() {
    std::vector<Hit> hits;
    const std::wstring exeDir = sysrecover::ExeDir();

    if (ExeUnderTemp(exeDir)) {
        // 规则 1：单EXE —— 父进程所在目录 + 其上一级（各一层）
        std::wstring parentExe = ParentProcessExePath();
        if (!parentExe.empty()) {
            std::wstring pdir = ParentDirOf(parentExe);
            if (!pdir.empty()) {
                AppendDir(pdir, hits);
                std::wstring up = ParentDirOf(pdir);
                if (!up.empty()) AppendDir(up, hits);
            }
        }
    } else if (ExeUnderProgramFiles(exeDir)) {
        // 规则 2：程序装在 Program Files → 镜像可能在任何数据盘
        AppendNonSystemDrives(hits);
    } else {
        // 规则 3：正常 —— exe 目录的上一级，仅一层
        std::wstring up = ParentDirOf(exeDir);
        if (!up.empty()) AppendDir(up, hits);
    }

    // 按修改时间新 → 旧（"最新的一个" = 第 0 个）
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& a, const Hit& b) { return a.mtime > b.mtime; });

    std::vector<std::wstring> out;
    out.reserve(hits.size());
    for (auto& h : hits) out.push_back(std::move(h.path));
    return out;
}
