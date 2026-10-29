// 镜像加密容器实现（格式见 crypt.h）。
//
// v2 = 连续单流 CBC-ESSIV（Linux 可用 loop + dm-crypt 映射，零额外空间）；
// v1 = 分块 CTR + 每块 HMAC（只保留读取能力，已生成的 .zje 还能打开）。
//
// 只依赖 Windows CNG（bcrypt.dll，Vista/Win7 起可用）+ kernel32。
#include "common/crypt.h"

#include "common/i18n.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace sysrecover {
namespace {

constexpr uint8_t kMagic1[8] = {'Z', 'J', 'E', 'N', 'C', '1', 0, 0};
constexpr uint8_t kMagic2[8] = {'Z', 'J', 'E', 'N', 'C', '2', 0, 0};
constexpr uint32_t kVersion1 = 1;
constexpr uint32_t kVersion2 = 2;
constexpr uint32_t kKdfPbkdf2Sha256 = 1;
constexpr uint32_t kIterations = 600000;
constexpr uint32_t kChunkSizeV1 = 1024u * 1024u;
constexpr uint32_t kSectorSizeV2 = 512u;   // dm-crypt 默认扇区（大扇区被内核拒）
constexpr uint32_t kHeaderSizeV2 = 4096u;  // 头固定 4KB，与扇区大小解耦
constexpr uint64_t kBodyOffsetV2 = 0x100000ull;  // 1MB：放 header + meta
constexpr uint64_t kMetaChunk = 0xFFFFFFFFFFFFFFFFull;
constexpr size_t kMacLen = 32;
constexpr size_t kSaltLen = 16;

#pragma pack(push, 1)
struct HeaderV1 {
    uint8_t magic[8];
    uint32_t version;
    uint32_t kdf;
    uint32_t iterations;
    uint32_t chunkSize;
    uint64_t plainSize;
    uint8_t salt[kSaltLen];
    uint8_t headerMac[kMacLen];
};
struct HeaderV2 {
    uint8_t magic[8];
    uint32_t version;
    uint32_t kdf;
    uint32_t iterations;
    uint32_t sectorSize;
    uint64_t plainSize;
    uint8_t salt[kSaltLen];
    uint8_t headerMac[kMacLen];
    uint8_t reserved[kHeaderSizeV2 - 80];
};
#pragma pack(pop)
static_assert(sizeof(HeaderV1) == 80, "ZJENC1 header must be 80 bytes");
static_assert(sizeof(HeaderV2) == kHeaderSizeV2, "ZJENC2 header must be 4096");

#ifndef BCRYPT_SUCCESS
#define BCRYPT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

std::string W2U8(const std::wstring& w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr,
                                0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n,
                        nullptr, nullptr);
    return s;
}

std::wstring U82W(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool ConstantEq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i)
        d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~Alg() {
        if (h)
            BCryptCloseAlgorithmProvider(h, 0);
    }
};

struct Hash {
    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<uint8_t> obj;
    ~Hash() {
        if (h)
            BCryptDestroyHash(h);
    }
};

bool OpenHmac(Alg& a, std::wstring& err) {
    NTSTATUS st = BCryptOpenAlgorithmProvider(
        &a.h, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"无法初始化 SHA256（bcrypt 错误 ") +
              std::to_wstring((long long)st) + Tr(L"）");
        return false;
    }
    return true;
}

bool HmacSha256(BCRYPT_ALG_HANDLE hAlg, const uint8_t key[32],
                const uint8_t* a, size_t alen, const uint8_t* b, size_t blen,
                uint8_t out[kMacLen], std::wstring& err) {
    DWORD objLen = 0, cb = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                                         (PUCHAR)&objLen, sizeof(objLen), &cb,
                                         0)) ||
        objLen == 0) {
        err = Tr(L"读取 SHA256 参数失败");
        return false;
    }
    Hash hs;
    hs.obj.assign(objLen, 0);
    NTSTATUS st = BCryptCreateHash(hAlg, &hs.h, hs.obj.data(), objLen,
                                   (PUCHAR)key, 32, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"HMAC 初始化失败");
        return false;
    }
    if (alen && a)
        st = BCryptHashData(hs.h, (PUCHAR)a, (ULONG)alen, 0);
    if (BCRYPT_SUCCESS(st) && blen && b)
        st = BCryptHashData(hs.h, (PUCHAR)b, (ULONG)blen, 0);
    if (BCRYPT_SUCCESS(st))
        st = BCryptFinishHash(hs.h, out, kMacLen, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"HMAC 计算失败");
        return false;
    }
    return true;
}

