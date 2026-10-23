// reparse-lab.cpp — 1019 案 F6 实验室工具（2026-10-19）
// 宿主（Windows）验证重解析点链路；**不入产品、不参与构建**（用时手工编译）：
//   D:\Prog\ProgIDE\mingw64\bin\g++.exe -O2 -std=c++17 -D_WIN32_WINNT=0x0601 \
//       -DUNICODE -D_UNICODE tools/reparse-lab.cpp src/common/ea.cpp \
//       -o build/reparse-lab.exe -static -mconsole -municode -lntdll -ladvapi32
//
// 用法（管理员；先建一个空目录当实验室——roundtrip 会扫整个目录，里面只放目标文件）：
//   reparse-lab make <file> [tagHex]   造非微软 GUID 重解析点文件
//                                      （默认 0xDEAD；0/1 是系统保留值，会 err=4393）
//   reparse-lab check <path>           谓词判定（无 blob 子集 + 归一化 blob 全量）
//   reparse-lab roundtrip <file>       CaptureReparse → 删文件 → ApplyRepack →
//                                      重新采集逐字节比对（repack 回放首次真机验证）
//   reparse-lab capture <srcDir> <dstWim>  用 libwim **无排除规则**捕获目录（模拟
//                                      "老软件/第三方工具"做出的镜像 —— 含 Linux 救援层
//                                      写不回的 reparse 点；供救援层预检演练
//                                      ZJ_DRILL_REPARSE=reject）。编译时需加：
//                                      -I third_party/wimlib -L third_party/wimlib -l:libwim-15.dll
//   reparse-lab dir <wim> [index]      列出子镜像元数据（attrs/reparse tag/流大小）——
//                                      W2 用：验证 EFS 文件带 ENCRYPTED 属性 + 加密数据流
//                                      （streams[0] = 未命名流，加密文件时=EFSRPC 原始数据）。
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/common/ea.h"
#include "../third_party/wimlib/wimlib.h"

namespace ea = sysrecover::ea;

// REPARSE_GUID_DATA_BUFFER 形态（非微软 tag 必须带 GUID 送给
// FSCTL_SET_REPARSE_POINT；字段与微软结构一致，自带 payload 缓冲方便直接送）。
#pragma pack(push, 1)
struct ZjGuidReparse {
    unsigned long tag;
    unsigned short datalen;  // = payload 字节数（不含 8 字节头与 16 字节 GUID）
    unsigned short reserved;
    GUID guid;
    unsigned char payload[64];
};
#pragma pack(pop)

static unsigned long Le32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static std::wstring ParentOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos);
}

static bool OpenRp(const wchar_t* path, unsigned long access,
                   unsigned long share, HANDLE* out) {
    *out = CreateFileW(path, access, share, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                       nullptr);
    return *out != INVALID_HANDLE_VALUE;
}

static bool ReadBlobRaw(HANDLE h, std::vector<unsigned char>& out) {
    out.resize(65536);
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, out.data(),
                         (DWORD)out.size(), &ret, nullptr)) {
        out.clear();
        return false;
    }
    out.resize(ret);
    return true;
}

static bool ReadContent(HANDLE h, std::vector<unsigned char>& out) {
    DWORD sz = GetFileSize(h, nullptr);
    if (sz == INVALID_FILE_SIZE || sz == 0) {
        out.clear();
        return true;
    }
    out.resize(sz);
    DWORD rd = 0;
    if (!ReadFile(h, out.data(), sz, &rd, nullptr)) {
        out.clear();
        return false;
    }
    out.resize(rd);
    return true;
}

static void Usage() {
    printf("usage:\n"
           "  reparse-lab make <file> [tagHex]   create non-MS GUID reparse file"
           " (default tag 0xDEAD)\n"
           "  reparse-lab check <path>           predicate verdicts\n"
           "  reparse-lab roundtrip <file>       CaptureReparse->ApplyRepack verify\n"
           "  reparse-lab capture <src> <wim>    capture dir to WIM with NO"
           " exclusion rules (old-software image)\n"
           "  reparse-lab dir <wim> [index]      list subimage metadata"
           " (attrs/reparse/streams)\n");
}

