// 开机「快捷启动键」识别表与查询（详见 bioskey.h）。
#include "bioskey.h"

#include <windows.h>

#include "i18n.h"

namespace sysrecover {
namespace {

struct KeyEntry {
    const wchar_t* kw;      // 关键字（大写；对标识串做子串匹配）
    const wchar_t* vendor;  // 品牌名（展示/诊断用）
    const wchar_t* boot;    // 开机快捷启动键（空 = 表里没把握）
};

// 表顺序即优先级（同 BiosKC.c 的 match()：先命中先返回）。
const KeyEntry kTab[] = {
    {L"ASUSTEK", L"ASUS", L"F8"},
    {L"ASUS", L"ASUS", L"F8"},
    {L"MICRO-STAR", L"MSI", L"F11"},
    {L"MSI", L"MSI", L"F11"},
    {L"GIGABYTE", L"Gigabyte", L"F12"},
    {L"ASROCK", L"ASRock", L"F11"},
    {L"BIOSTAR", L"Biostar", L"F9"},
    {L"FOXCONN", L"Foxconn", L"F12"},
    {L"HON HAI", L"Foxconn", L"F12"},
    {L"SUPERMICRO", L"Supermicro", L"F11"},
    {L"SUPER MICRO", L"Supermicro", L"F11"},
    {L"INTEL", L"Intel", L"F10"},
    {L"COLORFUL", L"Colorful", L"F11"},
    {L"SOYO", L"SOYO", L"F11"},
    {L"ONDA", L"ONDA", L"F11"},
    {L"MAXSUN", L"MAXSUN", L"F11"},
    {L"YESTON", L"Yeston", L"F11"},
    {L"UNIKA", L"UNIKA", L"F11"},
    {L"GALAXY", L"Galaxy", L"F11"},
    {L"ELITEGROUP", L"ECS", L"F11"},
    {L"ECS", L"ECS", L"F11"},
    {L"JETWAY", L"Jetway", L"F11"},
    {L"ABIT", L"Abit", L"F11"},
    {L"DFI", L"DFI", L"F11"},
    {L"EPOX", L"Epox", L"F11"},
    {L"EVGA", L"EVGA", L"F12"},
    {L"PCCHIPS", L"PCChips", L"F11"},
    {L"SHUTTLE", L"Shuttle", L"F12"},
    {L"SOLTEK", L"Soltek", L"F11"},
    {L"TYAN", L"Tyan", L"F11"},
    {L"ZOTAC", L"Zotac", L"F11"},
    {L"AOPEN", L"AOpen", L"F11"},
    {L"CHAINTECH", L"Chaintech", L"F11"},
    {L"IWILL", L"Iwill", L"F11"},
    {L"MATSONIC", L"Matsonic", L"F11"},
    {L"QDI", L"QDI", L"F11"},
    {L"TRIGEM", L"Trigem", L"F11"},
    {L"WINFAST", L"WinFast", L"F11"},
    {L"LEADTEK", L"Leadtek", L"F11"},
    {L"PEGATRON", L"Pegatron", L"F11"},
    {L"SAPPHIRE", L"Sapphire", L"F11"},
    {L"POWERCOLOR", L"PowerColor", L"F11"},
    {L"XFX", L"XFX", L"F11"},
    {L"NVIDIA", L"NVIDIA", L"F11"},
    {L"AMD", L"AMD", L"F12"},
    {L"SIS", L"SiS", L"F11"},
    {L"VIA", L"VIA", L"F11"},
    {L"MITAC", L"MiTAC", L"F12"},
    {L"TATUNG", L"Tatung", L"F11"},
    {L"SURFACE", L"Microsoft Surface", L""},
    {L"MICROSOFT", L"Microsoft Surface", L""},
    {L"DELL", L"Dell", L"F12"},
    {L"LENOVO", L"Lenovo", L"F12 or Fn+F12"},
    {L"HPE", L"HP Enterprise", L"F11"},
    {L"HEWLETT", L"HP", L"F9"},
    {L"HPQ", L"HP", L"F9"},
    {L"HP", L"HP", L"F9"},
    {L"ACER", L"Acer", L"F12"},
    {L"TOSHIBA", L"Toshiba", L"F12"},
    {L"DYNABOOK", L"Dynabook", L"F12"},
    {L"SAMSUNG", L"Samsung", L"F10 or ESC"},
    {L"SONY", L"Sony", L"F11 or ESC"},
    {L"VAIO", L"VAIO", L"F11 or ESC"},
    {L"FUJITSU", L"Fujitsu", L"F12"},
    {L"PANASONIC", L"Panasonic", L"F12"},
    {L"NEC", L"NEC", L"F12"},
    {L"SHARP", L"Sharp", L"F12"},
    {L"HITACHI", L"Hitachi", L"F12"},
    {L"HUAWEI", L"Huawei", L"F12"},
    {L"HONOR", L"Honor", L"F12"},
    {L"XIAOMI", L"Xiaomi", L"F12"},
    {L"HASEE", L"Hasee", L"F12"},
    {L"MECHREVO", L"MECHREVO", L"F12"},
    {L"THUNDEROBOT", L"ThundeRobot", L"F12"},
    {L"MACHENIKE", L"MACHENIKE", L"F12"},
    {L"TONGFANG", L"Tongfang", L"F12"},
    {L"TSINGHUA", L"Tongfang", L"F12"},
    {L"CLEVO", L"Clevo", L"F12"},
    {L"HEDY", L"Hedy", L"F12"},
    {L"FOUNDER", L"Founder", L"F12"},
    {L"GREATWALL", L"Great Wall", L"F12"},
    {L"THTF", L"THTF", L"F12"},
    {L"RAZER", L"Razer", L"F12"},
    {L"ALIENWARE", L"Alienware", L"F12"},
    {L"GATEWAY", L"Gateway", L"F12"},
    {L"EMACHINES", L"eMachines", L"F12"},
    {L"PACKARD", L"Packard Bell", L"F12"},
    {L"MEDION", L"Medion", L"F12"},
    {L"SIEMENS", L"Fujitsu-Siemens", L"F12"},
    {L"COMPAQ", L"Compaq", L"F9"},
    {L"IBM", L"IBM", L"F12"},
    {L"BENQ", L"BenQ", L"F12"},
    {L"HAIER", L"Haier", L"F12"},
    {L"HISENSE", L"Hisense", L"F12"},
    {L"TCL", L"TCL", L"F12"},
    {L"KONKA", L"Konka", L"F12"},
    {L"LG", L"LG", L"F10"},
    {L"AIGO", L"Aigo", L"F12"},
    {L"TECLAST", L"Teclast", L"F12"},
    {L"CHUWI", L"CHUWI", L"F12"},
    {L"CUBE", L"Cube", L"F12"},
    {L"ILIFE", L"iLife", L"F12"},
    {L"DERE", L"DERE", L"F12"},
    {L"EMDOOR", L"Emdoor", L"F12"},
    {L"VOYO", L"VOYO", L"F12"},
    {L"ALLDOCUBE", L"Alldocube", L"F12"},
    {L"BMAX", L"BMAX", L"F12"},
    {L"JUMPER", L"Jumper", L"F12"},
    {L"HIGOLE", L"Higole", L"F12"},
    {L"FNF", L"FNF", L"F12"},
    {L"VIEWSONIC", L"ViewSonic", L"F12"},
    {L"POSITIVO", L"Positivo", L"F12"},
    {L"MULTILASER", L"Multilaser", L"F12"},
    {L"CCE", L"CCE", L"F12"},
    {L"SEMP", L"Semp Toshiba", L"F12"},
    {L"GRADIENTE", L"Gradiente", L"F12"},
    {L"PHILCO", L"Philco", L"F12"},
    {L"EXO", L"EXO", L"F12"},
    {L"AVELL", L"Avell", L"F12"},
    {L"ELUKTRONICS", L"Eluktronics", L"F12"},
    {L"CYBERPOWER", L"CyberPowerPC", L"F11"},
    {L"IBUYPOWER", L"iBuyPower", L"F11"},
    {L"ORIGIN", L"Origin PC", L"F12"},
    {L"MAINGEAR", L"Maingear", L"F11"},
    {L"DIGITALSTORM", L"Digital Storm", L"F11"},
    {L"FALCON", L"Falcon Northwest", L"F11"},
    {L"XMG", L"XMG", L"F12"},
    {L"SCHENKER", L"Schenker", L"F12"},
    {L"PCSPECIALIST", L"PCSpecialist", L"F12"},
    {L"PROSTAR", L"Prostar", L"F12"},
    {L"SAGER", L"Sager", L"F12"},
    {L"METABOX", L"Metabox", L"F12"},
    {L"SCAN", L"Scan", L"F11"},
    {L"CHILLBLAST", L"Chillblast", L"F11"},
    {L"YOYOTECH", L"Yoyotech", L"F11"},
    {L"WORTMANN", L"Wortmann", L"F12"},
    {L"MAXDATA", L"Maxdata", L"F12"},
    {L"BLUECHIP", L"BlueChip", L"F11"},
    {L"TERRA", L"Terra", L"F12"},
    {L"HYRICAN", L"Hyrican", L"F11"},
    {L"MIFCOM", L"MIFCOM", L"F11"},
    {L"TUXEDO", L"TUXEDO", L"F12"},
    {L"SYSTEM76", L"System76", L"F12"},
    {L"STARLABS", L"Star Labs", L"F12"},
    {L"SLIMBOOK", L"Slimbook", L"F12"},
    {L"ENTROWARE", L"Entroware", L"F12"},
    {L"JUNO", L"Juno", L"F12"},
    {L"FRAMEWORK", L"Framework", L"F12"},
    {L"WYSE", L"Dell Wyse", L"F12"},
    {L"BULL", L"Bull", L"F12"},
    {L"OLIVETTI", L"Olivetti", L"F12"},
    {L"HCL", L"HCL", L"F12"},
    {L"IBALL", L"iBall", L"F12"},
    {L"MICROMAX", L"Micromax", L"F12"},
    {L"LAVA", L"Lava", L"F12"},
    {L"WIPRO", L"Wipro", L"F12"},
    {L"WISTRON", L"Wistron", L"F12"},
    {L"COMPAL", L"Compal", L"F12"},
    {L"QUANTA", L"Quanta", L"F12"},
    {L"INVENTEC", L"Inventec", L"F12"},
    {L"GEEKOM", L"GEEKOM", L"F11"},
    {L"MINISFORUM", L"MINISFORUM", L"F11"},
    {L"BEELINK", L"Beelink", L"F11"},
    {L"TRIGKEY", L"Trigkey", L"F11"},
    {L"ACEMAGICIAN", L"Acemagician", L"F11"},
    {L"KAMRUI", L"KAMRUI", L"F11"},
    {L"GMKTEC", L"GMKtec", L"F11"},
    {L"MOREFINE", L"Morefine", L"F11"},
    {L"NIPOGI", L"Nipogi", L"F11"},
    {L"AWOW", L"AWOW", L"F11"},
    {L"CENAVA", L"Cenava", L"F11"},
    {L"CISCO", L"Cisco", L"F12"},
    {L"ORACLE", L"Oracle", L"F12"},
    {L"INSPIR", L"Inspur", L"F11"},
    {L"SUGON", L"Sugon", L"F11"},
    {L"H3C", L"H3C", L"F12"},
    {L"ZTE", L"ZTE", L"F11"},
    {L"THINKPAD", L"ThinkPad", L"F12"},
    {L"THINKCENTRE", L"Lenovo", L"F12"},
    {L"THINKSTATION", L"Lenovo", L"F12"},
    {L"THINKSERVER", L"Lenovo", L"F12"},
    {L"THINKSYSTEM", L"Lenovo", L"F12"},
    {L"IDEAPAD", L"Lenovo", L"F12"},
    {L"LEGION", L"Lenovo", L"F12"},
    {L"YOGA", L"Lenovo", L"F12"},
    {L"INSPIRON", L"Dell", L"F12"},
    {L"LATITUDE", L"Dell", L"F12"},
    {L"PRECISION", L"Dell", L"F12"},
    {L"OPTIPLEX", L"Dell", L"F12"},
    {L"XPS", L"Dell", L"F12"},
    {L"VOSTRO", L"Dell", L"F12"},
    {L"POWEREDGE", L"Dell PowerEdge", L"F11"},
    {L"PROBOOK", L"HP", L"F9"},
    {L"ELITEBOOK", L"HP", L"F9"},
    {L"PAVILION", L"HP", L"F9"},
    {L"SPECTRE", L"HP", L"F9"},
    {L"ENVY", L"HP", L"F9"},
    {L"OMEN", L"HP", L"F9"},
    {L"ZBOOK", L"HP", L"F9"},
    {L"PROLIANT", L"HP ProLiant", L"F11"},
    {L"ASPIRE", L"Acer", L"F12"},
    {L"PREDATOR", L"Acer", L"F12"},
    {L"TRAVELMATE", L"Acer", L"F12"},
    {L"SWIFT", L"Acer", L"F12"},
    {L"NITRO", L"Acer", L"F12"},
    {L"ESPRIMO", L"Fujitsu", L"F12"},
    {L"LIFEBOOK", L"Fujitsu", L"F12"},
    {L"STYLISTIC", L"Fujitsu", L"F12"},
    {L"PRIMERGY", L"Fujitsu", L"F12"},
    {L"CHROMEBOOK", L"Chromebook", L"ESC"},
    {L"POSITIVO BGH", L"Positivo BGH", L"F12"},
    {L"BGH", L"Positivo BGH", L"F12"},
    {L"NOBLEX", L"Noblex", L"F12"},
    {L"KEN BROWN", L"Ken Brown", L"F12"},
    {L"NEWSAN", L"Newsan", L"F12"},
};

std::wstring ToUpperAscii(std::wstring s) {
    for (wchar_t& c : s)
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    return s;
}

std::wstring RegStr(HKEY root, const wchar_t* sub, const wchar_t* name) {
    std::wstring out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    wchar_t b[512] = {};
    DWORD cb = sizeof(b) - 2, t = 0;
    if (RegQueryValueExW(k, name, nullptr, &t, (LPBYTE)b, &cb) == ERROR_SUCCESS &&
        b[0])
        out = b;
    RegCloseKey(k);
    return out;
}

}  // namespace

std::wstring LookupBootMenuKey(const std::wstring& baseBoardManufacturer,
                               const std::wstring& baseBoardProduct,
                               const std::wstring& systemManufacturer,
                               const std::wstring& systemProduct,
                               std::wstring* vendor) {
    const std::wstring* src[] = {&baseBoardManufacturer, &baseBoardProduct,
                                 &systemManufacturer, &systemProduct};
    for (const std::wstring* s : src) {
        if (s->empty()) continue;
        std::wstring up = ToUpperAscii(*s);
        for (const KeyEntry& e : kTab) {
            if (wcsstr(up.c_str(), e.kw)) {
                if (vendor) *vendor = e.vendor;
                return e.boot;
            }
        }
    }
    return std::wstring();
}

std::wstring LocalBootMenuKey(std::wstring* vendor) {
    static bool done = false;
    static std::wstring key;
    static std::wstring ven;
    if (!done) {
        const wchar_t* sub = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
        std::wstring bm = RegStr(HKEY_LOCAL_MACHINE, sub, L"BaseBoardManufacturer");
        std::wstring bp = RegStr(HKEY_LOCAL_MACHINE, sub, L"BaseBoardProduct");
        std::wstring sm = RegStr(HKEY_LOCAL_MACHINE, sub, L"SystemManufacturer");
        std::wstring sp = RegStr(HKEY_LOCAL_MACHINE, sub, L"SystemProductName");
        key = LookupBootMenuKey(bm, bp, sm, sp, &ven);
        done = true;
    }
    if (vendor) *vendor = ven;
    return key;
}

std::wstring BootMenuKeyHint() {
    static bool done = false;
    static std::wstring hint;
    if (!done) {
        std::wstring k = LocalBootMenuKey(nullptr);
        if (k.empty()) {
            hint = Tr(L"快捷启动键");
        } else {
            std::wstring orW = Tr(L" 或 ");
            std::wstring out;
            const std::wstring pat = L" or ";
            size_t i = 0;
            while (true) {
                size_t p = k.find(pat, i);
                if (p == std::wstring::npos) {
                    out += k.substr(i);
                    break;
                }
                out += k.substr(i, p - i) + orW;
                i = p + pat.size();
            }
            hint = out;
        }
        done = true;
    }
    return hint;
}

}  // namespace sysrecover
