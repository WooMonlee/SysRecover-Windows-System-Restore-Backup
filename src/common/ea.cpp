// EA 跨层修复核心实现（PIT-122/123，2026-10-06）。见 ea.h 的设计说明。
// 关键事实（全部实测）：
//   · NtSetEaFile 缓冲 = FILE_FULL_EA_INFORMATION **链**（逐条 4 字节对齐，
//     末条 NextEntryOffset=0；无外层"头"条目）—— 对齐 go-winio/restic 的实现，
//     且 tools/ea-scan.cpp --set 的单条目版本已在本机 fsutil 复核通过。
//   · registry.pol 记录 = `[key;\0 name;\0 type(4LE) ;\0 size(2LE)+00 00 ;\0 data ]\0`
//     （UTF-16LE 分号分隔；头部 "PReg" + u32 1）。实测 QEMU 启动生效（PIT-123）。
#include "ea.h"

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "version.h"

namespace sysrecover {
namespace ea {
namespace {

const long kStatusNoEas = (long)0xC0000052;      // STATUS_NO_EAS_ON_FILE
const long kStatusBufferOverflow = (long)0x80000005;

// NtSetEaFile 是写 EA 的唯一用户态通道（SetFileInformationByHandle 不支持 set）。
extern "C" long __stdcall NtSetEaFile(HANDLE, void*, void*, unsigned long);
extern "C" long __stdcall NtQueryEaFile(HANDLE, void*, void*, unsigned long,
                                        unsigned char, void*, unsigned long,
                                        unsigned long*, unsigned char);

void PutU32(std::vector<unsigned char>& v, unsigned long x) {
    v.push_back((unsigned char)(x & 0xFF));
    v.push_back((unsigned char)((x >> 8) & 0xFF));
    v.push_back((unsigned char)((x >> 16) & 0xFF));
    v.push_back((unsigned char)((x >> 24) & 0xFF));
}
unsigned long GetU32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

// registry.pol 单条记录（REG_DWORD）。
void PutPolDword(std::vector<unsigned char>& out, const char* key,
                 const char* name, unsigned long value) {
    auto puts16 = [&](const char* s) {
        for (; *s; ++s) {
            out.push_back((unsigned char)*s);
            out.push_back(0);
        }
        out.push_back(0);
        out.push_back(0);
    };
    out.push_back(0x5B);  // '['
    out.push_back(0x00);
    puts16(key);
    out.push_back(0x3B);
    out.push_back(0x00);
    puts16(name);
    out.push_back(0x3B);
    out.push_back(0x00);
    PutU32(out, 4);  // REG_DWORD
    out.push_back(0x3B);
    out.push_back(0x00);
    PutU32(out, 4);  // size = 4（低 16 位有效 + 2 padding）
    out.push_back(0x3B);
    out.push_back(0x00);
    PutU32(out, value);
    out.push_back(0x5D);  // ']'
    out.push_back(0x00);
}

std::string LowerAscii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool WriteWithRetry(const std::wstring& path,
                    const std::vector<unsigned char>& data, int tries = 5) {
    for (int i = 0; i < tries; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            BOOL ok = data.empty()
                          ? TRUE
                          : WriteFile(h, data.data(), (DWORD)data.size(), &wr,
                                      nullptr);
            CloseHandle(h);
            if (ok && (data.empty() || wr == data.size())) return true;
        }
        Sleep(200);
    }
    return false;
}

bool ReadWithRetry(const std::wstring& path, std::vector<unsigned char>& out,
                   int tries = 5) {
    for (int i = 0; i < tries; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz = {};
            if (GetFileSizeEx(h, &sz) && sz.QuadPart <= (256LL << 20)) {
                out.resize((size_t)sz.QuadPart);
                DWORD rd = 0, total = 0;
                BOOL ok = TRUE;
                while (total < out.size()) {
                    if (!ReadFile(h, out.data() + total,
                                  (DWORD)std::min<size_t>(out.size() - total,
                                                          1u << 20),
                                  &rd, nullptr)) {
                        ok = FALSE;
                        break;
                    }
                    if (rd == 0) break;
                    total += rd;
                }
                CloseHandle(h);
                if (ok && total == out.size()) return true;
                return false;
            }
            CloseHandle(h);
        }
        Sleep(200);
    }
    return false;
}

// 解析一条 pol 记录；返回记录总字节数（含 "]\\0"），0 = 解析失败。
size_t ParsePolRecord(const unsigned char* p, size_t avail,
                      std::string* key, std::string* name) {
    auto readU16Str = [&](size_t off, std::string& s) -> size_t {
        size_t i = off;
        s.clear();
        while (i + 2 <= avail) {
            unsigned char lo = p[i], hi = p[i + 1];
            if (lo == 0 && hi == 0) return i + 2;  // 终止
            if (hi != 0) return 0;                 // 非 UTF-16LE ASCII 文本
            s.push_back((char)lo);
            i += 2;
        }
        return 0;
    };
    if (avail < 2 || p[0] != 0x5B || p[1] != 0x00) return 0;
    size_t off = 2;
    off = readU16Str(off, *key);
    if (!off) return 0;
    if (off + 2 > avail || p[off] != 0x3B || p[off + 1] != 0x00) return 0;
    off += 2;
    off = readU16Str(off, *name);
    if (!off) return 0;
    if (off + 2 > avail || p[off] != 0x3B || p[off + 1] != 0x00) return 0;
    off += 2;
    if (off + 4 > avail) return 0;
    off += 4;  // type
    if (off + 2 > avail || p[off] != 0x3B || p[off + 1] != 0x00) return 0;
    off += 2;
    if (off + 4 > avail) return 0;
    unsigned long dataLen = p[off] | ((unsigned long)p[off + 1] << 8);
    off += 4;  // size（2 字节 + 2 padding）
    if (off + 2 > avail || p[off] != 0x3B || p[off + 1] != 0x00) return 0;
    off += 2;
    if (off + dataLen + 2 > avail) return 0;
    off += dataLen;
    if (p[off] != 0x5D || p[off + 1] != 0x00) return 0;
    return off + 2;
}

// FILE_FULL_EA_INFORMATION 链（go-winio 同款：逐条 4 字节对齐）。
void PutEaEntry(std::vector<unsigned char>& out, const EaValue& v, bool last) {
    size_t entryOff = out.size();
    out.resize(entryOff + 8 + v.name.size() + 1 + v.value.size() + 3);
    size_t raw = 8 + v.name.size() + 1 + v.value.size();
    size_t padded = (raw + 3) & ~(size_t)3;
    unsigned char* e = out.data() + entryOff;
    unsigned long next = last ? 0 : (unsigned long)padded;
    e[0] = (unsigned char)(next & 0xFF);
    e[1] = (unsigned char)((next >> 8) & 0xFF);
    e[2] = (unsigned char)((next >> 16) & 0xFF);
    e[3] = (unsigned char)((next >> 24) & 0xFF);
    e[4] = 0;  // Flags
    e[5] = (unsigned char)v.name.size();
    e[6] = (unsigned char)(v.value.size() & 0xFF);         // EaValueLength
    e[7] = (unsigned char)((v.value.size() >> 8) & 0xFF);  // （u16 已限）
    memcpy(e + 8, v.name.data(), v.name.size());
    e[8 + v.name.size()] = 0;
    if (!v.value.empty())
        memcpy(e + 8 + v.name.size() + 1, v.value.data(), v.value.size());
    out.resize(entryOff + padded, 0);
}

}  // namespace