// 造非微软重解析点文件：先写内容，再以 GUID 形态 SET（管理员）。
static int ModeMake(const wchar_t* pathArg, unsigned long tag) {
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(pathArg, MAX_PATH, full, nullptr)) {
        printf("bad path\n");
        return 2;
    }
    HANDLE h = CreateFileW(full, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"create failed: %ls err=%lu\n", full, GetLastError());
        return 2;
    }
    const char content[] = "ZJREPARSE-LAB-CONTENT-0123456789ABCDEF\n";
    DWORD wr = 0;
    WriteFile(h, content, sizeof(content) - 1, &wr, nullptr);
    CloseHandle(h);

    ZjGuidReparse zb = {};
    zb.tag = tag;
    memcpy(zb.payload, "ZJ-LAB-REPARSE-DATA", 19);
    zb.datalen = 19;
    zb.guid = GUID{0x11223344, 0x5566, 0x7788,
                   {0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22}};
    const DWORD total = 8 + 16 + zb.datalen;
    HANDLE f = INVALID_HANDLE_VALUE;
    if (!OpenRp(full, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &f)) {
        wprintf(L"open-for-set failed: %ls err=%lu\n", full, GetLastError());
        return 2;
    }
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(f, FSCTL_SET_REPARSE_POINT, &zb, total, nullptr, 0,
                              &ret, nullptr);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(f);
    if (!ok) {
        wprintf(L"FSCTL_SET_REPARSE_POINT failed: err=%lu"
                L" (need admin? tag is non-Microsoft, GUID form required)\n",
                err);
        return 2;
    }
    wprintf(L"made: %ls tag=0x%08lX datalen=%u\n", full, tag,
            (unsigned)zb.datalen);
    return 0;
}

static int ModeCheck(const wchar_t* pathArg) {
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(pathArg, MAX_PATH, full, nullptr)) {
        printf("bad path\n");
        return 2;
    }
    unsigned long attrs = GetFileAttributesW(full);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"not found: %ls err=%lu\n", full, GetLastError());
        return 2;
    }
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!OpenRp(full, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &h)) {
        wprintf(L"open failed: %ls err=%lu\n", full, GetLastError());
        return 2;
    }
    std::vector<unsigned char> blob;
    bool got = ReadBlobRaw(h, blob);
    CloseHandle(h);
    if (!got || blob.size() < 8) {
        wprintf(L"path=%ls\nattrs=0x%08lX\nnot a reparse point\n", full, attrs);
        return 2;
    }
    const unsigned long tag = Le32(blob.data());
    const unsigned short dl = (unsigned short)(blob[4] | (blob[5] << 8));
    wprintf(L"path=%ls\n", full);
    wprintf(L"attrs=0x%08lX tag=0x%08lX datalen=%u len=%zu\n", attrs, tag,
            (unsigned)dl, blob.size());
    printf("non-ms=%s\n", ea::IsNonMsReparseTag(tag) ? "yes" : "no");
    printf("verdict(no-blob)=%s\n",
           ea::IsUnsettableReparse(tag, attrs, nullptr, 0) ? "UNSETTABLE"
                                                           : "settable?");
    std::vector<unsigned char> norm = blob;
    bool nok = ea::NormalizeReparseBlob(norm.data(), norm.size());
    printf("normalize=%s\n", nok ? "ok" : "fail");
    printf("verdict(blob,norm)=%s\n",
           ea::IsUnsettableReparse(tag, attrs, nok ? norm.data() : blob.data(),
                                   nok ? norm.size() : blob.size())
               ? "UNSETTABLE"
               : "settable?");
    printf("verdict(blob,raw)=%s\n",
           ea::IsUnsettableReparse(tag, attrs, blob.data(), blob.size())
               ? "UNSETTABLE"
               : "settable?");
    return 0;
}

