#pragma once
// 崩溃处理：把**未处理异常**（访问冲突等致命异常）写成 minidump + 可读文本，
// 落到 <baseDir>\logs\crash\，供用户直接把那个目录发回来排错。
//
// 借鉴自 DreamGrain 电子教室的 CrashHandler（见 AGENTS §17）：
//   · dump 写盘用系统 dbghelp.dll 的 MiniDumpWriteDump —— **动态加载**，不引入链接依赖；
//   · 同时写一份**可读文本**（异常码/地址/调用栈地址/模块基址/OS/版本/命令行）；
//   · 只处理"未处理异常"（SetUnhandledExceptionFilter）——不装 VEH，避免把
//     C++ 里正常的首次异常也当崩溃（那是日志噪音，不是崩溃）。
#include <string>
#include <vector>

namespace sysrecover {

// baseDir 一般是 ExeDir()；内部拼 `\logs\crash\` 并自动建目录。
// 失败（目录建不了等）静默忽略 —— 崩溃处理本身绝不能把程序拖垮。
void InstallCrashHandler(const std::wstring& baseDir);

// 「日志」按钮用：给指定进程写迷你转储（MiniDumpNormal|WithThreadInfo ——
// 含全部线程栈、体积小，专用于分析卡死/等待链）。返回 false = 打开进程 /
// 加载 dbghelp / 写盘任一失败（进程已退出也会失败，调用方按个数展示即可）。
bool WriteProcessMiniDump(unsigned long pid, const std::wstring& dmpPath);

// 按可执行文件名（不含路径，大小写不敏感；如 L"explorer.exe"）枚举 PID。
std::vector<unsigned long> FindProcessIdsByName(const wchar_t* exeName);

// 供诊断/测试用：返回崩溃目录（<baseDir>\logs\crash）。
std::wstring CrashDir(const std::wstring& baseDir);

}  // namespace sysrecover