std::vector<unsigned char> BuildPack(const std::vector<FileEntry>& files) {
    std::vector<unsigned char> out;
    const char magic[8] = {'Z', 'J', 'E', 'A', '1', 0, 0, 0};
    out.insert(out.end(), magic, magic + 8);
    PutU32(out, (unsigned long)files.size());
    for (const auto& f : files) {
        PutU32(out, (unsigned long)(f.relPath.size() * 2));
        const unsigned char* p = (const unsigned char*)f.relPath.data();
        out.insert(out.end(), p, p + f.relPath.size() * 2);
        PutU32(out, (unsigned long)f.eas.size());
        for (const auto& e : f.eas) {
            out.push_back((unsigned char)e.name.size());
            out.push_back(0);  // flags
            PutU32(out, (unsigned long)e.value.size());
            out.insert(out.end(), e.name.begin(), e.name.end());
            out.insert(out.end(), e.value.begin(), e.value.end());
        }
    }
    return out;
}

bool ParsePack(const unsigned char* data, size_t len,
               std::vector<FileEntry>& out, std::string& why) {
    out.clear();
    const unsigned long kMaxFiles = 2000000;
    const unsigned long kMaxPathBytes = 65536;
    const unsigned long kMaxEas = 4096;
    const unsigned long kMaxValue = 65535;
    if (len < 12 || memcmp(data, "ZJEA1", 5) != 0) {
        why = "bad magic";
        return false;
    }
    size_t off = 8;
    if (off + 4 > len) {
        why = "truncated fileCount";
        return false;
    }
    unsigned long fileCount = GetU32(data + off);
    off += 4;
    if (fileCount > kMaxFiles) {
        why = "fileCount too large";
        return false;
    }
    out.reserve(fileCount);
    for (unsigned long i = 0; i < fileCount; ++i) {
        if (off + 4 > len) {
            why = "truncated pathLen";
            return false;
        }
        unsigned long pb = GetU32(data + off);
        off += 4;
        if (pb > kMaxPathBytes || (pb & 1) || off + pb > len) {
            why = "bad pathLen";
            return false;
        }
        FileEntry fe;
        fe.relPath.assign((const wchar_t*)(data + off), pb / 2);
        off += pb;
        if (off + 4 > len) {
            why = "truncated eaCount";
            return false;
        }
        unsigned long eaCount = GetU32(data + off);
        off += 4;
        if (eaCount > kMaxEas) {
            why = "eaCount too large";
            return false;
        }
        for (unsigned long j = 0; j < eaCount; ++j) {
            if (off + 6 > len) {
                why = "truncated ea header";
                return false;
            }
            unsigned char nameLen = data[off];
            unsigned long valLen = GetU32(data + off + 2);
            off += 6;
            if (nameLen == 0 || valLen > kMaxValue || off + nameLen + valLen > len) {
                why = "bad ea body";
                return false;
            }
            EaValue ev;
            ev.name.assign((const char*)(data + off), nameLen);
            off += nameLen;
            ev.value.assign(data + off, data + off + valLen);
            off += valLen;
            fe.eas.push_back(std::move(ev));
        }
        out.push_back(std::move(fe));
    }
    return true;
}

