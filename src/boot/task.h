#pragma once
// 还原任务暂存：restore-task.json + restore-task.conf（key=value sidecar）
//             + 目标分区根目录 _zjresy*.log（restore.sh 主契约）。
// 字段契约见 AGENTS.md §5，改字段需双端发版（contract_version=1）。
#include <cstdint>
#include <string>

namespace sysrecover {

struct RestoreTask {
    std::wstring imagePath;      // 镜像绝对路径
    std::wstring imagePartGuid;  // 镜像所在分区 GUID（MBR 下可空）
    std::wstring imageRelPath;   // 镜像相对路径（正斜杠，供 Linux 侧）
    int imageIndex = 1;
    std::wstring targetGuid;     // 目标分区 GUID（MBR 下可空）
    uint64_t targetOffset = 0;
    uint64_t targetSize = 0;
    std::wstring targetDiskSerial;
    int targetDisk = 0;
    int targetPart = 0;
    std::string ptType = "mbr";  // mbr | gpt
    bool repairBoot = true;
    uint32_t partCount = 1;
    // _zjresy 恢复日志附加字段（对齐旧 C# WriteRestoreLog，Phase 5 补）
    std::wstring targetDiskName;   // 磁盘型号
    uint64_t targetDiskSize = 0;   // 整盘字节数
    std::wstring targetFs;         // 目标文件系统（NTFS...）
    std::wstring targetVolLabel;   // 目标卷标
    std::wstring softwarePath;     // 本程序 exe 全路径
    // 诊断日志目录（PIT-059）：一般是软件所在目录；软件在目标盘/只读介质上时
    // 回退到数据盘 <数据盘>\ZJRESTORE。Linux 侧按它把日志写进去（成功后保留）。
    std::wstring softwareDir;
};

// 写任务到 dir（json + conf 各一份）。成功返回 true。
// conf 含 image_path 键（restore.sh 第 7 步 get image_path 依赖）。
bool WriteRestoreTask(const std::wstring& dir, const RestoreTask& t,
                      std::string& log);

// —— 以下三个是**纯函数**（不碰文件系统），抽出来便于单元测试契约文本 ——
// 任务 sidecar（key=value，Linux restore.sh 读）。
std::string BuildTaskConf(const RestoreTask& t);
// 任务的 JSON 形态（同内容，供 Windows 侧/GUI 读）。
std::string BuildTaskJson(const RestoreTask& t);
// _zjresy 恢复日志文本（主发现契约，AGENTS.md §5）。
// isoTimestamp 由调用方给（便于测试；生产用当前本地时间）。
std::string BuildRestoreLogText(const RestoreTask& t,
                                const std::string& isoTimestamp);

// 写 _zjresy{日时分}.log 到目标分区根目录，写前先清同模式旧文件。
// letter=目标盘符（如 L"C"）。这是 restore.sh 的主发现契约：
// Linux 侧扫描各分区根目录，含 action=restore 的日志所在分区即还原目标。
// 失败返回 false（暂存流程应视为致命，缺日志则 Linux 侧找不到目标）。
bool WriteRestoreLog(wchar_t letter, const RestoreTask& t, std::string& log);

}  // namespace sysrecover