// 单次 SHA256（ESSIV 的 IV 密钥 = SHA256(encKey)）
bool Sha256Once(const uint8_t* data, size_t len, uint8_t out[32],
                std::wstring& err) {
    Alg a;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&a.h, BCRYPT_SHA256_ALGORITHM,
                                                    nullptr, 0))) {
        err = Tr(L"无法初始化 SHA256");
        return false;
    }
    DWORD objLen = 0, cb = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(a.h, BCRYPT_OBJECT_LENGTH,
                                          (PUCHAR)&objLen, sizeof(objLen), &cb,
                                          0)) ||
        objLen == 0) {
        err = Tr(L"读取 SHA256 参数失败");
        return false;
    }
    Hash hs;
    hs.obj.assign(objLen, 0);
    NTSTATUS st = BCryptCreateHash(a.h, &hs.h, hs.obj.data(), objLen, nullptr, 0,
                                   0);
    if (BCRYPT_SUCCESS(st))
        st = BCryptHashData(hs.h, (PUCHAR)data, (ULONG)len, 0);
    if (BCRYPT_SUCCESS(st))
        st = BCryptFinishHash(hs.h, out, 32, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"HMAC 计算失败");
        return false;
    }
    return true;
}

bool DeriveKeys(const std::wstring& password, const uint8_t salt[kSaltLen],
                uint32_t iterations, uint8_t keys[64], std::wstring& err) {
    if (password.empty()) {
        err = Tr(L"密码不能为空");
        return false;
    }
    const std::string pwd = W2U8(password);
    if (pwd.empty()) {
        err = Tr(L"密码编码失败");
        return false;
    }
    Alg hmac;
    if (!OpenHmac(hmac, err))
        return false;
    NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hmac.h, (PUCHAR)pwd.data(), (ULONG)pwd.size(), (PUCHAR)salt, kSaltLen,
        iterations, keys, 64, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"口令派生失败（bcrypt 错误 ") +
              std::to_wstring((long long)st) + Tr(L"）");
        return false;
    }
    return true;
}

struct AesKey {
    Alg alg;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::vector<uint8_t> obj;
    ~AesKey() {
        if (key)
            BCryptDestroyKey(key);
    }
};

bool OpenAes(const uint8_t encKey[32], const wchar_t* mode, AesKey& out,
             std::wstring& err) {
    NTSTATUS st =
        BCryptOpenAlgorithmProvider(&out.alg.h, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"无法初始化 AES（bcrypt 错误 ") +
              std::to_wstring((long long)st) + Tr(L"）");
        return false;
    }
    st = BCryptSetProperty(out.alg.h, BCRYPT_CHAINING_MODE, (PUCHAR)mode,
                           (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t)), 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"设置 AES 工作模式失败");
        return false;
    }
    DWORD objLen = 0, cb = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(out.alg.h, BCRYPT_OBJECT_LENGTH,
                                          (PUCHAR)&objLen, sizeof(objLen), &cb,
                                          0)) ||
        objLen == 0) {
        err = Tr(L"读取 AES 参数失败");
        return false;
    }
    out.obj.assign(objLen, 0);
    st = BCryptGenerateSymmetricKey(out.alg.h, &out.key, out.obj.data(),
                                    objLen, (PUCHAR)encKey, 32, 0);
    if (!BCRYPT_SUCCESS(st)) {
        err = Tr(L"会话密钥创建失败");
        return false;
    }
    return true;
}

// v1 的 AES-256-CTR：counter = chunk(LE64) || block(LE64)
bool AesCtrXor(AesKey& aes, uint64_t chunk, uint8_t* data, size_t len,
               std::wstring& err) {
    if (len == 0)
        return true;
    const size_t nblocks = (len + 15) / 16;
    std::vector<uint8_t> ctr(nblocks * 16);
    std::vector<uint8_t> ks(nblocks * 16);
    for (size_t i = 0; i < nblocks; ++i) {
        const uint64_t b = (uint64_t)i;
        std::memcpy(&ctr[i * 16], &chunk, 8);
        std::memcpy(&ctr[i * 16 + 8], &b, 8);
    }
    ULONG outLen = 0;
    NTSTATUS st = BCryptEncrypt(aes.key, ctr.data(), (ULONG)ctr.size(), nullptr,
                                nullptr, 0, ks.data(), (ULONG)ks.size(),
                                &outLen, 0);
    if (!BCRYPT_SUCCESS(st) || outLen < (ULONG)len) {
        err = Tr(L"AES 密钥流生成失败");
        return false;
    }
    for (size_t i = 0; i < len; ++i)
        data[i] ^= ks[i];
    return true;
}