std::vector<unsigned char> BuildSyncPolFragment() {
    std::vector<unsigned char> out;
    const char magic[8] = {'P', 'R', 'e', 'g', 1, 0, 0, 0};
    out.insert(out.end(), magic, magic + 8);
    PutPolDword(out,
                "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                "RunStartupScriptSync", 1);
    PutPolDword(
        out, "Software\\Policies\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        "SyncForegroundPolicy", 1);
    return out;
}

std::vector<unsigned char> StripOurPolRecords(
    const std::vector<unsigned char>& data, bool* removedAny) {
    if (removedAny) *removedAny = false;
    if (data.size() < 8 || memcmp(data.data(), "PReg", 4) != 0) return data;
    // 目标记录（小写化比较）
    const std::string kKey1 =
        LowerAscii(
            "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System");
    const std::string kKey2 = LowerAscii(
        "Software\\Policies\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon");
    const std::string kName1 = LowerAscii("RunStartupScriptSync");
    const std::string kName2 = LowerAscii("SyncForegroundPolicy");

    std::vector<unsigned char> out(data.begin(), data.begin() + 8);
    size_t off = 8;
    while (off < data.size()) {
        std::string key, name;
        size_t recLen = ParsePolRecord(data.data() + off, data.size() - off,
                                       &key, &name);
        if (recLen == 0) return data;  // 解析失败 → 原样返回（不动未知内容）
        bool ours = (LowerAscii(key) == kKey1 && LowerAscii(name) == kName1) ||
                    (LowerAscii(key) == kKey2 && LowerAscii(name) == kName2);
        if (ours) {
            if (removedAny) *removedAny = true;
        } else {
            out.insert(out.end(), data.begin() + off,
                       data.begin() + off + recLen);
        }
        off += recLen;
    }
    return out;
}

bool RemoveScriptEntry(std::string& text, const std::string& scriptName) {
    const std::string want = LowerAscii(scriptName);
    std::vector<std::string> lines;
    {
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) {
                lines.push_back(text.substr(i));
                break;
            }
            lines.push_back(text.substr(i, j - i + 1));
            i = j + 1;
        }
    }
    auto stripCR = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
        return s;
    };
    // 先找我们的 cmdLine 行及其编号
    std::vector<bool> drop(lines.size(), false);
    bool changed = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = stripCR(lines[i]);
        std::string lt = LowerAscii(t);
        size_t eq = lt.find("cmdline=");
        if (eq == std::string::npos) continue;
        std::string num = lt.substr(0, eq);
        if (num.empty() ||
            num.find_first_not_of("0123456789") != std::string::npos)
            continue;
        if (lt.substr(eq + 8) != want) continue;
        drop[i] = true;
        changed = true;
        // 同编号的 parameters 行
        std::string pnum = lt.substr(0, eq) + "parameters=";
        for (size_t k = 0; k < lines.size(); ++k)
            if (LowerAscii(stripCR(lines[k])).rfind(pnum, 0) == 0) drop[k] = true;
    }
    if (!changed) return false;
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
        if (!drop[i]) out += lines[i];
    text = out;
    return true;
}

