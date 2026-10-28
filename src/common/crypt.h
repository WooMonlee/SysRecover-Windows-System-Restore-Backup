#pragma once
// 镜像加密容器 ZJENC1（docs/30，2026-10-28 用户规格：一个口令护全镜像）。
//
// 设计要点（为什么不是"改 WIM 头"）：
//   · wimlib 没有任何加密 API（只有 WIMLIB_ERR_WIM_IS_ENCRYPTED），且按**随机
//     访问**读文件 → 加密层必须包在 WIM **外面**，并且**按块**加密；
//   · 明文外壳头只放"怎么解"（KDF 参数/salt/块大小/明文长度），让 Linux 侧能
//     认出这是加密镜像并给出准确提示（而不是当成坏镜像）；
//   · 摘要（头 MAC + 每块 MAC）让错密码/篡改/截断一律 fail-closed。
//
// 算法（Win7 可用的 CNG 组合；GCM 在 Win7 的 CNG 上不保证可用，故不用）：
//   PBKDF2-HMAC-SHA256(口令, salt, 600000) -> 64B = encKey[32] + macKey[32]
//   正文：AES-256-CTR（counter = chunkIndex(LE64) || blockIndex(LE64)），
//         每块 = 密文 + HMAC-SHA256(macKey, chunkIndex || 密文)
//
// 容器布局：
//   Header(80B) | metaLen(u64) | metaCipher | metaMac(32) | chunk0 | chunk1 | ...
//   meta 用 chunkIndex = 0xFFFFFFFFFFFFFFFF（与正文块索引不冲突），内含
//   WIM 的子镜像清单（名称/描述/占用/时间/是否含 winload.exe）—— 这样
//   GUI 选镜像、空间预估都不必先把几十 GB 解密一遍。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sysrecover {

struct ZjeInfo {
    uint32_t version = 0;
    uint32_t iterations = 0;
    uint32_t chunkSize = 0;
    uint64_t plainSize = 0;
};

// 加密时写进 meta 的子镜像摘要（来源：WIM 元数据 XML + winload 探测）。
struct ZjeImage {
    int index = 0;
    std::wstring name;
    std::wstring description;
    unsigned long long sizeBytes = 0;     // 实际占用（TOTALBYTES − HARDLINKBYTES）
    unsigned long long creationTime = 0;  // FILETIME(UTC)，0=未知
    bool hasWinload = false;              // 含 \Windows\system32\winload.exe
};

// 进度：done/total 字节；total=0 表示未知。返回 true = 取消。
using CryptProgress =
    std::function<bool(unsigned long long done, unsigned long long total)>;

// 只读前 8 字节 magic（不需要密码；用于选择框过滤/分支）。
bool ZjeIsFile(const std::wstring& path);

// 加密 plain → out（.zje）。images 写进 meta（可为空，但还原侧会缺子镜像清单）。
// 原子写：先写 <out>.tmp，成功才改名。返回 0 成功；非 0 失败（err=原因）。
int ZjeEncryptFile(const std::wstring& plain, const std::wstring& out,
                   const std::wstring& password,
                   const std::vector<ZjeImage>& images, CryptProgress progress,
                   std::wstring& err);

// 解密 zje → plain（覆盖写）。错密码/篡改/截断 → 非 0（不留下半个明文文件）。
int ZjeDecryptFile(const std::wstring& zje, const std::wstring& plain,
                   const std::wstring& password, CryptProgress progress,
                   std::wstring& err);

// 只解 meta（快；需要密码）。info/images 可为空。
int ZjeReadMeta(const std::wstring& zje, const std::wstring& password,
                ZjeInfo* info, std::vector<ZjeImage>* images,
                std::wstring& err);

// 密码是否正确（= ZjeReadMeta 成功；不解正文）。
bool ZjeCheckPassword(const std::wstring& zje, const std::wstring& password);

// 生成"同卷临时明文"路径（调用方负责删除；不创建文件）。
std::wstring ZjeTempPlainPath(const std::wstring& zje);

}  // namespace sysrecover