// ── v2：CBC-ESSIV（与 dm-crypt `aes-cbc-essiv:sha256` 同口径）──────────
// IV = AES-256-ECB(sha256(encKey), sectorNumber_LE64 || 0^8)
// 每个扇区独立 CBC（4096B，无填充），扇区号从 0 开始。
bool EssivCryptSectors(AesKey& dataAes, AesKey& ivAes, uint64_t startSector,
                       uint8_t* data, size_t len, bool encrypt,
                       std::wstring& err) {
    const size_t nsec = len / kSectorSizeV2;
    if (nsec == 0)
        return true;
    // 1) 批量算 IV（一次 ECB 调用）
    std::vector<uint8_t> ctr(nsec * 16, 0);
    std::vector<uint8_t> ivs(nsec * 16, 0);
    for (size_t i = 0; i < nsec; ++i) {
        const uint64_t s = startSector + i;
        std::memcpy(&ctr[i * 16], &s, 8);
    }
    ULONG outLen = 0;
    NTSTATUS st =
        BCryptEncrypt(ivAes.key, ctr.data(), (ULONG)ctr.size(), nullptr,
                      nullptr, 0, ivs.data(), (ULONG)ivs.size(), &outLen, 0);
    if (!BCRYPT_SUCCESS(st) || outLen != (ULONG)ivs.size()) {
        err = Tr(L"AES 密钥流生成失败");
        return false;
    }
    // 2) 每个扇区一次 CBC
    std::vector<uint8_t> out(kSectorSizeV2);
    for (size_t i = 0; i < nsec; ++i) {
        outLen = 0;
        // 注意：CBC 必须显式给 cbIV=16（传 0 会被 CNG 判成
        // STATUS_INVALID_BUFFER_SIZE=0xC0000206，实测踩到）。
        st = encrypt
                 ? BCryptEncrypt(dataAes.key, data + i * kSectorSizeV2,
                                 kSectorSizeV2, nullptr, &ivs[i * 16], 16,
                                 out.data(), kSectorSizeV2, &outLen, 0)
                 : BCryptDecrypt(dataAes.key, data + i * kSectorSizeV2,
                                 kSectorSizeV2, nullptr, &ivs[i * 16], 16,
                                 out.data(), kSectorSizeV2, &outLen, 0);
        if (!BCRYPT_SUCCESS(st) || outLen != kSectorSizeV2) {
            wchar_t dbg[96] = {};
            swprintf(dbg, 96, L" (st=%lld outLen=%lld)", (long long)st,
                     (long long)outLen);
            err = Tr(L"AES 扇区处理失败");
            err += dbg;
            return false;
        }
        std::memcpy(data + i * kSectorSizeV2, out.data(), kSectorSizeV2);
    }
    return true;
}

bool ReadExact(HANDLE h, void* buf, DWORD n) {
    size_t done = 0;
    while (done < n) {
        DWORD got = 0;
        if (!ReadFile(h, (uint8_t*)buf + done, n - (DWORD)done, &got, nullptr) ||
            got == 0)
            return false;
        done += got;
    }
    return true;
}

bool WriteExact(HANDLE h, const void* buf, DWORD n) {
    size_t done = 0;
    while (done < n) {
        DWORD put = 0;
        if (!WriteFile(h, (const uint8_t*)buf + done, n - (DWORD)done, &put,
                       nullptr) ||
            put == 0)
            return false;
        done += put;
    }
    return true;
}

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN))
        return false;
    return ReadExact(h, buf, n);
}

// ── meta 序列化（子镜像清单；二进制、长度前缀）──
void PutU32(std::vector<uint8_t>& v, uint32_t x) {
    v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4);
}
void PutU64(std::vector<uint8_t>& v, uint64_t x) {
    v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 8);
}
void PutStr(std::vector<uint8_t>& v, const std::wstring& s) {
    const std::string u8 = W2U8(s);
    PutU32(v, (uint32_t)u8.size());
    v.insert(v.end(), u8.begin(), u8.end());
}
bool GetU32(const std::vector<uint8_t>& v, size_t& p, uint32_t& x) {
    if (p + 4 > v.size())
        return false;
    std::memcpy(&x, &v[p], 4);
    p += 4;
    return true;
}
bool GetU64(const std::vector<uint8_t>& v, size_t& p, uint64_t& x) {
    if (p + 8 > v.size())
        return false;
    std::memcpy(&x, &v[p], 8);
    p += 8;
    return true;
}
bool GetStr(const std::vector<uint8_t>& v, size_t& p, std::wstring& s) {
    uint32_t n = 0;
    if (!GetU32(v, p, n) || p + n > v.size())
        return false;
    s = U82W(std::string((const char*)&v[p], n));
    p += n;
    return true;
}

std::vector<uint8_t> SerializeMeta(const std::vector<ZjeImage>& images) {
    std::vector<uint8_t> v;
    PutU32(v, 0x314D4A5Au);  // 'ZJM1'
    PutU32(v, (uint32_t)images.size());
    for (const auto& im : images) {
        PutU32(v, (uint32_t)im.index);
        PutStr(v, im.name);
        PutStr(v, im.description);
        PutU64(v, im.sizeBytes);
        PutU64(v, im.creationTime);
        PutU32(v, im.hasWinload ? 1u : 0u);
    }
    return v;
}