bool BumpGptIniVersion(std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find('\n', i);
        size_t end = (j == std::string::npos) ? text.size() : j + 1;
        std::string line = text.substr(i, end - i);
        std::string t = line;
        while (!t.empty() && (t.back() == '\r' || t.back() == '\n')) t.pop_back();
        if (LowerAscii(t).rfind("version=", 0) == 0) {
            std::string num = t.substr(8);
            if (!num.empty() &&
                num.find_first_not_of("0123456789") == std::string::npos) {
                unsigned long long v = strtoull(num.c_str(), nullptr, 10);
                std::string nv = std::to_string(v + 1);
                std::string nl = "Version=" + nv;
                if (line.size() >= 2 && line.back() == '\n' &&
                    line[line.size() - 2] == '\r')
                    nl += "\r\n";
                else if (!line.empty() && line.back() == '\n')
                    nl += "\n";
                text = text.substr(0, i) + nl + text.substr(end);
                return true;
            }
            return false;  // Version 行格式异常 → 不动（保守）
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return false;
}

bool WriteBytes(const std::wstring& path,
                const std::vector<unsigned char>& data) {
    return WriteWithRetry(path, data);
}

bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& out) {
    return ReadWithRetry(path, out);
}

bool IsNonMsReparseTag(unsigned long tag) {
    // 微软 tag 的 bit31 恒置位（symlink 0xA000000C / junction 0xA0000003 /
    // WOF 0x80000017 / APPEXECLINK 0x8000001B …）；ISV 自定义 tag 不置位。
    return tag != 0 && (tag & 0x80000000UL) == 0;
}

int FindNonMsReparse(const std::wstring& rootIn,
                     std::vector<std::wstring>& out, size_t cap) {
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
        root.pop_back();
    if (root.empty()) return 0;
    std::vector<std::wstring> stack;
    stack.push_back(root);
    int n = 0;
    while (!stack.empty()) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(dir + L"\\" + fd.cFileName);
                continue;
            }
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            if (!IsNonMsReparseTag(fd.dwReserved0)) continue;
            if ((size_t)n >= cap) {
                FindClose(h);
                return n;
            }
            std::wstring p = dir + L"\\" + fd.cFileName;
            out.push_back(p.substr(root.size()));  // 保留 "\..." 前缀
            ++n;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}

// ── 非微软重解析点打包/写回（PIT-136）──────────────────────────────
namespace {
std::string W2A(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? (char)c : '?';
    return s;
}
}  // namespace

std::vector<unsigned char> BuildRepack(const std::vector<ReparseEntry>& in) {
    std::vector<unsigned char> out;
    auto put = [&](const void* p, size_t n) {
        const unsigned char* b = (const unsigned char*)p;
        out.insert(out.end(), b, b + n);
    };
    auto u32 = [&](unsigned long v) { put(&v, 4); };
    put("ZJRP1", 4);
    u32((unsigned long)in.size());
    for (const auto& e : in) {
        u32((unsigned long)(e.relPath.size() * 2));
        put(e.relPath.data(), e.relPath.size() * 2);
        u32(e.attrs);
        put(&e.mtime, 8);
        u32((unsigned long)e.content.size());
        if (!e.content.empty()) put(e.content.data(), e.content.size());
        u32((unsigned long)e.reparse.size());
        if (!e.reparse.empty()) put(e.reparse.data(), e.reparse.size());
    }
    return out;
}

