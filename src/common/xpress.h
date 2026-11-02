#pragma once
// XPRESS-Huffman 压缩/解压（PIT-213）：直接调 ntdll 的 RtlCompressBuffer /
// RtlDecompressBuffer，**零第三方依赖**（x86/x64 的 ntdll 都有，Win7+ 可用）。
//
// 用途：单文件容器的载荷压缩 —— 未压缩 ~50MB，压缩后约 38MB。
// 注意：这几个 Rtl* 在 ntifs.h 里声明，但 ntdll 一直导出；动态取地址比链接
// import lib 稳（不同 mingw 版本的导入库不一定带）。
#include <windows.h>

#include <cstddef>
#include <vector>

namespace sysrecover {
namespace xpress {

// COMPRESSION_FORMAT_XPRESS_HUFF（压缩比最好的一档；LZNT1 差很多）
const USHORT kFormat = 4;

typedef LONG(WINAPI* FnGetWorkSpace)(USHORT, PULONG, PULONG);
typedef LONG(WINAPI* FnCompress)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG,
                                 PULONG, PVOID);
typedef LONG(WINAPI* FnDecompress)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG,
                                   PULONG);
typedef LONG(WINAPI* FnDecompressEx)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG,
                                     PULONG, PVOID);

inline HMODULE Ntdll() {
    static HMODULE h = GetModuleHandleW(L"ntdll.dll");
    return h;
}

inline bool Available() {
    return Ntdll() && GetProcAddress(Ntdll(), "RtlCompressBuffer") &&
           GetProcAddress(Ntdll(), "RtlDecompressBuffer");
}

// 压缩；结果不比原文小 → 返回 false（调用方存原样）。
inline bool Compress(const std::vector<unsigned char>& src,
                     std::vector<unsigned char>& dst) {
    dst.clear();
    if (src.empty()) return false;
    HMODULE h = Ntdll();
    auto fnWs = (FnGetWorkSpace)GetProcAddress(h,
                                               "RtlGetCompressionWorkSpaceSize");
    auto fnCo = (FnCompress)GetProcAddress(h, "RtlCompressBuffer");
    if (!fnWs || !fnCo) return false;
    ULONG wsC = 0, wsD = 0;
    if (fnWs(kFormat, &wsC, &wsD) != 0) return false;
    // 最坏情况略大于输入；多给 1/8 + 4KB 足够
    dst.resize(src.size() + src.size() / 8 + 4096);
    std::vector<unsigned char> ws(wsC ? wsC : 1);
    ULONG final = 0;
    const LONG st = fnCo(kFormat, (PUCHAR)src.data(), (ULONG)src.size(),
                         dst.data(), (ULONG)dst.size(), 4096, &final, ws.data());
    if (st != 0 || final == 0 || final >= src.size()) {
        dst.clear();
        return false;
    }
    dst.resize(final);
    return true;
}

// 解压到已知原始长度 rawLen。
// ⚠️ XPRESS_HUFF **不能用 RtlDecompressBuffer**（它对 HUFF 返回
// STATUS_INVALID_PARAMETER）——必须走 RtlDecompressBufferEx + workspace。
inline bool Decompress(const unsigned char* src, size_t srcLen, size_t rawLen,
                       std::vector<unsigned char>& dst, LONG* statusOut = nullptr) {
    dst.clear();
    if (rawLen == 0) return false;
    HMODULE h = Ntdll();
    auto fnWs = (FnGetWorkSpace)GetProcAddress(h,
                                               "RtlGetCompressionWorkSpaceSize");
    auto fnEx = (FnDecompressEx)GetProcAddress(h, "RtlDecompressBufferEx");
    auto fnDe = (FnDecompress)GetProcAddress(h, "RtlDecompressBuffer");
    ULONG wsC = 0, wsD = 0;
    if (fnWs) fnWs(kFormat, &wsC, &wsD);
    dst.resize(rawLen);
    ULONG final = 0;
    LONG st = -1;
    if (fnEx) {
        std::vector<unsigned char> ws(wsD ? wsD : 1);
        st = fnEx(kFormat, dst.data(), (ULONG)rawLen, (PUCHAR)src, (ULONG)srcLen,
                  &final, ws.data());
    }
    if ((st != 0 || final != rawLen) && fnDe) {
        final = 0;
        st = fnDe(kFormat, dst.data(), (ULONG)rawLen, (PUCHAR)src, (ULONG)srcLen,
                  &final);
    }
    if (statusOut) *statusOut = st;
    if (st != 0 || final != rawLen) {
        dst.clear();
        return false;
    }
    return true;
}

}  // namespace xpress
}  // namespace sysrecover
