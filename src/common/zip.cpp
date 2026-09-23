// 极简 ZIP 写入器（store 模式）实现，见 zip.h 顶部说明。
#include "zip.h"

#include <windows.h>

#include <cstring>

namespace sysrecover {
namespace {

uint32_t Crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// MS-DOS 时间/日期（ZIP 用的老格式）
void DosTimeDate(uint16_t* t, uint16_t* d) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wYear < 1980)
        st.wYear = 1980;
    *t = static_cast<uint16_t>((st.wHour << 11) | (st.wMinute << 5) |
                               (st.wSecond / 2));
    *d = static_cast<uint16_t>(((st.wYear - 1980) << 9) | (st.wMonth << 5) |
                               st.wDay);
}

void Put16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void Put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

ZipWriter::ZipWriter(const std::wstring& zipPath) {
    HANDLE h = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ok_ = false;
        return;
    }
    fp_ = h;
}

ZipWriter::~ZipWriter() {
    if (fp_) {
        Close();
        CloseHandle(static_cast<HANDLE>(fp_));
        fp_ = nullptr;
    }
}

bool ZipWriter::AddFile(const std::wstring& srcPath,
                        const std::string& zipName) {
    if (!ok_ || closed_)
        return false;
    HANDLE h = CreateFileW(srcPath.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;  // 单个文件读不到 → 跳过，不让整包失败
    LARGE_INTEGER sz = {};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart < 0 || sz.QuadPart > 0xFFFFFFFFll) {
        CloseHandle(h);
        return false;
    }
    std::string buf(static_cast<size_t>(sz.QuadPart), 0);
    DWORD got = 0;
    BOOL r = buf.empty()
                 ? TRUE
                 : ReadFile(h, &buf[0], static_cast<DWORD>(buf.size()), &got,
                            nullptr);
    CloseHandle(h);
    if (!r)
        return false;
    return AddEntry(zipName, buf.data(), buf.size());
}

bool ZipWriter::AddData(const void* data, size_t size,
                        const std::string& zipName) {
    if (!ok_ || closed_)
        return false;
    return AddEntry(zipName, data, size);
}

bool ZipWriter::AddEntry(const std::string& zipName, const void* data,
                         size_t size) {
    Entry e;
    e.name = zipName;
    e.crc = Crc32(data, size);
    e.size = static_cast<uint32_t>(size);
    DosTimeDate(&e.dosTime, &e.dosDate);

    LARGE_INTEGER off = {}, zero = {};
    SetFilePointerEx(static_cast<HANDLE>(fp_), zero, &off, FILE_CURRENT);
    e.offset = static_cast<uint32_t>(off.QuadPart);

    std::string hdr;
    Put32(hdr, 0x04034b50);  // local file header
    Put16(hdr, 20);          // version needed
    Put16(hdr, 0x0800);      // flags: 文件名是 UTF-8
    Put16(hdr, 0);           // method = 0 (store)
    Put16(hdr, e.dosTime);
    Put16(hdr, e.dosDate);
    Put32(hdr, e.crc);
    Put32(hdr, e.size);
    Put32(hdr, e.size);
    Put16(hdr, static_cast<uint16_t>(e.name.size()));
    Put16(hdr, 0);  // extra len
    hdr += e.name;

    DWORD wrote = 0;
    if (!WriteFile(static_cast<HANDLE>(fp_), hdr.data(),
                   static_cast<DWORD>(hdr.size()), &wrote, nullptr) ||
        (size && !WriteFile(static_cast<HANDLE>(fp_), data,
                            static_cast<DWORD>(size), &wrote, nullptr))) {
        ok_ = false;
        return false;
    }
    entries_.push_back(e);
    return true;
}

bool ZipWriter::Close() {
    if (closed_)
        return ok_;
    closed_ = true;
    if (!ok_ || !fp_)
        return false;

    LARGE_INTEGER off = {}, zero = {};
    SetFilePointerEx(static_cast<HANDLE>(fp_), zero, &off, FILE_CURRENT);
    const uint32_t cdOffset = static_cast<uint32_t>(off.QuadPart);

    std::string cd;
    for (const auto& e : entries_) {
        Put32(cd, 0x02014b50);  // central directory header
        Put16(cd, 20);          // version made by
        Put16(cd, 20);          // version needed
        Put16(cd, 0x0800);      // flags
        Put16(cd, 0);           // method
        Put16(cd, e.dosTime);
        Put16(cd, e.dosDate);
        Put32(cd, e.crc);
        Put32(cd, e.size);
        Put32(cd, e.size);
        Put16(cd, static_cast<uint16_t>(e.name.size()));
        Put16(cd, 0);  // extra len
        Put16(cd, 0);  // comment len
        Put16(cd, 0);  // disk number start
        Put16(cd, 0);  // internal attrs
        Put32(cd, 0);  // external attrs
        Put32(cd, e.offset);
        cd += e.name;
    }
    DWORD wrote = 0;
    if (!cd.empty() &&
        !WriteFile(static_cast<HANDLE>(fp_), cd.data(),
                   static_cast<DWORD>(cd.size()), &wrote, nullptr))
        return false;

    std::string eocd;
    Put32(eocd, 0x06054b50);  // end of central directory
    Put16(eocd, 0);
    Put16(eocd, 0);
    Put16(eocd, static_cast<uint16_t>(entries_.size()));
    Put16(eocd, static_cast<uint16_t>(entries_.size()));
    Put32(eocd, static_cast<uint32_t>(cd.size()));
    Put32(eocd, cdOffset);
    Put16(eocd, 0);  // comment len
    if (!WriteFile(static_cast<HANDLE>(fp_), eocd.data(),
                   static_cast<DWORD>(eocd.size()), &wrote, nullptr))
        return false;
    FlushFileBuffers(static_cast<HANDLE>(fp_));
    return true;
}

}  // namespace sysrecover