bool ParseRepack(const std::vector<unsigned char>& bytes,
                 std::vector<ReparseEntry>& out) {
    if (bytes.size() < 8 || memcmp(bytes.data(), "ZJRP1", 4) != 0) return false;
    unsigned long n = 0;
    memcpy(&n, bytes.data() + 4, 4);
    size_t o = 8;
    auto need = [&](size_t k) { return o + k <= bytes.size(); };
    for (unsigned long i = 0; i < n; ++i) {
        if (!need(4)) return false;
        unsigned long pl = 0;
        memcpy(&pl, bytes.data() + o, 4);
        o += 4;
        if (pl % 2 || !need((size_t)pl + 4 + 8 + 4)) return false;
        ReparseEntry e;
        e.relPath.assign((const wchar_t*)(bytes.data() + o), pl / 2);
        o += pl;
        memcpy(&e.attrs, bytes.data() + o, 4);
        o += 4;
        memcpy(&e.mtime, bytes.data() + o, 8);
        o += 8;
        unsigned long cl = 0;
        memcpy(&cl, bytes.data() + o, 4);
        o += 4;
        if (!need((size_t)cl + 4)) return false;
        e.content.assign(bytes.data() + o, bytes.data() + o + cl);
        o += cl;
        unsigned long rl = 0;
        memcpy(&rl, bytes.data() + o, 4);
        o += 4;
        if (!need(rl)) return false;
        e.reparse.assign(bytes.data() + o, bytes.data() + o + rl);
        o += rl;
        out.push_back(std::move(e));
    }
    return true;
}

int CaptureReparse(const std::wstring& rootIn, const std::wstring& packPath,
                   std::string& err) {
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
        root.pop_back();
    if (root.empty()) {
        err = "empty root";
        return -1;
    }
    std::vector<ReparseEntry> entries;
    std::vector<std::wstring> stack;
    stack.push_back(root);
    unsigned long long skipped = 0;
    while (!stack.empty()) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(p);
                continue;
            }
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                continue;
            if (!IsNonMsReparseTag(fd.dwReserved0)) continue;
            if (entries.size() >= 64) {
                ++skipped;
                continue;
            }
            HANDLE f = CreateFileW(p.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT |
                                       FILE_FLAG_BACKUP_SEMANTICS,
                                   nullptr);
            if (f == INVALID_HANDLE_VALUE) {
                ++skipped;
                continue;
            }
            ReparseEntry e;
            e.relPath = p.substr(root.size());
            e.attrs = fd.dwFileAttributes;
            FILETIME ft = {};
            if (GetFileTime(f, nullptr, nullptr, &ft))
                e.mtime = ((unsigned long long)ft.dwHighDateTime << 32) |
                          ft.dwLowDateTime;
            {
                std::vector<unsigned char> buf(64 * 1024);
                DWORD ret = 0;
                if (DeviceIoControl(f, FSCTL_GET_REPARSE_POINT, nullptr, 0,
                                    buf.data(), (DWORD)buf.size(), &ret,
                                    nullptr))
                    e.reparse.assign(buf.data(), buf.data() + ret);
            }
            DWORD sz = GetFileSize(f, nullptr);
            if (sz != INVALID_FILE_SIZE && sz > 0 && sz <= (16u << 20)) {
                e.content.resize(sz);
                DWORD rd = 0;
                if (!ReadFile(f, e.content.data(), sz, &rd, nullptr))
                    e.content.clear();
                else
                    e.content.resize(rd);
            }
            CloseHandle(f);
            if (e.reparse.empty()) {  // 取不到 reparse 数据就别装坏的
                ++skipped;
                continue;
            }
            entries.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (skipped)
        err = "skipped " + std::to_string(skipped) + " (cap/io)";
    if (entries.empty()) return 0;
    if (!WriteBytes(packPath, BuildRepack(entries))) {
        err = "write pack failed";
        return -1;
    }
    return (int)entries.size();
}

