#pragma once
// BCD 操作（bcdedit 封装）。成熟方案见 AGENTS.md §7，严禁发明。
// 条目存在性：bcdedit /enum {GUID} 看输出是否回显 GUID（两者退出码都 0）。
#include <string>

namespace sysrecover {

// 固定恢复条目 GUID（与旧原型一致，便于对照）。
inline const wchar_t* RecoveryGuid() {
    return L"{12345678-1234-1234-1234-123456789abc}";
}

// 解析 `bcdedit /enum {bootmgr}` 输出里的 timeout（秒），找不到 → -1（PIT-094）。
// **不依赖标签语言**：timeout 是 {bootmgr} 枚举里唯一「整行恰好两个空白分隔
// token、且第二个 token 全为数字（≤1 天）」的元素 —— device/locale/description
// 等第二个 token 都不是纯数字，displayorder 的续行只有一个 token，表头/分隔线
// 不是两 token 或第二 token 非数字。纯逻辑，单测覆盖。
inline int ParseBootmgrTimeout(const std::string& out) {
    auto isWs = [](char c) { return c == ' ' || c == '\t'; };
    size_t pos = 0;
    while (pos < out.size()) {
        size_t eol = out.find_first_of("\r\n", pos);
        if (eol == std::string::npos) eol = out.size();
        size_t a = pos;
        while (a < eol && isWs(out[a])) ++a;
        size_t b = a;
        while (b < eol && !isWs(out[b])) ++b;
        if (b > a && b < eol) {  // 有 token1，后面还有内容
            size_t c = b;
            while (c < eol && isWs(out[c])) ++c;
            size_t d = c;
            while (d < eol && !isWs(out[d])) ++d;
            size_t e = d;
            while (e < eol && isWs(out[e])) ++e;
            if (d > c && e == eol) {  // token2 后到行尾只有空白
                int v = 0;
                bool ok = true;
                for (size_t i = c; i < d; ++i) {
                    if (out[i] < '0' || out[i] > '9') {
                        ok = false;
                        break;
                    }
                    v = v * 10 + (out[i] - '0');
                    if (v > 86400) {  // 不可能是 timeout 秒数
                        ok = false;
                        break;
                    }
                }
                if (ok) return v;
            }
        }
        if (eol == out.size()) break;
        pos = eol + 1;
    }
    return -1;
}

// 条目是否存在（ASCII GUID 匹配，不受代码页影响）。
bool BcdEntryExists(const std::wstring& guid);

// 读系统存储的 `{bootmgr} timeout`（秒）；解析不到返回 -1（原文追加进 log）。
int BcdGetBootmgrTimeout(std::string& log);

// 写 `{bootmgr} timeout`（bcdedit /set {bootmgr} timeout N）。
bool BcdSetBootmgrTimeout(int seconds, std::string& log);

// 创建/刷新实模式启动扇区条目：device partition=X: + path \grldr.mbr + displayorder。
// **条目已存在时也会重设 device/path**（幂等自愈）：防止换机/换盘符/旧布局残留
// 造成"文件在新盘、entry 指旧盘"→ bootmgr 0xc000000F。
bool BcdCreateBootsector(const std::wstring& guid, const std::wstring& desc,
                         wchar_t driveLetter, std::string& log);

// 单次启动（BIOS 路径；UEFI 用固件 BootNext，见 boot/uefi.h）。
bool BcdSetBootsequence(const std::wstring& guid);

// 备份 BCD（bcdedit /export），失败不致命。
bool BcdExport(const std::wstring& backupPath);

}  // namespace sysrecover
