#pragma once
// 镜像加密容器（docs/30）—— **ZJENC2**：Windows 与 Linux（dm-crypt）都能读。
//
// 为什么从 v1（分块 CTR + 每块 HMAC）改成 v2：
//   v1 的正文是"密文 + 每块 32B 认证码"交错，dm-crypt 映射不了（它只会整段线性映射），
//   于是 Linux 侧只能先把整包解密成临时明文 —— 多占一份空间、多一遍全量 IO。
//   v2 把正文改成**连续的单流** `aes-cbc-essiv:sha256`（4096B 扇区），
//   Linux 直接用 loop + dm-crypt 映射成块设备交给 wimlib，**零额外空间、一遍读完**。
//
// 容器布局（v2）：
//   0x0000  Header 4096B（明文）：
//             magic "ZJENC2\0\0" / version=2 / kdf=PBKDF2-SHA256 / iters /
//             sectorSize=4096 / plainSize / salt[16] / headerMac[32]
//   0x1000  metaLen(u64) + metaCipher + metaMac(32)   ← 子镜像清单（CTR+HMAC，
//           与 v1 同款；这样 GUI 列表/空间预估不必解密整包）
//   0x10000 正文：CBC-ESSIV-SHA256，扇区 4096，长度 = round_up(plainSize, 4096)
//
// 密钥：PBKDF2-HMAC-SHA256(口令, salt, 600000) → 64B
//       encKey = [0..31]，macKey = [32..63]；ESSIV 的 IV 密钥 = SHA256(encKey)。
// 认证：头 MAC（错口令立刻可区分）+ meta MAC。正文的完整性由 WIM 自带的
//       integrity 表（我们备份时总是 CHECK_INTEGRITY）在 apply 时兜底。
//
// ⚠️ v1 只保留**读取**（已经生成的 .zje 仍能打开）；新写入一律 v2。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sysrecover {

struct ZjeInfo {
    uint32_t version = 0;   // 1 = 分块 CTR+HMAC；2 = CBC-ESSIV（dm-crypt 可读）
    uint32_t iterations = 0;
    uint32_t chunkSize = 0;  // v1 的块大小 / v2 的 sectorSize（都填这里）
    uint32_t sectorSize = 0; // v2 专用（4096），v1 为 0
    uint64_t plainSize = 0;
    uint64_t bodyOffset = 0; // v2 正文起点（dm-crypt 映射用）
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

// 只读 magic（不需要密码；v1/v2 都认）。
bool ZjeIsFile(const std::wstring& path);

// 加密 plain → out（.zje，**总是 v2**）。
// 原子写：先写 <out>.tmp，成功才改名。返回 0 成功；非 0 失败（err=原因）。
int ZjeEncryptFile(const std::wstring& plain, const std::wstring& out,
                   const std::wstring& password,
                   const std::vector<ZjeImage>& images, CryptProgress progress,
                   std::wstring& err);

// 解密 zje → plain（覆盖写；v1/v2 都支持）。
// 错密码/篡改/截断 → 非 0（不留下半个明文文件）。
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