static int ModeRoundtrip(const wchar_t* pathArg) {
    wchar_t full[MAX_PATH] = {};
    if (!GetFullPathNameW(pathArg, MAX_PATH, full, nullptr)) {
        printf("bad path\n");
        return 2;
    }
    unsigned long attrs0 = GetFileAttributesW(full);
    if (attrs0 == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"not found: %ls err=%lu\n", full, GetLastError());
        return 2;
    }
    std::wstring root = ParentOf(full);
    std::wstring pack = root + L"\\reparse-lab-pack.bin";

    // 1) 原始快照：reparse 原始缓冲 + 未命名内容
    std::vector<unsigned char> blob0, cont0;
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        if (!OpenRp(full, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &h)) {
            wprintf(L"open failed: %ls err=%lu\n", full, GetLastError());
            return 2;
        }
        bool g1 = ReadBlobRaw(h, blob0);
        bool g2 = ReadContent(h, cont0);
        CloseHandle(h);
        if (!g1) {
            wprintf(L"target is not a reparse point: %ls\n", full);
            return 2;
        }
        if (!g2) printf("warn: content read failed (continuing)\n");
    }

    // 2) CaptureReparse（扫 root 下所有"Linux 侧必被拒"的重解析点）
    std::string err;
    int n = ea::CaptureReparse(root, pack, err);
    printf("capture: entries=%d err=\"%s\"\n", n, err.c_str());
    if (n <= 0) {
        printf("FAIL: capture found nothing (pack=%ls)\n", pack.c_str());
        return 1;
    }

    // 3) 删掉再让 ApplyRepack 重建（模拟目标机首启补写）
    SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
    bool deleted = DeleteFileW(full) != 0;
    if (!deleted) {
        HANDLE d = CreateFileW(
            full, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (d != INVALID_HANDLE_VALUE) {
            deleted = true;
            CloseHandle(d);
        }
    }
    if (!deleted) {
        wprintf(L"FAIL: delete err=%lu (pack kept: %ls)\n", GetLastError(),
                pack.c_str());
        return 1;
    }

    // 4) ApplyRepack
    unsigned long long okN = 0, badN = 0;
    int rc = ea::ApplyRepack(pack, root, &okN, &badN,
                             [](const std::string& s) {
                                 printf("  %s\n", s.c_str());
                             });
    printf("apply: rc=%d ok=%llu failed=%llu\n", rc, okN, badN);
    if (rc != 0 || badN) {
        printf("FAIL: apply (pack kept: %ls)\n", pack.c_str());
        return 1;
    }

    // 5) 复核：blob / content 逐字节比对
    std::vector<unsigned char> blob1, cont1;
    {
        HANDLE h = INVALID_HANDLE_VALUE;
        if (!OpenRp(full, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    &h)) {
            wprintf(L"FAIL: file missing after apply: %ls err=%lu\n", full,
                    GetLastError());
            return 1;
        }
        bool g1 = ReadBlobRaw(h, blob1);
        ReadContent(h, cont1);
        CloseHandle(h);
        if (!g1) {
            printf("FAIL: recreated file has no reparse point (pack kept: %ls)\n",
                   pack.c_str());
            return 1;
        }
    }
    bool blobSame = blob1 == blob0;
    bool contSame = cont1 == cont0;
    printf("blob:    %zu -> %zu bytes, %s\n", blob0.size(), blob1.size(),
           blobSame ? "identical" : "DIFF");
    printf("content: %zu -> %zu bytes, %s\n", cont0.size(), cont1.size(),
           contSame ? "identical" : "DIFF");
    if (!blobSame) {
        size_t i = 0;
        while (i < blob0.size() && i < blob1.size() && blob0[i] == blob1[i]) ++i;
        if (i < blob0.size() && i < blob1.size())
            printf("  first diff at %zu: %02X vs %02X\n", i, blob0[i], blob1[i]);
    }
    if (!blobSame || !contSame) {
        printf("FAIL (pack kept: %ls)\n", pack.c_str());
        return 1;
    }
    DeleteFileW(pack.c_str());
    printf("PASS: capture->apply roundtrip byte-identical\n");
    return 0;
}

