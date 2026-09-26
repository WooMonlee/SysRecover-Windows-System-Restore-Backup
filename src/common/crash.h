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

namespace sysrecover {

// baseDir 一般是 ExeDir()；内部拼 `\logs\crash\` 并自动建目录。
// 失败（目录建不了等）静默忽略 —— 崩溃处理本身绝不能把程序拖垮。
void InstallCrashHandler(const std::wstring& baseDir);

// 供诊断/测试用：返回崩溃目录（<baseDir>\logs\crash）。
std::wstring CrashDir(const std::wstring& baseDir);

}  // namespace sysrecover
