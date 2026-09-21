// 运行中系统的可读描述（见 sysinfo.h 顶部规格）。
#include "sysinfo.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>

namespace sysrecover {
namespace {

// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion（强制 64 位视图：我们要的是
// 操作系统本身的版本信息，不是当前进程的位数）。
const wchar_t* kCurrentVersion =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

std::wstring ReadStr(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCurrentVersion, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[512] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out = buf;
    RegCloseKey(key);
    return out;
}

// 读不到返回 fallback（用 UINT32_MAX 表示"没有这个值"）。
DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kCurrentVersion, 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return fallback;
    DWORD value = fallback, cb = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &cb) != ERROR_SUCCESS ||
        type != REG_DWORD)
        value = fallback;
    RegCloseKey(key);
    return value;
}

// "Service Pack 1" → "SP1"（Win7 时代的写法）
std::wstring ShortServicePack(const std::wstring& csd) {
    const std::wstring kPrefix = L"Service Pack";
    if (csd.rfind(kPrefix, 0) != 0)
        return csd;
    std::wstring n = csd.substr(kPrefix.size());
    while (!n.empty() && n.front() == L' ')
        n.erase(n.begin());
    return n.empty() ? csd : L"SP" + n;
}

// EditionID → 中文名。
// 为什么不用 ProductName：实测有的系统（例如本机 Win10 IoT Enterprise LTSC 2021）
// ProductName 是**英文**、还带年份（"…LTSC 2021"），而我们要的是中文、不带年份的
// 简洁描述（用户规格：`Windows 10 IoT 企业版 LTSC 21H2 19044.4046`）。
const wchar_t* ChineseEdition(const std::wstring& id) {
    static const struct {
        const wchar_t* id;
        const wchar_t* name;
    } kMap[] = {
        {L"IoTEnterpriseS", L"IoT 企业版 LTSC"},
        {L"IoTEnterpriseSK", L"IoT 企业版"},
        {L"IoTEnterprise", L"IoT 企业版"},
        {L"EnterpriseS", L"企业版 LTSC"},
        {L"EnterpriseSN", L"企业版 LTSC"},
        {L"Professional", L"专业版"},
        {L"ProfessionalN", L"专业版"},
        {L"ProfessionalEducation", L"专业教育版"},
        {L"ProfessionalWorkstation", L"专业工作站版"},
        {L"Core", L"家庭版"},
        {L"CoreN", L"家庭版"},
        {L"CoreCountrySpecific", L"家庭中文版"},
        {L"CoreSingleLanguage", L"家庭单语言版"},
        {L"Enterprise", L"企业版"},
        {L"EnterpriseN", L"企业版"},
        {L"EnterpriseG", L"企业版 G"},
        {L"Education", L"教育版"},
        {L"EducationN", L"教育版"},
        {L"Starter", L"简易版"},
        {L"Ultimate", L"旗舰版"},
        {L"HomePremium", L"家庭高级版"},
        {L"HomeBasic", L"家庭普通版"},
        {L"ServerStandard", L"Server 标准版"},
        {L"ServerDatacenter", L"Server 数据中心版"},
    };
    for (const auto& e : kMap)
        if (id == e.id)
            return e.name;
    return nullptr;
}

}  // namespace

SystemDescription DescribeRunningSystem() {
    SystemDescription d;
    std::wstring product = ReadStr(L"ProductName");
    if (product.empty())
        product = ReadStr(L"EditionID");
    std::wstring edition = ReadStr(L"EditionID");
    std::wstring version = ReadStr(L"DisplayVersion");   // Win10 2004+："21H2"
    if (version.empty())
        version = ReadStr(L"ReleaseId");                 // 旧版："1909" 等
    if (version.empty())
        version = ShortServicePack(ReadStr(L"CSDVersion"));  // Win7："SP1"
    std::wstring build = ReadStr(L"CurrentBuildNumber");
    if (build.empty())
        build = ReadStr(L"CurrentBuild");
    DWORD ubr = ReadDword(L"UBR", 0xFFFFFFFFu);

    // Windows 11 的 ProductName 仍写 "Windows 10"（微软历史遗留），
    // 所以家族号一律以 build 为准：>= 22000 就是 11。
    bool isWin11 = false;
    if (!build.empty()) {
        unsigned long bn = wcstoul(build.c_str(), nullptr, 10);
        isWin11 = (bn >= 22000);
    }

    // 组装「Windows <家族> <中文版别>」：
    //   家族号取自 ProductName 的 "Windows N"，但 Win11 的 ProductName 仍写 10
    //   → 以 build 为准纠正；版别走 EditionID → 中文名（认不出才退回 ProductName 原文）。
    //   于是得到「Windows 10 IoT 企业版 LTSC」，而不是英文原值
    //   "Windows 10 IoT Enterprise LTSC 2021"。
    std::wstring familyName;
    if (product.rfind(L"Windows ", 0) == 0) {
        std::wstring rest = product.substr(8);
        size_t sp = rest.find(L' ');
        std::wstring fam =
            (sp == std::wstring::npos) ? rest : rest.substr(0, sp);
        if (isWin11)
            fam = L"11";
        familyName = L"Windows " + fam;
    }
    const wchar_t* zhEdition = ChineseEdition(edition);
    if (!familyName.empty() && zhEdition)
        d.full = familyName + L" " + zhEdition;
    else
        d.full = product;  // 认不出来就原样用 ProductName
    auto append = [&d](const std::wstring& s) {
        if (s.empty())
            return;
        if (!d.full.empty())
            d.full += L' ';
        d.full += s;
    };
    append(version);
    if (!build.empty()) {
        std::wstring b = build;
        if (ubr != 0xFFFFFFFFu && ubr > 0) {
            wchar_t u[16];
            swprintf(u, 16, L".%lu", (unsigned long)ubr);
            b += u;
        }
        append(b);
    }

    // 文件名用的短标记：Win<家族>.<build>
    int family = isWin11 ? 11 : 10;
    if (!isWin11 && product.rfind(L"Windows ", 0) == 0) {
        int n = 0;
        if (swscanf(product.c_str() + 8, L"%d", &n) == 1 && n >= 7)
            family = n;
    }
    if (!build.empty()) {
        wchar_t tag[32];
        swprintf(tag, 32, L"Win%d.%s", family, build.c_str());
        d.shortTag = tag;
    }
    return d;
}

}  // namespace sysrecover
