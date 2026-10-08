#pragma once
// EA（NTFS 扩展属性）跨层修复核心（PIT-122/123，2026-10-06）
//
// 背景：Linux 侧 wimlib apply 会丢弃 Windows EA（apply.out 报
// `Ignoring extended attributes of N files`，客户实测 804 个文件，见 PIT-120）。
// Windows 侧 apply 不丢（libwim 支持），但重启类还原必须走 Linux（§2 红线 1）。
// 方案（QEMU 全链实测选定）：
//   备份时：扫描源里带 EA 的文件 → eapack.dat（路径+名字+值，二进制安全）
//           → 连同补写器/策略片段打成子镜像 ZJEA 并入主镜像；
//   还原时：救援层把 eapack.dat + 补写器投放进目标，并安装 **本地组策略启动
//           脚本钩子**（登录前、SYSTEM 执行）；
//   首启时：补写器用 NtSetEaFile 把 EA 写回，随后自清理（还原策略/脚本/文件）。
//
// 本模块只做纯逻辑 + Win32 文件访问；不依赖 wimlib / UI / i18n。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sysrecover {
namespace ea {

struct EaValue {
    std::string name;                 // ASCII 名字（无终止符）
    std::vector<unsigned char> value; // 原始值字节
};

struct FileEntry {
    std::wstring relPath;             // 相对卷根路径（如 L"Windows\\a.dll"，无反斜杠开头）
    std::vector<EaValue> eas;
};

// ── 打包格式 ZJEA1（小端）────────────────────────────────────────────
//   magic 8B: 'Z','J','E','A','1',0,0,0
//   u32 fileCount
//   每个文件: u32 pathBytes(UTF-16LE 无终止) + path + u32 eaCount
//     每个 EA: u8 nameLen + u8 flags + u32 valueLen + name(ASCII) + value
// 解析失败（截断/超限）→ 返回 false + why。
std::vector<unsigned char> BuildPack(const std::vector<FileEntry>& files);
bool ParsePack(const unsigned char* data, size_t len,
               std::vector<FileEntry>& out, std::string& why);

// ── registry.pol 片段（同步策略，QEMU 实测定稿，PIT-123）──────────────
// 两条记录（REG_DWORD=1）：
//   Software\Microsoft\Windows\CurrentVersion\Policies\System!RunStartupScriptSync
//   Software\Policies\Microsoft\Windows NT\CurrentVersion\Winlogon!SyncForegroundPolicy
// 没有它们，Win10 的"启动脚本异步执行"会让补写器在 explorer 之后才跑
// （实测登录后 ~1 分钟），赶不上"桌面加载前修好 EA"。
// 返回**完整 pol 文件**（含 8 字节头）；追加到已有 pol 时调用方要剥掉前 8 字节。
std::vector<unsigned char> BuildSyncPolFragment();

// 从已有 pol 里剥掉"我们这两条记录"（按键+名精确匹配）。解析异常时原样返回。
std::vector<unsigned char> StripOurPolRecords(const std::vector<unsigned char>& data,
                                              bool* removedAny);

// ── 本地 GPO 文件（applier 清理用）────────────────────────────────
// scripts.ini：删除名字为 scriptName（不区分大小写）的 `NcmdLine=` 及其配套
// `Nparameters=` 行（同 N）。返回是否有改动。
bool RemoveScriptEntry(std::string& text, const std::string& scriptName);

// gpt.ini：把 `Version=N` 递增 1（让 gpsvc 下次识别为变更、重新处理）。
bool BumpGptIniVersion(std::string& text);

// ── 二进制文件读写 ─────────────────────────────────────────────────
bool WriteBytes(const std::wstring& path, const std::vector<unsigned char>& data);
bool ReadBytes(const std::wstring& path, std::vector<unsigned char>& out);

// ── EA 采集（备份端；root 为卷根如 L"C:"，路径相对 root 记录）────────
// 一次遍历同时产出：审计清单文本（auditPath，可为空）+ eapack.dat（packPath）。
// 返回带 EA 的文件数；-1 = 根不存在/写失败（err 填原因）。超过内部上限的
// 文件只记审计不打包（保证失败可解释、不爆内存）。
int CaptureVolume(const std::wstring& root, const std::wstring& packPath,
                  const std::wstring& auditPath, std::string& err);

// ── 非微软重解析点（PIT-135，2026-10-08）────────────────────────────
// Linux 侧 wimlib（NTFS-3G 块模式）**无法设置非微软（ISV）重解析点**——
// libntfs-3g 已知 bug（wimlib NEWS 原文："remains broken in NTFS-3G mode due
// to a libntfs-3g bug"）；实测 Intel ipfsrv.dptf（自定义 tag）令整个 apply
// rc=58 中止、目标分区只剩半成品（客户 20261008 三盘机）。Windows 侧（PE 就地
// 还原）无此问题。判定：tag 的 bit31（0x80000000）为 0 = 非微软（微软 tag 均置位）。
bool IsNonMsReparseTag(unsigned long tag);
// 快速遍历 root，收集带非微软重解析点的文件（相对路径，如
// \Windows\...\ipfsrv.dptf）；备份时并入排除清单，避免 Linux 还原必失败。
// 不打开文件（只 FindFirstFile 读 dwReserved0），比 EA 扫描快。返回个数（cap 上限）。
int FindNonMsReparse(const std::wstring& root, std::vector<std::wstring>& out,
                     size_t cap = 64);

// ── EA 补写（首启补写器用）────────────────────────────────────────
// 构造 NtSetEaFile 用的 FILE_FULL_EA_INFORMATION 完整缓冲区（含最外层头）。
// 纯逻辑，可单测（校验布局/长度）。
std::vector<unsigned char> BuildSetEaBuffer(const std::vector<EaValue>& eas);

struct ApplyStats {
    unsigned long long files = 0;     // 尝试
    unsigned long long ok = 0;        // 写成功
    unsigned long long missing = 0;   // 文件不存在
    unsigned long long failed = 0;    // 其它失败（权限/状态码）
    unsigned long lastStatus = 0;     // 最后一个非 0 状态码
    std::string firstErrors;          // 前几个失败样本（诊断用，含文件名）
};
// 把 pack 里的 EA 写回 root（如 L"C:"）。返回 0 = 全部处理完（允许 missing），
// 1 = 有 failed。日志行交给 logLine（可空）。
int ApplyPack(const std::wstring& packPath, const std::wstring& root,
              ApplyStats& stats,
              const std::function<void(const std::string&)>& logLine);

// ── 首启清理（applier 成功后调用）────────────────────────────────
// 目标根（系统盘）下：Scripts\Startup\zj-ea-restore.cmd（延迟删除）、
// scripts.ini 删条目、gpt.ini 版本 +1、Registry.pol 剥记录、eapack.dat 删除、
// 自身 exe 延迟删除。任一步失败只记录不中断。logLine 可为空。
void CleanupHookFiles(const std::wstring& root,
                      const std::function<void(const std::string&)>& logLine);

}  // namespace ea
}  // namespace sysrecover
