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

// ── PIT-208：BCD 的启动项到底指向哪个分区 / 用的是不是 UEFI 加载器 ────────
namespace {

std::string TrimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// bcdedit 的输出是"字段名 + 值"，值的续行是缩进的（displayorder 后面几行
// 只有 {GUID}）。压成逻辑行后，一个字段就只剩一条，便于按记录切分。
struct LogicalLine {
    std::string key;    // 已小写
    std::string value;
    bool sep = false;   // "--------" 分隔线：一个对象块从这里开始
};

std::vector<LogicalLine> ToLogicalLines(const std::string& text) {
    std::vector<LogicalLine> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos)
            e = text.size();
        std::string raw = text.substr(i, e - i);
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        const size_t first = raw.find_first_not_of(" \t");
        const bool blank = (first == std::string::npos);
        // "Windows Boot Manager" 下面的那行 ----- 是分隔线，不是字段
        const bool dashes =
            !blank && raw.find_first_not_of(" -\t") == std::string::npos;
        if (dashes) {
            LogicalLine ll;
            ll.sep = true;
            out.push_back(ll);
        } else if (!blank) {
            if (first > 0 && !out.empty() && !out.back().sep) {
                if (!out.back().value.empty())
                    out.back().value += ' ';
                out.back().value += raw.substr(first);
            } else {
                LogicalLine ll;
                const size_t sp = raw.find_first_of(" \t");
                ll.key = Lower(sp == std::string::npos ? raw
                                                       : raw.substr(0, sp));
                if (sp != std::string::npos)
                    ll.value = TrimCopy(raw.substr(sp + 1));
                out.push_back(ll);
            }
        }
        if (e >= text.size())
            break;
        i = e + 1;
    }
    return out;
}

struct BcdRecord {
    std::string id;                     // 含花括号，原样
    std::vector<std::string> devices;   // device + osdevice 的值
    std::vector<std::string> paths;     // winload 路径（原样）
    std::vector<std::string> order;     // displayorder + default 里的 GUID
    std::string def;                    // default 指向的 GUID（可空）
    bool isLoader = false;              // path 含 winload.
    bool isBootMgr = false;             // path 含 bootmgfw / bootmgr
};

// 字段名在部分语言/版本会本地化；值（路径）不会。
bool KeyIs(const std::string& k, const char* en, const char* zh) {
    return k == en || (zh && k == zh);
}

std::vector<std::string> GuidsIn(const std::string& v) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = v.find('{', p)) != std::string::npos) {
        const size_t q = v.find('}', p);
        if (q == std::string::npos)
            break;
        out.push_back(v.substr(p, q - p + 1));
        p = q + 1;
    }
    return out;
}

std::vector<BcdRecord> ParseRecords(const std::string& text) {
    std::vector<BcdRecord> recs;
    // 块的**第一行永远是 identifier**（bcdedit 的固定排版），但字段名本身
    // 会本地化（中文 Windows 上是"标识符"）——所以这里不看字段名，直接取
    // 这一行里的第一个 {…}。2026-11-02 实验室实测：只认英文 "identifier"
    // 会在中文系统上一个记录都解析不出来（等于校验形同虚设）。
    bool atBlockStart = false;
    for (const auto& ll : ToLogicalLines(text)) {
        if (ll.sep) {
            atBlockStart = true;
            continue;
        }
        // 两种起点都认：分隔线后的第一行（语言无关），或直接认出 identifier 字段
        // （测试/精简输出里可能没有分隔线）。
        const bool idKey =
            KeyIs(ll.key, "identifier",
                  "\xe6\xa0\x87\xe8\xaf\x86\xe7\xac\xa6");  // 标识符
        if (atBlockStart || idKey) {
            atBlockStart = false;
            const std::vector<std::string> ids = GuidsIn(ll.value);
            if (!ids.empty()) {
                BcdRecord r;
                r.id = ids.front();
                recs.push_back(r);
            }
            continue;
        }
        if (recs.empty())
            continue;
        BcdRecord& r = recs.back();
        // device / osdevice / filedevice 的值都长这样，与字段名语言无关
        if (ll.value.find("partition=") != std::string::npos)
            r.devices.push_back(ll.value);
        if (KeyIs(ll.key, "displayorder",
                  "\xe6\x98\xbe\xe7\xa4\xba\xe9\xa1\xba\xe5\xba\x8f")) {  // 显示顺序
            for (const auto& g : GuidsIn(ll.value))
                r.order.push_back(g);
        } else if (KeyIs(ll.key, "default", "\xe9\xbb\x98\xe8\xae\xa4")) {  // 默认
            for (const auto& g : GuidsIn(ll.value)) {
                r.order.push_back(g);
                if (r.def.empty())
                    r.def = g;
            }
        }
        // 路径值不本地化：直接按内容认
        const std::string lowv = Lower(ll.value);
        if (lowv.find("winload.") != std::string::npos) {
            r.isLoader = true;
            r.paths.push_back(ll.value);
        }
        if (lowv.find("bootmgfw") != std::string::npos ||
            lowv.find("bootmgr.efi") != std::string::npos ||
            lowv.find("\\bootmgr") != std::string::npos)
            r.isBootMgr = true;
    }
    return recs;
}

