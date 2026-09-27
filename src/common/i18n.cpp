#include "i18n.h"

#include <windows.h>

#include <mutex>
#include <unordered_map>

#include "selfarch.h"

namespace sysrecover {
namespace {

std::mutex g_mtx;
// UTF-8 key → UTF-8 value（词典本体）；宽串只是按需缓存，避免每次 Tr(L"…") 都转换。
std::unordered_map<std::string, std::string> g_dict;
std::unordered_map<std::wstring, std::wstring> g_wide;
std::string g_lang = "zh-CN";

std::string W2U(const wchar_t* w) {
    if (!w || !*w)
        return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s((size_t)(n - 1), '\0');  // n 含结尾 NUL，丢掉
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string W2U(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0,
                                  nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr,
                          nullptr);
    return s;
}

std::wstring U2W(const char* p, size_t len) {
    if (!p || len == 0)
        return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, p, (int)len, nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring s((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, p, (int)len, &s[0], n);
    return s;
}

std::wstring U2W(const std::string& s) {
    return U2W(s.data(), s.size());
}

// 语言标签归一：zh* → "zh-CN"（源语言），en* → "en"，其它原样（小写，如 ja/de/fr）。
std::string NormalizeLang(const char* s) {
    if (!s || !*s)
        return {};
    std::string v;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        v += c;
    }
    if (v.rfind("zh", 0) == 0)
        return "zh-CN";
    if (v.rfind("en", 0) == 0)
        return "en";
    return v;
}

std::string DetectLang() {
    // 显式开关优先：机房批量部署可统一设 SYSRECOVER_LANG=en（或 --lang，见 cli/main）。
    char buf[64] = {};
    DWORD n = ::GetEnvironmentVariableA("SYSRECOVER_LANG", buf, (DWORD)sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        std::string v = NormalizeLang(buf);
        if (!v.empty())
            return v;
    }
    // 系统 UI 语言（Win32，PE 也可用；不碰 WMI）：主语言 id 低 10 位 == 0x04 → 中文族。
    if ((::GetUserDefaultUILanguage() & 0x3FF) == 0x04)
        return "zh-CN";
    return "en";
}

// .lang 转义还原：\n \r \t \\ \=；其余 `\x` 原样保留（两个字符）。
// `\=` 是词条**键**里的等号（key=value 格式里首遇 '=' 即分割，
// 而 `备份失败(rc=%d): ` 这类键本身含 '='，必须转义），由
// tools/i18n-wrap.py::lang_escape 写入时生成。
std::string Unescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            o += s[i];
            continue;
        }
        char c = s[++i];
        if (c == 'n')
            o += '\n';
        else if (c == 'r')
            o += '\r';
        else if (c == 't')
            o += '\t';
        else if (c == '\\')
            o += '\\';
        else if (c == '=')
            o += '=';
        else {
            o += '\\';
            o += c;
        }
    }
    return o;
}

// 装载一段 .lang 文本（调用方持锁）。overwrite=false 时只补缺失键（回退词典用）。
void LoadTextLocked(const std::string& text, bool overwrite) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        if (eol == std::string::npos)
            eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;
        // 首个**未转义**的 '=' 才是键值分隔符（`\` 会转义紧随其后的字符），
        // 否则 `备份失败(rc\=%d): =...` 会被从键中间截断。
        size_t eq = std::string::npos;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\') {
                ++i;  // 跳过被转义的字符
                continue;
            }
            if (line[i] == '=') {
                eq = i;
                break;
            }
        }
        if (eq == std::string::npos || eq == 0)
            continue;
        std::string key = Unescape(line.substr(0, eq));
        if (!overwrite && g_dict.find(key) != g_dict.end())
            continue;
        g_dict[key] = Unescape(line.substr(eq + 1));
    }
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz = {};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 8 * 1024 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = out.empty()
                  ? TRUE
                  : ::ReadFile(h, &out[0], (DWORD)out.size(), &rd, nullptr);
    ::CloseHandle(h);
    return ok && rd == out.size();
}

void LoadFileLocked(const std::wstring& path, bool overwrite) {
    std::string bytes;
    if (!ReadWholeFile(path, bytes))
        return;
    size_t skip = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
                   (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
                      ? 3
                      : 0;
    LoadTextLocked(bytes.substr(skip), overwrite);
}

}  // namespace

void InitI18n(const char* forced) {
    std::string lang = forced && *forced ? NormalizeLang(forced) : std::string();
    if (lang.empty())
        lang = DetectLang();
    if (lang.empty())
        lang = "en";

    std::lock_guard<std::mutex> lk(g_mtx);
    g_dict.clear();
    g_wide.clear();
    g_lang = lang;
    if (lang == "zh-CN")
        return;  // 源语言：不载词典（源码字面量即译文，天然回退）
    std::wstring root = AppDir();
    if (lang != "en")
        LoadFileLocked(root + L"\\lang\\" + U2W(lang) + L".lang", true);
    LoadFileLocked(root + L"\\lang\\en.lang", false);  // 通用回退（ja → en → 源文本）
}

const char* LangTag() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_lang.c_str();
}

bool IsSourceLang() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_lang == "zh-CN";
}

const char* Tr(const char* src) {
    if (!src)
        return "";
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dict.empty())
        return src;
    auto it = g_dict.find(src);
    return it != g_dict.end() ? it->second.c_str() : src;
}

const wchar_t* Tr(const wchar_t* src) {
    if (!src)
        return L"";
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dict.empty())
        return src;
    auto wit = g_wide.find(src);
    if (wit != g_wide.end())
        return wit->second.c_str();
    std::string key = W2U(src);
    auto it = g_dict.find(key);
    if (it == g_dict.end())
        return src;
    std::wstring& slot = g_wide[src];  // 节点地址不受 rehash 影响，可长期返回
    slot = U2W(it->second);
    return slot.c_str();
}

void ClearI18n() {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dict.clear();
    g_wide.clear();
}

void LoadLangText(const std::string& langText) {
    std::lock_guard<std::mutex> lk(g_mtx);
    LoadTextLocked(langText, true);
}

std::wstring LoadSkinXml(const wchar_t* skinFileName) {
    if (!skinFileName || !*skinFileName)
        return L"";
    std::string bytes;
    if (!ReadWholeFile(AppDir() + L"\\skin\\" + skinFileName, bytes))
        return L"";
    size_t skip = (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
                   (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF)
                      ? 3
                      : 0;
    std::wstring xml = U2W(bytes.data() + skip, bytes.size() - skip);

    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_dict.empty())
        return xml;  // 中文：原样（仍走宽串内存解析，与原 LoadFromFile 等价）

    // 把每个 `"…"` 引号段当候选（皮肤文本都在属性值里；`<?xml encoding="utf-8"?>`
    // 之类 ASCII 值查不到键 → 原样）。键即源文本，所以查找用引号里的原文。
    std::wstring out;
    out.reserve(xml.size() + 64);
    for (size_t i = 0; i < xml.size();) {
        if (xml[i] != L'"') {
            out += xml[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < xml.size() && xml[j] != L'"')
            ++j;
        if (j >= xml.size()) {  // 未闭合（不应发生）：原样收尾
            out += xml.substr(i);
            break;
        }
        std::wstring val = xml.substr(i + 1, j - i - 1);
        auto it = g_dict.find(W2U(val));
        out += L'"';
        out += (it != g_dict.end()) ? U2W(it->second) : val;
        out += L'"';
        i = j + 1;
    }
    return out;
}

}  // namespace sysrecover