int ApplyRepack(const std::wstring& packPath, const std::wstring& rootIn,
                unsigned long long* ok, unsigned long long* failed,
                const std::function<void(const std::string&)>& logLine) {
    auto log = [&](const std::string& s) {
        if (logLine) logLine(s);
    };
    std::vector<unsigned char> bytes;
    if (!ReadBytes(packPath, bytes)) {
        log("reppack: read failed");
        return -1;
    }
    std::vector<ReparseEntry> entries;
    if (!ParseRepack(bytes, entries)) {
        log("reppack: parse failed");
        return -1;
    }
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
        root.pop_back();
    unsigned long long okN = 0, badN = 0;
    for (auto& e : entries) {
        std::wstring full = root + e.relPath;
        size_t pos = full.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            std::wstring parent = full.substr(0, pos);
            std::wstring cur;
            for (size_t i = 0; i < parent.size(); ++i) {
                cur += parent[i];
                if (parent[i] == L'\\' && cur.size() > 3)
                    CreateDirectoryW(cur.c_str(), nullptr);
            }
            CreateDirectoryW(parent.c_str(), nullptr);
        }
        bool okOne = false;
        HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr = 0;
            okOne = e.content.empty() ||
                    WriteFile(f, e.content.data(), (DWORD)e.content.size(), &wr,
                              nullptr) != 0;
            CloseHandle(f);
        }
        if (okOne) {
            HANDLE f2 = CreateFileW(full.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
            DWORD ret = 0;
            if (f2 == INVALID_HANDLE_VALUE ||
                !DeviceIoControl(f2, FSCTL_SET_REPARSE_POINT,
                                 (LPVOID)e.reparse.data(),
                                 (DWORD)e.reparse.size(), nullptr, 0, &ret,
                                 nullptr))
                okOne = false;
            if (f2 != INVALID_HANDLE_VALUE) CloseHandle(f2);
        }
        if (okOne) {
            if (e.attrs) SetFileAttributesW(full.c_str(), e.attrs);
            if (e.mtime) {
                FILETIME ft;
                ft.dwLowDateTime = (DWORD)(e.mtime & 0xFFFFFFFF);
                ft.dwHighDateTime = (DWORD)(e.mtime >> 32);
                HANDLE f3 = CreateFileW(full.c_str(), FILE_WRITE_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (f3 != INVALID_HANDLE_VALUE) {
                    SetFileTime(f3, nullptr, nullptr, &ft);
                    CloseHandle(f3);
                }
            }
            ++okN;
            log("reppack: restored " + W2A(e.relPath));
        } else {
            ++badN;
            log("reppack: FAILED " + W2A(e.relPath));
        }
    }
    if (ok) *ok = okN;
    if (failed) *failed = badN;
    return badN ? 1 : 0;
}

int CaptureVolume(const std::wstring& rootIn, const std::wstring& packPath,
                  const std::wstring& auditPath, std::string& err) {
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
        root.pop_back();
    if (root.empty()) {
        err = "empty root";
        return -1;
    }
    std::string audit;
    audit += "# SysRecover scan-ea " SYSRECOVER_VERSION " root=";
    for (wchar_t c : root) audit += (c < 128) ? (char)c : '?';
    audit += "\r\n";
    std::vector<FileEntry> bag;
    unsigned long long files = 0, hit = 0, packed = 0;
    bool capped = false;
    const size_t kPackCap = 128u << 20;
    std::vector<unsigned char> buf(256 * 1024);

    // 递归遍历（显式栈，避免深目录栈溢出）
    std::vector<std::wstring> stack;
    stack.push_back(root);
    std::string relUtf8;  // 复用：relPath 的 ASCII 近似（审计文本用）
    while (!stack.empty() && !capped) {
        std::wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
                continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    stack.push_back(p);
                continue;
            }
            ++files;
            HANDLE f = CreateFileW(p.c_str(), FILE_READ_EA | FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                       FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            struct {
                long status;
                void* info;
            } iosb = {};
            long st = NtQueryEaFile(f, &iosb, buf.data(),
                                    (unsigned long)buf.size(), FALSE, nullptr, 0,
                                    nullptr, FALSE);
            CloseHandle(f);
            if (st != 0 && st != kStatusBufferOverflow) continue;  // 无 EA
            ++hit;
            std::wstring rel = p.substr(root.size() + 1);
            std::string relA;
            for (wchar_t c : rel) relA += (c < 128) ? (char)c : '?';
            if (st == kStatusBufferOverflow) {
                audit += relA + " | (EA too large, not packed)\r\n";
                continue;
            }
            FileEntry fe;
            fe.relPath = rel;
            unsigned long off = 0;
            bool bad = false;
            for (;;) {
                const unsigned char* e = buf.data() + off;
                unsigned long next = *(const unsigned long*)e;
                unsigned char nameLen = e[5];
                unsigned short valLen = *(const unsigned short*)(e + 6);
                if (nameLen == 0 || off + 8u + nameLen + 1u + valLen > buf.size()) {
                    bad = true;
                    break;
                }
                EaValue ev;
                ev.name.assign((const char*)e + 8, nameLen);
                ev.value.assign(e + 8 + nameLen + 1, e + 8 + nameLen + 1 + valLen);
                fe.eas.push_back(std::move(ev));
                if (next == 0) break;
                if (off + next + 8 > buf.size()) {
                    bad = true;
                    break;
                }
                off += next;
            }
            if (bad || fe.eas.empty()) {
                audit += relA + " | (EA parse failed, not packed)\r\n";
                continue;
            }
            // 审计行：名字 + 值前 32 字节十六进制（与 ScanEaFiles 同风格）
            audit += relA + " | ";
            for (size_t k = 0; k < fe.eas.size(); ++k) {
                const auto& v = fe.eas[k];
                if (k) audit += " | ";
                audit += v.name + "=";
                size_t vn = std::min<size_t>(v.value.size(), 32);
                char hx[4];
                for (size_t m = 0; m < vn; ++m) {
                    snprintf(hx, sizeof(hx), "%02X", v.value[m]);
                    audit += hx;
                }
                if (v.value.size() > 32) audit += "..";
            }
            audit += "\r\n";
            // 打包（超上限的文件只审计）
            if (bag.size() < 2000000 && !capped) {
                size_t estimate = 8 + 4 + 4 + rel.size() * 2 + 4;
                for (const auto& v : fe.eas)
                    estimate += 6 + v.name.size() + v.value.size();
                if (estimate > kPackCap) {
                    capped = true;
                    audit += "# pack capped (size limit)\r\n";
                } else {
                    bag.push_back(std::move(fe));
                    ++packed;
                }
            }
        } while (FindNextFileW(h, &fd) && !capped);
        FindClose(h);
    }
    char tail[128];
    snprintf(tail, sizeof(tail), "# files=%llu withEA=%llu packed=%llu%s\r\n",
             files, hit, packed, capped ? " (capped)" : "");
    audit += tail;
    if (!auditPath.empty() && !WriteBytes(auditPath, std::vector<unsigned char>(
                                                   audit.begin(), audit.end()))) {
        err = "audit write failed";
        return -1;
    }
    if (hit > 0) {
        std::vector<unsigned char> packBytes = BuildPack(bag);
        if (!WriteBytes(packPath, packBytes)) {
            err = "pack write failed";
            return -1;
        }
    }
    return (int)hit;
}

