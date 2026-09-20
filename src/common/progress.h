#pragma once
// 进度文件：logs/progress.json（Phase/Percent/Status/Detail/UpdatedAt/Pid）。
// 供 AI/外部工具读取；写文件失败静默忽略。
#include <string>

namespace sysrecover {

void ProgressInit(const std::wstring& logsDir);
void ProgressUpdate(const std::string& phase, int percent,
                    const std::string& status);
void ProgressDone(const std::string& phase, const std::string& status);

}  // namespace sysrecover