bool ParseMeta(const std::vector<uint8_t>& v, std::vector<ZjeImage>& images) {
    images.clear();
    size_t p = 0;
    uint32_t magic = 0, count = 0;
    if (!GetU32(v, p, magic) || magic != 0x314D4A5Au ||
        !GetU32(v, p, count))
        return false;
    if (count > 4096)
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        ZjeImage im;
        uint32_t idx = 0, hl = 0;
        if (!GetU32(v, p, idx) || !GetStr(v, p, im.name) ||
            !GetStr(v, p, im.description) || !GetU64(v, p, im.sizeBytes) ||
            !GetU64(v, p, im.creationTime) || !GetU32(v, p, hl))
            return false;
        im.index = (int)idx;
        im.hasWinload = hl != 0;
        images.push_back(std::move(im));
    }
    return true;
}

// meta 加解密（v1/v2 共用）：CTR(encKey, chunk=kMetaChunk) + HMAC(macKey)
bool EncryptMeta(const uint8_t keys[64], std::vector<uint8_t>& meta,
                 uint8_t mac[kMacLen], std::wstring& err) {
    AesKey aes;
    if (!OpenAes(keys, BCRYPT_CHAIN_MODE_ECB, aes, err))
        return false;
    if (!AesCtrXor(aes, kMetaChunk, meta.data(), meta.size(), err))
        return false;
    Alg hmac;
    if (!OpenHmac(hmac, err))
        return false;
    uint8_t idx[8];
    std::memcpy(idx, &kMetaChunk, 8);
    return HmacSha256(hmac.h, keys + 32, idx, 8, meta.data(), meta.size(), mac,
                      err);
}

bool DecryptMetaVerified(const uint8_t keys[64], std::vector<uint8_t>& meta,
                         const uint8_t mac[kMacLen], std::wstring& err) {
    Alg hmac;
    if (!OpenHmac(hmac, err))
        return false;
    uint8_t idx[8];
    std::memcpy(idx, &kMetaChunk, 8);
    uint8_t want[kMacLen] = {};
    if (!HmacSha256(hmac.h, keys + 32, idx, 8, meta.data(), meta.size(), want,
                    err))
        return false;
    if (!ConstantEq(want, mac, kMacLen)) {
        err = Tr(L"加密镜像元数据被篡改（校验失败）");
        return false;
    }
    AesKey aes;
    if (!OpenAes(keys, BCRYPT_CHAIN_MODE_ECB, aes, err))
        return false;
    if (!AesCtrXor(aes, kMetaChunk, meta.data(), meta.size(), err))
        return false;
    return true;
}

// 读 header + meta（不解正文）。成功 0；同时给出 keys / images / info。
int UnlockV2(HANDLE h, const std::wstring& password, HeaderV2& hdr,
             std::vector<ZjeImage>& images, uint8_t keys[64],
             std::wstring& err) {
    if (!ReadAt(h, 0, &hdr, sizeof(hdr))) {
        err = Tr(L"读取加密镜像头失败（文件被截断？）");
        return 1;
    }
    if (std::memcmp(hdr.magic, kMagic2, 8) != 0 || hdr.version != kVersion2 ||
        hdr.kdf != kKdfPbkdf2Sha256) {
        err = Tr(L"加密镜像版本不受支持（请用更新版本的 SysRecover）");
        return 1;
    }
    if (hdr.sectorSize != kSectorSizeV2) {
        err = Tr(L"加密镜像头损坏（扇区大小非法）");
        return 1;
    }
    if (hdr.iterations < 1000 || hdr.iterations > 10000000u ||
        hdr.plainSize == 0) {
        err = Tr(L"加密镜像头损坏（KDF 参数非法）");
        return 1;
    }
    if (!DeriveKeys(password, hdr.salt, hdr.iterations, keys, err))
        return 1;
    Alg hmac;
    if (!OpenHmac(hmac, err))
        return 1;
    uint8_t mac[kMacLen] = {};
    if (!HmacSha256(hmac.h, keys + 32, (const uint8_t*)&hdr, 48, nullptr, 0, mac,
                    err)) {
        err = Tr(L"加密镜像头校验失败");
        return 1;
    }
    if (!ConstantEq(mac, hdr.headerMac, kMacLen)) {
        err = Tr(L"密码错误（或镜像头被篡改）");
        return 1;
    }
    uint64_t metaLen64 = 0;
    if (!ReadAt(h, kHeaderSizeV2, &metaLen64, 8) || metaLen64 == 0 ||
        metaLen64 > kBodyOffsetV2 - kHeaderSizeV2 - 8 - kMacLen) {
        err = Tr(L"加密镜像头损坏（元数据长度非法）");
        return 1;
    }
    const uint32_t metaLen = (uint32_t)metaLen64;
    std::vector<uint8_t> meta(metaLen);
    if (!ReadExact(h, meta.data(), metaLen)) {
        err = Tr(L"读取加密镜像元数据失败（文件被截断？）");
        return 1;
    }
    uint8_t metaMac[kMacLen] = {};
    if (!ReadExact(h, metaMac, kMacLen)) {
        err = Tr(L"读取加密镜像元数据校验值失败（文件被截断？）");
        return 1;
    }
    if (!DecryptMetaVerified(keys, meta, metaMac, err))
        return 1;
    if (!ParseMeta(meta, images)) {
        err = Tr(L"加密镜像元数据格式错误");
        return 1;
    }
    return 0;
}