// capture：libwim 无排除规则捕获目录 → WIM（config=NULL，不经我们 exe 的
// FindNonMsReparse 自动排除，见 PIT-135 —— 因此产物**含**非微软重解析点，
// 等价于 1019 案机器用旧版软件做出的镜像，供救援层预检的拒绝路径演练）。
static int ModeCapture(const wchar_t* srcArg, const wchar_t* dstArg) {
    wchar_t src[MAX_PATH] = {}, dst[MAX_PATH] = {};
    if (!GetFullPathNameW(srcArg, MAX_PATH, src, nullptr) ||
        !GetFullPathNameW(dstArg, MAX_PATH, dst, nullptr)) {
        printf("bad path\n");
        return 2;
    }
    wimlib_global_init(0);
    WIMStruct* w = nullptr;
    int rc = wimlib_create_new_wim(WIMLIB_COMPRESSION_TYPE_XPRESS, &w);
    if (rc != 0) {
        printf("create_new_wim rc=%d\n", rc);
        return 2;
    }
    rc = wimlib_add_image(w, src, L"1", nullptr, 0);
    if (rc != 0) {
        printf("add_image rc=%d (%ls)\n", rc, wimlib_get_error_string(
                   (enum wimlib_error_code)rc));
        wimlib_free(w);
        return 2;
    }
    rc = wimlib_write(w, dst, WIMLIB_ALL_IMAGES, 0, 0);
    wimlib_free(w);
    wimlib_global_cleanup();
    if (rc != 0) {
        printf("write rc=%d\n", rc);
        return 2;
    }
    wprintf(L"captured: %ls -> %ls\n", src, dst);
    return 0;
}

// dir：列出子镜像全部目录项元数据（W2 EFS 实验用）。
// 关注：attributes 里的 ENCRYPTED(0x4000)、reparse_tag、未命名流大小
// （加密文件时 streams[0].resource = EFSRPC 原始加密数据，proves blob 在镜像里）。
static int DirCb(const struct wimlib_dir_entry* e, void* ctx) {
    int* count = (int*)ctx;
    ++*count;
    unsigned enc = e->attributes & WIMLIB_FILE_ATTRIBUTE_ENCRYPTED;
    wprintf(L"attrs=0x%08X%s%s%s  streams=%u  %ls",
            e->attributes,
            enc ? L" ENCRYPTED" : L"",
            (e->attributes & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT) ? L" REPARSE"
                                                                  : L"",
            (e->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) ? L" DIR" : L"",
            e->num_named_streams, e->full_path ? e->full_path : L"(root)");
    if (e->attributes & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT)
        wprintf(L" tag=0x%08X", e->reparse_tag);
    wprintf(L"\n");
    /* streams[0] = 未命名流（普通数据 / reparse 数据 / 加密数据 之一）；
     * 其后 num_named_streams 个为命名流。 */
    for (unsigned i = 0; i <= e->num_named_streams; ++i) {
        const struct wimlib_stream_entry* s = &e->streams[i];
        wprintf(L"    stream[%u] name=%ls size=%llu\n", i,
                s->stream_name ? s->stream_name : L"(unnamed)",
                (unsigned long long)s->resource.uncompressed_size);
    }
    return 0;
}

static int ModeDir(const wchar_t* wimArg, int index) {
    wchar_t path[MAX_PATH] = {};
    if (!GetFullPathNameW(wimArg, MAX_PATH, path, nullptr)) {
        printf("bad path\n");
        return 2;
    }
    wimlib_global_init(0);
    WIMStruct* w = nullptr;
    int rc = wimlib_open_wim(path, 0, &w);
    if (rc != 0) {
        printf("open rc=%d\n", rc);
        wimlib_global_cleanup();
        return 2;
    }
    int count = 0;
    rc = wimlib_iterate_dir_tree(w, index, L"\\",
                                 WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE, DirCb,
                                 &count);
    wimlib_free(w);
    wimlib_global_cleanup();
    printf("iterate rc=%d entries=%d\n", rc, count);
    return rc == 0 ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    std::wstring cmd = argv[1];
    if (cmd == L"make") {
        unsigned long tag = 0xDEADUL;
        if (argc >= 4) tag = wcstoul(argv[3], nullptr, 0);
        return ModeMake(argv[2], tag);
    }
    if (cmd == L"check") return ModeCheck(argv[2]);
    if (cmd == L"roundtrip") return ModeRoundtrip(argv[2]);
    if (cmd == L"capture") {
        if (argc < 4) {
            Usage();
            return 2;
        }
        return ModeCapture(argv[2], argv[3]);
    }
    if (cmd == L"dir") {
        int index = (argc >= 4) ? _wtoi(argv[3]) : 1;
        return ModeDir(argv[2], index);
    }
    Usage();
    return 2;
}
