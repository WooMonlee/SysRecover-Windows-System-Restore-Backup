// 计算机名：目标系统大版本探测（PIT-211）。
// 读**目标系统自己的** SOFTWARE 库（离线目录也能读），不依赖当前运行的 OS ——
// 用 PE 给 Win11 镜像改名时，版本号才不会被 PE 的 10 带偏。
#include "compname.h"

#ifdef _WIN32
#include <windows.h>

#include <cstdlib>

namespace sysrecover {
namespace {

// 读一个 REG_SZ 的字符串值（不存在/类型不符 → 空）。
std::wstring RegStr(HKEY root, const std::wstring& sub, const wchar_t* name) {
    wchar_t buf[128] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ, &type, buf,
                     &sz) != ERROR_SUCCESS)
        return std::wstring();
    return buf;
}

int MajorFromBuild(int build) {
    if (build >= 22000) return 11;  // Win11
    if (build >= 10240) return 10;  // Win10 / 2016+
    if (build >= 9600) return 8;    // 8.1
    if (build >= 9200) return 8;
    if (build >= 7600) return 7;
    return 0;
}

}  // namespace

int WindowsMajorOfDir(const std::wstring& winDir) {
    if (winDir.empty()) return 0;
    const std::wstring hive = winDir + L"\\System32\\config\\SOFTWARE";
    if (GetFileAttributesW(hive.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    const wchar_t* kTmp = L"ZJ_VER";
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kTmp);  // 清上次残留（异常退出）
    if (RegLoadKeyW(HKEY_LOCAL_MACHINE, kTmp, hive.c_str()) != ERROR_SUCCESS)
        return 0;
    const std::wstring base = std::wstring(kTmp) +
                              L"\\Microsoft\\Windows NT\\CurrentVersion";
    int build = _wtoi(RegStr(HKEY_LOCAL_MACHINE, base, L"CurrentBuild").c_str());
    if (build == 0)
        build = _wtoi(
            RegStr(HKEY_LOCAL_MACHINE, base, L"CurrentBuildNumber").c_str());
    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kTmp);
    return MajorFromBuild(build);
}

}  // namespace sysrecover

#else  // !_WIN32（单测/工具链不编译本文件；给个空实现防链接错）
namespace sysrecover {
int WindowsMajorOfDir(const std::wstring&) { return 0; }
}  // namespace sysrecover
#endif