// v1 的 header + meta（只读）
int UnlockV1(HANDLE h, const std::wstring& password, HeaderV1& hdr,
             std::vector<ZjeImage>& images, uint8_t keys[64],
             std::wstring& err) {
    if (!ReadAt(h, 0, &hdr, sizeof(hdr))) {
        err = Tr(L"读取加密镜像头失败（文件被截断？）");
        return 1;
    }
    if (std::memcmp(hdr.magic, kMagic1, 8) != 0 ||
        hdr.version != kVersion1 || hdr.kdf != kKdfPbkdf2Sha256) {
        err = Tr(L"加密镜像版本不受支持（请用更新版本的 SysRecover）");
        return 1;
    }
    if (hdr.chunkSize < 4096 || hdr.chunkSize > 64u * 1024u * 1024u ||
        hdr.iterations < 1000 || hdr.iterations > 10000000u) {
        err = Tr(L"加密镜像头损坏（块大小非法）");
        return 1;
    }
    if (!DeriveKeys(password, hdr.salt, hdr.iterations, keys, err))
        return 1;
    Alg hmac;
    if (!OpenHmac(hmac, err))
        return 1;
    uint8_t mac[kMacLen] = {};
    if (!HmacSha256(hmac.h, keys + 32, (const uint8_t*)&hdr, 48, nullptr, 0, mac,
                    err)) {
        err = Tr(L"加密镜像头校验失败");
        return 1;
    }
    if (!ConstantEq(mac, hdr.headerMac, kMacLen)) {
        err = Tr(L"密码错误（或镜像头被篡改）");
        return 1;
    }
    uint64_t metaLen64 = 0;
    if (!ReadAt(h, sizeof(HeaderV1), &metaLen64, 8) || metaLen64 == 0 ||
        metaLen64 > 64ull * 1024ull * 1024ull) {
        err = Tr(L"加密镜像头损坏（元数据长度非法）");
        return 1;
    }
    const uint32_t metaLen = (uint32_t)metaLen64;
    std::vector<uint8_t> meta(metaLen);
    if (!ReadExact(h, meta.data(), metaLen)) {
        err = Tr(L"读取加密镜像元数据失败（文件被截断？）");
        return 1;
    }
    uint8_t metaMac[kMacLen] = {};
    if (!ReadExact(h, metaMac, kMacLen)) {
        err = Tr(L"读取加密镜像元数据校验值失败（文件被截断？）");
        return 1;
    }
    if (!DecryptMetaVerified(keys, meta, metaMac, err))
        return 1;
    if (!ParseMeta(meta, images)) {
        err = Tr(L"加密镜像元数据格式错误");
        return 1;
    }
    return 0;
}

}  // namespace

bool ZjeIsFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    uint8_t m[8] = {};
    DWORD got = 0;
    bool ok = ReadFile(h, m, 8, &got, nullptr) && got == 8 &&
              (std::memcmp(m, kMagic1, 8) == 0 ||
               std::memcmp(m, kMagic2, 8) == 0);
    CloseHandle(h);
    return ok;
}

int ZjeReadMeta(const std::wstring& zje, const std::wstring& password,
                ZjeInfo* info, std::vector<ZjeImage>* images,
                std::wstring& err) {
    err.clear();
    HANDLE h = CreateFileW(zje.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = Tr(L"打不开加密镜像（文件不存在或被占用）");
        return 1;
    }
    uint8_t magic[8] = {};
    if (!ReadAt(h, 0, magic, 8)) {
        CloseHandle(h);
        err = Tr(L"读取加密镜像头失败（文件被截断？）");
        return 1;
    }
    uint8_t keys[64] = {};
    std::vector<ZjeImage> imgs;
    int rc = 1;
    if (std::memcmp(magic, kMagic2, 8) == 0) {
        HeaderV2 hdr = {};
        rc = UnlockV2(h, password, hdr, imgs, keys, err);
        if (rc == 0 && info) {
            info->version = hdr.version;
            info->iterations = hdr.iterations;
            info->chunkSize = hdr.sectorSize;
            info->sectorSize = hdr.sectorSize;
            info->plainSize = hdr.plainSize;
            info->bodyOffset = kBodyOffsetV2;
        }
    } else {
        HeaderV1 hdr = {};
        rc = UnlockV1(h, password, hdr, imgs, keys, err);
        if (rc == 0 && info) {
            info->version = hdr.version;
            info->iterations = hdr.iterations;
            info->chunkSize = hdr.chunkSize;
            info->sectorSize = 0;
            info->plainSize = hdr.plainSize;
            info->bodyOffset = 0;
        }
    }
    CloseHandle(h);
    if (rc == 0 && images)
        *images = std::move(imgs);
    return rc;
}

