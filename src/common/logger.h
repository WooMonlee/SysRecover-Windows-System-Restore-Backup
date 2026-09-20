#pragma once
// 日志：程序目录 logs/SysRecover-YYYYMMDD.log 为主；写失败静默忽略。
// 同时回显到控制台（INFO/WARN 去 stdout，ERROR 去 stderr）。
#include <string>

namespace sysrecover {

void LogInit(const std::wstring& logDir);
void LogInfo(const std::string& msg);
void LogWarn(const std::string& msg);
void LogError(const std::string& msg);

}  // namespace sysrecover