std::vector<unsigned char> BuildSetEaBuffer(const std::vector<EaValue>& eas) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i < eas.size(); ++i)
        PutEaEntry(out, eas[i], i + 1 == eas.size());
    return out;
}

int ApplyPack(const std::wstring& packPath, const std::wstring& root,
              ApplyStats& stats,
              const std::function<void(const std::string&)>& logLine) {
    std::vector<unsigned char> raw;
    if (!ReadBytes(packPath, raw)) {
        if (logLine) logLine("apply: cannot read pack: " + std::string("open failed"));
        return 1;
    }
    std::vector<FileEntry> files;
    std::string why;
    if (!ParsePack(raw.data(), raw.size(), files, why)) {
        if (logLine) logLine("apply: bad pack: " + why);
        return 1;
    }
    if (logLine)
        logLine("apply: " + std::to_string(files.size()) + " file(s) with EA");
    std::wstring rootCanon = root;
    while (!rootCanon.empty() &&
           (rootCanon.back() == L'/' || rootCanon.back() == L'\\'))
        rootCanon.pop_back();
    for (const auto& f : files) {
        ++stats.files;
        std::wstring full = rootCanon + L"\\" + f.relPath;
        HANDLE h = CreateFileW(full.c_str(), FILE_WRITE_EA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                ++stats.missing;
            } else {
                ++stats.failed;
                stats.lastStatus = e;
                if (stats.firstErrors.size() < 2048) {
                    std::string na;
                    for (wchar_t c : f.relPath) na += (c < 128) ? (char)c : '?';
                    stats.firstErrors +=
                        "open err=" + std::to_string(e) + " " + na + "\r\n";
                }
            }
            continue;
        }
        std::vector<unsigned char> eaBuf = BuildSetEaBuffer(f.eas);
        struct {
            long status;
            void* info;
        } iosb = {};
        long st = NtSetEaFile(h, &iosb, eaBuf.data(), (unsigned long)eaBuf.size());
        CloseHandle(h);
        if (st == 0) {
            ++stats.ok;
        } else {
            ++stats.failed;
            stats.lastStatus = (unsigned long)st;
            if (stats.firstErrors.size() < 2048) {
                std::string na;
                for (wchar_t c : f.relPath) na += (c < 128) ? (char)c : '?';
                char hx[32];
                snprintf(hx, sizeof(hx), "%08lX", (unsigned long)st);
                stats.firstErrors +=
                    std::string("set 0x") + hx + " " + na + "\r\n";
            }
        }
    }
    return stats.failed > 0 ? 1 : 0;
}