bool ZjeCheckPassword(const std::wstring& zje, const std::wstring& password) {
    std::wstring err;
    return ZjeReadMeta(zje, password, nullptr, nullptr, err) == 0;
}

int ZjeEncryptFile(const std::wstring& plain, const std::wstring& out,
                   const std::wstring& password,
                   const std::vector<ZjeImage>& images, CryptProgress progress,
                   std::wstring& err) {
    err.clear();
    if (password.empty()) {
        err = Tr(L"密码不能为空");
        return 1;
    }
    HANDLE in = CreateFileW(plain.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        err = Tr(L"打不开要加密的镜像文件");
        return 1;
    }
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(in, &sz) || sz.QuadPart <= 0) {
        CloseHandle(in);
        err = Tr(L"镜像文件为空或大小读取失败");
        return 1;
    }
    const uint64_t plainSize = (uint64_t)sz.QuadPart;

    HeaderV2 hdr = {};
    std::memcpy(hdr.magic, kMagic2, 8);
    hdr.version = kVersion2;
    hdr.kdf = kKdfPbkdf2Sha256;
    hdr.iterations = kIterations;
    hdr.sectorSize = kSectorSizeV2;
    hdr.plainSize = plainSize;
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, hdr.salt, kSaltLen,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        CloseHandle(in);
        err = Tr(L"随机数生成失败（salt）");
        return 1;
    }
    uint8_t keys[64] = {};
    if (!DeriveKeys(password, hdr.salt, hdr.iterations, keys, err)) {
        CloseHandle(in);
        return 1;
    }
    Alg hmac;
    if (!OpenHmac(hmac, err) ||
        !HmacSha256(hmac.h, keys + 32, (const uint8_t*)&hdr, 48, nullptr, 0,
                    hdr.headerMac, err)) {
        CloseHandle(in);
        return 1;
    }
    // meta（子镜像清单）：CTR+HMAC，放在 header 之后、正文之前
    std::vector<uint8_t> meta = SerializeMeta(images);
    uint8_t metaMac[kMacLen] = {};
    if (!EncryptMeta(keys, meta, metaMac, err)) {
        CloseHandle(in);
        return 1;
    }
    const std::wstring tmp = out + L".tmp";
    DeleteFileW(tmp.c_str());
    HANDLE o = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) {
        CloseHandle(in);
        err = Tr(L"无法创建加密镜像文件（磁盘满/无权限？）");
        return 1;
    }
    bool ok = true;
    {
        const uint64_t metaLen = (uint64_t)meta.size();
        ok = WriteExact(o, &hdr, sizeof(hdr)) && WriteExact(o, &metaLen, 8) &&
             WriteExact(o, meta.data(), (DWORD)meta.size()) &&
             WriteExact(o, metaMac, kMacLen);
        // 显式补零到 1MB（正文起点固定，dm-crypt 才能按固定 offset 映射）
        uint64_t written =
            sizeof(hdr) + 8 + (uint64_t)meta.size() + (uint64_t)kMacLen;
        if (ok && written < kBodyOffsetV2) {
            std::vector<uint8_t> zeros(64 * 1024, 0);
            while (written < kBodyOffsetV2) {
                const DWORD n = (DWORD)std::min<uint64_t>(
                    zeros.size(), kBodyOffsetV2 - written);
                if (!WriteExact(o, zeros.data(), n)) {
                    ok = false;
                    break;
                }
                written += n;
            }
        }
    }
    AesKey dataAes, ivAes;
    uint8_t ivKey[32] = {};
    if (ok) {
        ok = Sha256Once(keys, 32, ivKey, err) &&
             OpenAes(keys, BCRYPT_CHAIN_MODE_CBC, dataAes, err) &&
             OpenAes(ivKey, BCRYPT_CHAIN_MODE_ECB, ivAes, err);
    }
    std::vector<uint8_t> buf;
    if (ok) {
        const size_t kBuf = 1024u * 1024u;  // 256 扇区
        buf.resize(kBuf);
        uint64_t sector = 0;
        uint64_t done = 0;
        while (done < plainSize && ok) {
            const size_t len = (size_t)std::min<uint64_t>(
                kBuf, ((plainSize - done + kSectorSizeV2 - 1) / kSectorSizeV2) *
                          kSectorSizeV2);
            // 读 len（最后一块可能超出 EOF → 补零）
            size_t got = 0;
            while (got < len) {
                DWORD rd = 0;
                const DWORD want = (DWORD)std::min<size_t>(len - got, 1u << 20);
                if (!ReadFile(in, buf.data() + got, want, &rd, nullptr))
                    break;
                if (rd == 0)
                    break;
                got += rd;
            }
            if (got < len)
                std::memset(buf.data() + got, 0, len - got);
            ok = EssivCryptSectors(dataAes, ivAes, sector, buf.data(),
                                   (len / kSectorSizeV2) * kSectorSizeV2, true,
                                   err);
            if (!ok)
                break;
            ok = WriteExact(o, buf.data(), (DWORD)len);
            if (!ok) {
                err = Tr(L"写入加密镜像失败（磁盘满/被拦截？）");
                break;
            }
            sector += len / kSectorSizeV2;
            done += std::min<uint64_t>(len, plainSize - done);
            if (progress && progress(done, plainSize)) {
                err = Tr(L"已取消");
                ok = false;
                break;
            }
        }
    }
    if (ok && !FlushFileBuffers(o))
        ok = false;
    CloseHandle(o);
    CloseHandle(in);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty())
            err = Tr(L"写入加密镜像失败（磁盘满/被拦截？）");
        return 1;
    }
    if (!MoveFileExW(tmp.c_str(), out.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = Tr(L"加密镜像改名失败（目标被占用？）");
        return 1;
    }
    return 0;
}

