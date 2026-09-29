#pragma once
// 进度文件：logs/progress.json（Phase/Percent/Status/Detail/UpdatedAt/Pid）。
// 供 AI/外部工具读取；写文件失败静默忽略。
#include <string>

namespace sysrecover {

void ProgressInit(const std::wstring& logsDir);
void ProgressUpdate(const std::string& phase, int percent,
                    const std::string& status);

// 收尾。成功（status == "done" / "staged"）写 100%；**失败写"最后一个已知进度"**
//（此前无论成败都硬写 100 —— 失败时显示 100% 会误导排障，见 docs/15 §7-P7b）。
void ProgressDone(const std::string& phase, const std::string& status);

}  // namespace sysrecover
