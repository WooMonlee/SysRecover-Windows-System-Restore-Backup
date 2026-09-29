#include "bcd_parse.h"

#include <cctype>
#include <cstring>
#include <string>

namespace sysrecover {
namespace {

std::string Lower(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s)
        o += (char)std::tolower((unsigned char)c);
    return o;
}

// label（已小写）出现在**行首**（允许前导空格），且同一行里跟了 `{…}`（≥1 字符）。
// bcdedit 的输出形如：
//   displayorder            {default}
//                           {18c8dcd6-…}
// —— 只要第一个 GUID 在同一行就够判"非空"。
// 也接受中文标签（部分语言/版本会本地化字段名，UTF-8 字节写死在这里）。
bool LabelLineHasGuid(const std::string& lowerText, const char* label) {
    const size_t len = std::strlen(label);
    for (size_t pos = lowerText.find(label); pos != std::string::npos;
         pos = lowerText.find(label, pos + 1)) {
        size_t lineStart = lowerText.rfind('\n', pos);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        bool onlyIndent = true;
        for (size_t i = lineStart; i < pos; ++i) {
            if (!std::isspace((unsigned char)lowerText[i])) {
                onlyIndent = false;
                break;
            }
        }
        if (!onlyIndent)
            continue;
        size_t lineEnd = lowerText.find('\n', pos);
        if (lineEnd == std::string::npos)
            lineEnd = lowerText.size();
        size_t b = lowerText.find('{', pos);
        if (b == std::string::npos || b > lineEnd)
            continue;
        size_t e = lowerText.find('}', b);
        if (e != std::string::npos && e > b + 1)
            return true;
    }
    (void)len;
    return false;
}

// 中文标签 "显示顺序"（UTF-8），以及 "启动管理器" 之类不参与判定。
const char kDisplayOrderZh[] = "\xe6\x98\xbe\xe7\xa4\xba\xe9\xa1\xba\xe5\xba\x8f";

}  // namespace

bool BcdEnumShowsOsEntry(const std::string& enumText, std::string* why) {
    auto fail = [&](const char* m) {
        if (why)
            *why = m;
        return false;
    };
    if (enumText.empty())
        return fail("bcdedit produced no output");

    const std::string low = Lower(enumText);
    if (!LabelLineHasGuid(low, "displayorder") &&
        !LabelLineHasGuid(low, kDisplayOrderZh))
        return fail("BCD has no non-empty displayorder");

    if (low.find("winload.") == std::string::npos)
        return fail("BCD has no Windows Boot Loader (winload) entry");

    return true;
}

}  // namespace sysrecover