namespace {

// v1 正文：分块 CTR + 每块 HMAC
int DecryptV1Body(HANDLE in, const HeaderV1& hdr, const uint8_t keys[64],
                  const std::wstring& plain, CryptProgress progress,
                  std::wstring& err) {
    const uint64_t nchunks =
        (hdr.plainSize + hdr.chunkSize - 1) / hdr.chunkSize;
    uint64_t expected = sizeof(HeaderV1) + 8;
    {
        uint64_t metaLen64 = 0;
        if (!ReadAt(in, sizeof(HeaderV1), &metaLen64, 8)) {
            err = Tr(L"加密镜像头损坏");
            return 1;
        }
        expected += metaLen64 + kMacLen;
        for (uint64_t i = 0; i < nchunks; ++i) {
            const uint64_t len = std::min<uint64_t>(
                hdr.chunkSize, hdr.plainSize - i * hdr.chunkSize);
            expected += len + kMacLen;
        }
    }
    LARGE_INTEGER actual = {};
    if (!GetFileSizeEx(in, &actual) ||
        (uint64_t)actual.QuadPart != expected) {
        err = Tr(L"加密镜像被截断或损坏（文件大小不符）");
        return 1;
    }
    const std::wstring tmp = plain + L".tmp";
    DeleteFileW(tmp.c_str());
    HANDLE o = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) {
        err = Tr(L"无法创建解密临时文件（磁盘空间不足？）");
        return 1;
    }
    AesKey aes;
    Alg hmac;
    bool ok = OpenAes(keys, BCRYPT_CHAIN_MODE_ECB, aes, err) &&
              OpenHmac(hmac, err);
    std::vector<uint8_t> chunk;
    if (ok) {
        uint64_t metaLen64 = 0;
        ok = ReadAt(in, sizeof(HeaderV1), &metaLen64, 8);
        if (ok) {
            LARGE_INTEGER li;
            li.QuadPart = (LONGLONG)(sizeof(HeaderV1) + 8 + metaLen64 + kMacLen);
            ok = SetFilePointerEx(in, li, nullptr, FILE_BEGIN) != 0;
        }
    }
    if (ok) {
        chunk.resize((size_t)hdr.chunkSize);
        uint64_t done = 0;
        for (uint64_t i = 0; i < nchunks && ok; ++i) {
            const size_t len = (size_t)std::min<uint64_t>(
                hdr.chunkSize, hdr.plainSize - i * hdr.chunkSize);
            ok = ReadExact(in, chunk.data(), (DWORD)len);
            uint8_t mac[kMacLen] = {};
            uint8_t want[kMacLen] = {};
            if (ok)
                ok = ReadExact(in, mac, kMacLen);
            if (!ok) {
                err = Tr(L"加密镜像被截断（块数据不完整）");
                break;
            }
            uint8_t idx[8];
            std::memcpy(idx, &i, 8);
            if (!HmacSha256(hmac.h, keys + 32, idx, 8, chunk.data(), len, want,
                            err)) {
                ok = false;
                break;
            }
            if (!ConstantEq(want, mac, kMacLen)) {
                err = Tr(L"加密镜像数据校验失败（被篡改或损坏）");
                ok = false;
                break;
            }
            if (!AesCtrXor(aes, i, chunk.data(), len, err)) {
                ok = false;
                break;
            }
            if (!WriteExact(o, chunk.data(), (DWORD)len)) {
                err = Tr(L"写入解密文件失败（磁盘空间不足？）");
                ok = false;
                break;
            }
            done += len;
            if (progress && progress(done, hdr.plainSize)) {
                err = Tr(L"已取消");
                ok = false;
                break;
            }
        }
    }
    if (ok && !FlushFileBuffers(o))
        ok = false;
    CloseHandle(o);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty())
            err = Tr(L"解密失败");
        return 1;
    }
    if (!MoveFileExW(tmp.c_str(), plain.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = Tr(L"解密文件改名失败（目标被占用？）");
        return 1;
    }
    return 0;
}