struct ReachResult {
    std::vector<const BcdRecord*> loaders;  // 可达的 Windows Boot Loader
    const BcdRecord* defaultRec = nullptr;  // {bootmgr} 的 default 解析结果
    int dangling = 0;                       // displayorder 指向不存在的条目
    bool hasOrder = false;
};

ReachResult CollectReachable(const std::vector<BcdRecord>& recs) {
    ReachResult rr;
    auto find = [&](const std::string& id) -> const BcdRecord* {
        const std::string low = Lower(id);
        for (const auto& r : recs)
            if (Lower(r.id) == low)
                return &r;
        return nullptr;
    };

    // 起点 = 启动管理器（path 含 bootmgfw/bootmgr）。找不到就当所有记录都是起点
    //（BIOS BCD 里 {bootmgr} 的 path 是 \bootmgr，同样能认出来）。
    std::vector<const BcdRecord*> roots;
    for (const auto& r : recs)
        if (r.isBootMgr)
            roots.push_back(&r);
    if (roots.empty())
        for (const auto& r : recs)
            roots.push_back(&r);

    // 广度优先沿 displayorder/default 走，允许嵌套（多 Windows / 链式引导）。
    std::vector<std::string> queue;
    for (const auto* r : roots) {
        for (const auto& g : r->order)
            queue.push_back(g);
        if (!r->def.empty() && !rr.defaultRec)
            rr.defaultRec = find(r->def);
    }
    rr.hasOrder = !queue.empty();
    // 极少见：根记录自己就是加载器（BCD 里没有 {bootmgr} 时）
    if (!rr.hasOrder)
        for (const auto* r : roots)
            if (r->isLoader)
                rr.loaders.push_back(r);

    std::vector<std::string> seen;
    while (!queue.empty()) {
        const std::string id = queue.back();
        queue.pop_back();
        const std::string low = Lower(id);
        bool dup = false;
        for (const auto& s : seen)
            if (s == low) {
                dup = true;
                break;
            }
        if (dup)
            continue;
        seen.push_back(low);
        const BcdRecord* r = find(id);
        if (!r) {
            ++rr.dangling;
            continue;
        }
        if (r->isLoader)
            rr.loaders.push_back(r);
        for (const auto& g : r->order)
            queue.push_back(g);
    }
    return rr;
}

}  // namespace

bool BcdOsEntryTargetsVolume(const std::string& enumText,
                             const std::vector<std::string>& candidates,
                             std::string* why) {
    auto fail = [&](const std::string& m) {
        if (why)
            *why = m;
        return false;
    };
    if (enumText.empty())
        return fail("bcdedit produced no output");

    const std::vector<BcdRecord> recs = ParseRecords(enumText);
    if (recs.empty())
        return fail("bcdedit output has no identifier records");

    const ReachResult rr = CollectReachable(recs);
    if (rr.loaders.empty()) {
        if (!rr.hasOrder)
            return fail("BCD has no displayorder/default entry to boot");
        if (rr.dangling > 0)
            return fail("displayorder/default points at entries that do not "
                        "exist");
        return fail("displayorder/default has no Windows Boot Loader (winload) "
                    "entry");
    }

    // 收集实际 device 值；有 device 行才可比。
    std::vector<std::string> devs;
    for (const auto* r : rr.loaders)
        for (const auto& d : r->devices)
            devs.push_back(d);
    if (devs.empty()) {
        if (why)
            *why = "BCD has no device/osdevice lines; device check skipped";
        return true;
    }
    if (candidates.empty()) {
        if (why)
            *why = "target volume identity unavailable; device check skipped";
        return true;
    }

    for (const auto& d : devs) {
        const std::string lowd = Lower(d);
        for (const auto& c : candidates) {
            if (c.empty())
                continue;
            if (lowd.find(Lower(c)) != std::string::npos)
                return true;
        }
    }
    return fail("the boot entry points at \"" + devs.front() +
                "\", not at the restored partition");
}

bool BcdOsEntryIsUefiLoader(const std::string& enumText, std::string* why) {
    auto fail = [&](const std::string& m) {
        if (why)
            *why = m;
        return false;
    };
    if (enumText.empty())
        return fail("bcdedit produced no output");

    const std::vector<BcdRecord> recs = ParseRecords(enumText);
    if (recs.empty())
        return fail("bcdedit output has no identifier records");

    const ReachResult rr = CollectReachable(recs);
    if (rr.loaders.empty())
        return fail("no reachable Windows Boot Loader (winload) entry");

    // 优先只看 {bootmgr} 的 default —— 开机默认进的就是它。
    std::vector<const BcdRecord*> check;
    if (rr.defaultRec && rr.defaultRec->isLoader)
        check.push_back(rr.defaultRec);
    else
        check = rr.loaders;

    for (const auto* r : check) {
        for (const auto& p : r->paths) {
            if (Lower(p).find("winload.efi") != std::string::npos)
                return true;
        }
    }
    std::string sample = check.empty() || check.front()->paths.empty()
                             ? std::string("?")
                             : check.front()->paths.front();
    return fail("boot loader path is \"" + sample +
                "\", not winload.efi (BCD was built for BIOS boot?)");
}

}  // namespace sysrecover
