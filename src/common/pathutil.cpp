#include "pathutil.h"

#include <windows.h>

#include <cwctype>

namespace sysrecover {
namespace {

std::wstring TrimSpaces(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace

bool IsVolumeRoot(const std::wstring& p0) {
    std::wstring p = TrimSpaces(p0);
    if (p.size() < 2 || p[1] != L':')
        return false;
    if (p.size() == 2)
        return true;
    return p.size() == 3 && (p[2] == L'\\' || p[2] == L'/');
}

std::wstring NormalizeVolumeRoot(const std::wstring& p0) {
    if (!IsVolumeRoot(p0))
        return p0;
    std::wstring p = TrimSpaces(p0);
    if (p[0] >= L'a' && p[0] <= L'z')
        p[0] = static_cast<wchar_t>(p[0] - (L'a' - L'A'));
    return p.substr(0, 2) + L"/";
}

std::wstring ParentDir(const std::wstring& path) {
    size_t i = path.find_last_of(L"\\/");
    if (i == std::wstring::npos)
        return std::wstring();
    if (i == 2 && path.size() > 2 && path[1] == L':')
        return path.substr(0, 3);  // `D:\a.wim` → `D:\`（盘符根目录要带分隔符）
    return path.substr(0, i);
}

bool MakeDirTree(const std::wstring& dir) {
    if (dir.empty())
        return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring parent = ParentDir(dir);
    if (!parent.empty() && parent != dir && !MakeDirTree(parent))
        return false;
    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace sysrecover