void CleanupHookFiles(const std::wstring& rootIn,
                      const std::function<void(const std::string&)>& logLine) {
    auto log = [&](const std::string& s) {
        if (logLine) logLine(s);
    };
    std::wstring root = rootIn;
    while (!root.empty() && (root.back() == L'/' || root.back() == L'\\'))
        root.pop_back();
    const std::wstring gp = root + L"\\Windows\\System32\\GroupPolicy";
    const std::wstring cmd =
        gp + L"\\Machine\\Scripts\\Startup\\zj-ea-restore.cmd";
    const std::wstring ini = gp + L"\\Machine\\Scripts\\scripts.ini";
    const std::wstring gpt = gp + L"\\gpt.ini";
    const std::wstring pol = gp + L"\\Machine\\Registry.pol";
    const std::wstring pack = root + L"\\ZJRESTORE\\ea\\eapack.dat";

    // 1) scripts.ini 去掉我们的条目
    std::vector<unsigned char> iniRaw;
    if (ReadBytes(ini, iniRaw)) {
        std::string text(iniRaw.begin(), iniRaw.end());
        if (RemoveScriptEntry(text, "zj-ea-restore.cmd")) {
            std::vector<unsigned char> out(text.begin(), text.end());
            log(WriteBytes(ini, out) ? "cleanup: scripts.ini entry removed"
                                     : "cleanup: scripts.ini write FAILED");
        } else {
            log("cleanup: scripts.ini has no our entry");
        }
    } else {
        log("cleanup: read FAILED scripts.ini");
    }
    // 2) gpt.ini 版本 +1（下次引导 gpsvc 重新处理 → 卸掉同步策略）
    std::vector<unsigned char> gptRaw;
    if (ReadBytes(gpt, gptRaw)) {
        std::string text(gptRaw.begin(), gptRaw.end());
        if (BumpGptIniVersion(text)) {
            std::vector<unsigned char> out(text.begin(), text.end());
            log(WriteBytes(gpt, out) ? "cleanup: gpt.ini version bumped"
                                     : "cleanup: gpt.ini write FAILED");
        } else {
            log("cleanup: gpt.ini version not found (skip)");
        }
    } else {
        log("cleanup: read FAILED gpt.ini");
    }
    // 3) Registry.pol 剥掉我们的两条记录
    std::vector<unsigned char> polRaw;
    if (ReadBytes(pol, polRaw)) {
        bool removed = false;
        std::vector<unsigned char> out = StripOurPolRecords(polRaw, &removed);
        if (removed) {
            bool onlyHeader = out.size() <= 8;
            if (onlyHeader) {
                log(DeleteFileW(pol.c_str())
                        ? "cleanup: Registry.pol deleted (ours only)"
                        : "cleanup: Registry.pol delete FAILED");
            } else {
                log(WriteBytes(pol, out) ? "cleanup: Registry.pol records stripped"
                                         : "cleanup: Registry.pol write FAILED");
            }
        } else {
            log("cleanup: Registry.pol has no our records");
        }
    } else {
        log("cleanup: read FAILED Registry.pol");
    }
    // 4) 删除 pack（立即）与 cmd/自身（延迟到重启前，规避正在执行的文件句柄）
    log(DeleteFileW(pack.c_str()) ? "cleanup: eapack.dat deleted"
                                  : "cleanup: eapack.dat delete FAILED");
    // PIT-136：reppack.dat 同清（与 eapack 一致；失败保留重试）
    {
        const std::wstring rp = root + L"\\ZJRESTORE\\ea\\reppack.dat";
        if (GetFileAttributesW(rp.c_str()) != INVALID_FILE_ATTRIBUTES)
            log(DeleteFileW(rp.c_str()) ? "cleanup: reppack.dat deleted"
                                        : "cleanup: reppack.dat delete FAILED");
    }
    if (!MoveFileExW(cmd.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        log("cleanup: cmd delayed-delete FAILED err=" +
            std::to_string(GetLastError()));
    }
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) &&
        !MoveFileExW(self, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        log("cleanup: self delayed-delete FAILED err=" +
            std::to_string(GetLastError()));
    }
}

}  // namespace ea
}  // namespace sysrecover
