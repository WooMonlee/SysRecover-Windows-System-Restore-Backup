#pragma once
// 计算机名生成（PIT-211，用户 2026-11-02 规格）：
//   W + Windows 大版本 + 厂家 + YYMMDDHHMMSS   → 例如 W10ASUS261013112604
//   · 厂家来自 bioskey 的识别结果（ASUS/Dell/Lenovo/HP/Acer…），认不出用 PC；
//   · 大版本只能是 7 / 8 / 10 / 11（Win8.1 也记 8）；
//   · 时间 = 确认还原的那一刻（本地时间）。
// 纯逻辑放 header（单测直接包含），Windows 侧的版本探测在 compname.cpp。
#include <cwchar>
#include <cwctype>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sysrecover {

// 只保留字母/数字并转大写；过长截断（NetBIOS 15 字符，这里给到 12 留余量）。
inline std::wstring SanitizeVendor(const std::wstring& vendor) {
    std::wstring v;
    for (wchar_t c : vendor) {
        if (iswalnum(c)) v.push_back((wchar_t)towupper(c));
    }
    if (v.size() > 12) v.resize(12);
    if (v.empty()) v = L"PC";
    return v;
}

inline std::wstring MakeComputerName(const std::wstring& vendor, int winMajor,
                                     int year, int month, int day, int hour,
                                     int minute, int second) {
    if (winMajor != 7 && winMajor != 8 && winMajor != 10 && winMajor != 11)
        winMajor = 10;
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"W%d%ls%02d%02d%02d%02d%02d%02d", winMajor,
             SanitizeVendor(vendor).c_str(), year % 100, month, day, hour,
             minute, second);
    return buf;
}

// 目标系统（可以是离线目录，如 C:\Windows 或 C:\~new\Windows）的 Windows
// 大版本：读它的 SOFTWARE 库里的 CurrentBuild。读不到 → 0（调用方用当前系统兜底）。
int WindowsMajorOfDir(const std::wstring& winDir);

}  // namespace sysrecover
