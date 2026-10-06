#pragma once
// WIM 引擎：直接链接 libwim（LGPLv3），不调 exe、不解析文本。
// wimlib.h 不向外泄漏（前置声明 WIMStruct），见 AGENTS.md §8。
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct WIMStruct;

namespace sysrecover {

struct ImageDesc {
    int index = 0;  // 1-based
    std::wstring name;
    std::wstring description;             // 描述（可能为空）
    unsigned long long sizeBytes = 0;     // **实际占用** = TOTALBYTES − HARDLINKBYTES（PIT-073）
    unsigned long long creationTime = 0;  // FILETIME(UTC)，0 = 未知
};

// 进度回调：percent 0-100，stage 为阶段文本；返回 true 表示取消。
using ProgressFn = std::function<bool(int, const std::string&)>;

class WimEngine {
public:
    WimEngine();
    ~WimEngine();
    WimEngine(const WimEngine&) = delete;
    WimEngine& operator=(const WimEngine&) = delete;

    bool ok() const { return inited_; }

    // compress: "fast"(XPRESS) | "maximum"(LZX) | "recovery"(LZMS+solid)
    // snapshot=true 时做 VSS 快照（热备系统盘用）。
    // configFile: WimScript.ini 风格排除配置（空=无排除；热备必须给，否则易失文件导致 rc=88）。
    int Capture(const std::wstring& source, const std::wstring& imagePath,
                const std::string& compress, const std::wstring& name,
                bool snapshot, const std::wstring& configFile,
                ProgressFn progress);
    // desc 非空时给新子镜像写 <DESCRIPTION>（ESP 并入用 name="ESP" + 注释）。
    // 原子性：追加在**目标文件内**进行（wimlib_overwrite），调用方如需"ESP 失败
    // 不损坏主镜像"要自己走中转文件（见 ops.cpp::RunBackup 的 .stage 流程）。
    int Append(const std::wstring& source, const std::wstring& imagePath,
               const std::string& compress, const std::wstring& name,
               bool snapshot, const std::wstring& configFile,
               ProgressFn progress, const std::wstring& desc = L"");
    int Apply(const std::wstring& imagePath, int index,
              const std::wstring& target, ProgressFn progress);
    // progress：校验进度（wimlib VERIFY_STREAMS 有真实字节数；不给回调也行）。
    int Verify(const std::wstring& imagePath, ProgressFn progress = {});
    // 可用性**快检**（秒级；还原暂存前必做、备份写出后也必做）：能打开 + 没有
    // "写入未完成"标记 + 至少一个子镜像（防 PIT-057 的"半截镜像"黑屏）。
    // ⚠️ 不做全文件校验（PIT-106）：大镜像全量扫描要十几分钟，用户无法忍受；
    // 需要全量校验请显式调 Verify（--verify / verify 命令）。
    // 返回 0=可用；非 0=不可用（why 填原因，UTF-16）。
    int Probe(const std::wstring& imagePath, std::wstring& why);
    int ListImages(const std::wstring& imagePath,
                   std::vector<ImageDesc>& out);
    // 取某个子镜像的**未压缩**内容大小（字节）—— 还原前空间预检（P1）用。
    // 返回：0 成功；非 0 = wimlib 错误码（调用方自行决定是否阻断）。
    int ImageSize(const std::wstring& imagePath, int index,
                  unsigned long long* bytes);
    // 从镜像里**提取指定路径**到 destDir（该目录必须已存在）。
    // paths 用 Windows 风格、以 \ 开头，支持通配符，例如：
    //   L"\\Windows\\win.ini"   L"\\Users\\*\\Desktop\\*.txt"
    // 返回 0 成功；非 0 = wimlib 错误码。
    int ExtractPaths(const std::wstring& imagePath, int index,
                     const std::vector<std::wstring>& paths,
                     const std::wstring& destDir);

    // 镜像里**是否存在某路径**（只读元数据、不解压文件数据 —— solid ESD 也快）。
    // 用途：不支持的系统预检（I-1，2026-10-06）：Win7+ 必有
    // `\Windows\system32\winload.exe`，Vista 以前（XP/2003/2000/98）没有 →
    // 还原前拒绝，避免"格式化完才发现起不来"。
    // 返回 0 = 查询成功（*exists 有效；路径不存在也是成功）；非 0 = wimlib 错误码。
    int ImagePathExists(const std::wstring& imagePath, int index,
                        const std::wstring& path, bool* exists);

    static const wchar_t* ErrorString(int code);

private:
    bool inited_ = false;
};

}  // namespace sysrecover
