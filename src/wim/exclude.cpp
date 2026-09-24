// 捕获排除配置实现。wimlib_add_image 的 config_file 参数直接接受此格式。
#include "exclude.h"

#include <windows.h>

#include <string>
#include <vector>

namespace sysrecover {
namespace {

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

const char* DefaultExclusionConfig() {
    return
        "[ExclusionList]\n"
        "\\hiberfil.sys\n"
        "\\pagefile.sys\n"
        "\\swapfile.sys\n"
        "\\$ntfs.log\n"
        "\\System Volume Information\n"
        "\\$Recycle.Bin*\n"
        "\\Recycler\n"
        "\\Windows\\CSC\n"
        "\\Windows\\Temp*\n"
        "\\Windows\\Logs*\n"
        "\\Windows\\Prefetch*\n"
        "\\Windows\\SoftwareDistribution\n"
        "\\Windows\\CbsTemp*\n"
        "\\Users\\*\\AppData\\Local\\Temp*\n"
        "\\Users\\*\\AppData\\Local\\Microsoft\\Windows\\INetCache*\n"
        "\\Users\\*\\AppData\\Local\\Microsoft\\Windows\\Explorer*\n"
        "\\Windows\\winsxs\\InstallTemp*\n"
        "\\Windows\\winsxs\\ManifestCache*\n"
        "\\*.et\n"
        "\\*.tmp\n";
    // 注意：**不要**排除注册表事务日志（`\\Windows\\System32\\config\\*.LOG1/2`、
    // `*.regtrans-ms`、`*.TM.blf`、`\\Users\\*\\NTUSER.DAT*.LOG*`）。
    // 热备（VSS）时若有 hive 处于"脏"状态（primary_seq != secondary_seq，运行中的
    // 系统很常见），Windows 开机必须靠这些日志把 hive 恢复一致；日志被排除 =
    // **还原后黑屏起不来**（PIT-056，实测 SYSTEM hive primary=130/secondary=129）。
    // 微软默认 WimScript.ini 也不排除它们。日志只有几 MB，随包捕获即可。
    // ↑ 单元测试 tests/unit_tests.cpp 有**回归守卫**，别手滑加回去。
}

const std::vector<std::wstring>& CloudFolderNames() {
    static const std::vector<std::wstring> kFolders = {
        L"OneDrive",
        L"OneDrive - \u4e2a\u4eba",
        L"OneDrive - \u5de5\u4f5c",
        L"Google Drive",
        L"Dropbox",
        L"iCloudDrive",
        L"\u575a\u679c\u4e91",       // 坚果云
        L"\u767e\u5ea6\u7f51\u76d8",  // 百度网盘
        L"\u5fae\u4e91",              // 微云
        L"115\u7f51\u76d8",
        L"\u963f\u91cc\u4e91\u76d8",  // 阿里云盘
        L"\u5929\u7ffc\u4e91\u76d8",  // 天翼云盘
        L"\u548c\u5f69\u4e91",
        L"WPS Cloud",
        L"\u817e\u8baf\u5fae\u4e91",  // 腾讯微云
    };
    return kFolders;
}

std::string BuildExclusionContent(
    const std::vector<std::wstring>& presentCloudFolders) {
    std::string content = DefaultExclusionConfig();
    for (const std::wstring& folder : presentCloudFolders) {
        if (folder.empty())
            continue;
        // 转 UTF-8 后追加 "\\<folder>\*" 行
        int n = WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
        if (n <= 1)
            continue;
        std::string u8(n - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, folder.c_str(), -1, u8.data(), n,
                            nullptr, nullptr);
        std::string line = "\\" + u8 + "\\*\n";
        if (content.find(line) == std::string::npos)
            content += line;
    }
    return content;
}

std::wstring EnsureExclusionConfig(const std::wstring& sourceRoot) {
    std::wstring root = sourceRoot;
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
        root.pop_back();
    std::vector<std::wstring> present;
    for (const std::wstring& folder : CloudFolderNames())
        if (DirExists(root + L"\\" + folder))
            present.push_back(folder);
    std::string content = BuildExclusionContent(present);

    wchar_t tmpDir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmpDir) == 0)
        return L"";
    std::wstring path =
        std::wstring(tmpDir) + L"zjrestore-exclusion-dynamic.ini";
    HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"";
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written,
                        nullptr);
    CloseHandle(h);
    if (!ok || written != content.size())
        return L"";
    return path;
}

}  // namespace sysrecover
