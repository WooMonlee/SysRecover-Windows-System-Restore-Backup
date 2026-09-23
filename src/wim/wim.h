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
    int Append(const std::wstring& source, const std::wstring& imagePath,
               const std::string& compress, const std::wstring& name,
               bool snapshot, const std::wstring& configFile,
               ProgressFn progress);
    int Apply(const std::wstring& imagePath, int index,
              const std::wstring& target, ProgressFn progress);
    int Verify(const std::wstring& imagePath);
    // 可用性检查（还原暂存前必做）：打开镜像并检查是否"写入未完成/不完整"。
    // 返回 0=可用；非 0=不可用（why 填原因，UTF-16）。
    int Probe(const std::wstring& imagePath, std::wstring& why);
    int ListImages(const std::wstring& imagePath,
                   std::vector<ImageDesc>& out);
    // 取某个子镜像的**未压缩**内容大小（字节）—— 还原前空间预检（P1）用。
    // 返回：0 成功；非 0 = wimlib 错误码（调用方自行决定是否阻断）。
    int ImageSize(const std::wstring& imagePath, int index,
                  unsigned long long* bytes);

    static const wchar_t* ErrorString(int code);

private:
    bool inited_ = false;
};

}  // namespace sysrecover