int DecryptV2Body(HANDLE in, const HeaderV2& hdr, const uint8_t keys[64],
                  const std::wstring& plain, CryptProgress progress,
                  std::wstring& err) {
    const uint64_t bodyLen =
        ((hdr.plainSize + kSectorSizeV2 - 1) / kSectorSizeV2) * kSectorSizeV2;
    LARGE_INTEGER actual = {};
    if (!GetFileSizeEx(in, &actual) ||
        (uint64_t)actual.QuadPart != kBodyOffsetV2 + bodyLen) {
        err = Tr(L"加密镜像被截断或损坏（文件大小不符）");
        return 1;
    }
    const std::wstring tmp = plain + L".tmp";
    DeleteFileW(tmp.c_str());
    HANDLE o = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (o == INVALID_HANDLE_VALUE) {
        err = Tr(L"无法创建解密临时文件（磁盘空间不足？）");
        return 1;
    }
    AesKey dataAes, ivAes;
    uint8_t ivKey[32] = {};
    bool ok = Sha256Once(keys, 32, ivKey, err) &&
              OpenAes(keys, BCRYPT_CHAIN_MODE_CBC, dataAes, err) &&
              OpenAes(ivKey, BCRYPT_CHAIN_MODE_ECB, ivAes, err);
    if (ok) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)kBodyOffsetV2;
        ok = SetFilePointerEx(in, li, nullptr, FILE_BEGIN) != 0;
    }
    std::vector<uint8_t> buf;
    if (ok) {
        const size_t kBuf = 1024u * 1024u;
        buf.resize(kBuf);
        uint64_t sector = 0;
        uint64_t readDone = 0;  // 正文读取位置（含尾部补零）
        uint64_t written = 0;   // 已写出的明文字节数
        while (readDone < bodyLen && ok) {
            const size_t len =
                (size_t)std::min<uint64_t>(kBuf, bodyLen - readDone);
            ok = ReadExact(in, buf.data(), (DWORD)len);
            if (!ok) {
                err = Tr(L"加密镜像被截断（块数据不完整）");
                break;
            }
            ok = EssivCryptSectors(dataAes, ivAes, sector, buf.data(),
                                   (len / kSectorSizeV2) * kSectorSizeV2, false,
                                   err);
            if (!ok)
                break;
            // 最后一个扇区含补零，只写 plainSize 以内的字节
            const size_t writable = (size_t)std::min<uint64_t>(
                len, hdr.plainSize - written);
            ok = WriteExact(o, buf.data(), (DWORD)writable);
            if (!ok) {
                err = Tr(L"写入解密文件失败（磁盘空间不足？）");
                break;
            }
            sector += len / kSectorSizeV2;
            readDone += len;
            written += writable;
            if (progress && progress(written, hdr.plainSize)) {
                err = Tr(L"已取消");
                ok = false;
                break;
            }
        }
    }
    if (ok && !FlushFileBuffers(o))
        ok = false;
    CloseHandle(o);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty())
            err = Tr(L"解密失败");
        return 1;
    }
    if (!MoveFileExW(tmp.c_str(), plain.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        err = Tr(L"解密文件改名失败（目标被占用？）");
        return 1;
    }
    return 0;
}

}  // namespace

int ZjeDecryptFile(const std::wstring& zje, const std::wstring& plain,
                   const std::wstring& password, CryptProgress progress,
                   std::wstring& err) {
    err.clear();
    HANDLE in = CreateFileW(zje.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) {
        err = Tr(L"打不开加密镜像（文件不存在或被占用）");
        return 1;
    }
    uint8_t magic[8] = {};
    if (!ReadAt(in, 0, magic, 8)) {
        CloseHandle(in);
        err = Tr(L"读取加密镜像头失败（文件被截断？）");
        return 1;
    }
    uint8_t keys[64] = {};
    std::vector<ZjeImage> imgs;
    int rc;
    if (std::memcmp(magic, kMagic2, 8) == 0) {
        HeaderV2 hdr = {};
        rc = UnlockV2(in, password, hdr, imgs, keys, err);
        CloseHandle(in);
        if (rc == 0)
            rc = [&] {
                HANDLE h = CreateFileW(zje.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE)
                    return 1;
                int r = DecryptV2Body(h, hdr, keys, plain, progress, err);
                CloseHandle(h);
                return r;
            }();
        return rc;
    }
    HeaderV1 hdr = {};
    rc = UnlockV1(in, password, hdr, imgs, keys, err);
    CloseHandle(in);
    if (rc == 0)
        rc = [&] {
            HANDLE h = CreateFileW(zje.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
            if (h == INVALID_HANDLE_VALUE)
                return 1;
            int r = DecryptV1Body(h, hdr, keys, plain, progress, err);
            CloseHandle(h);
            return r;
        }();
    return rc;
}

std::wstring ZjeTempPlainPath(const std::wstring& zje) {
    size_t slash = zje.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : zje.substr(0, slash);
    wchar_t buf[96] = {};
    swprintf(buf, 96, L"\\~zjenc-%lu-%llu.tmp",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    return dir + buf;
}

}  // namespace sysrecover
