#pragma once
// 进程执行器：CreateProcessW 直调（无 PowerShell 解析问题），
// 输出按 OEM 代码页转 UTF-8（bcdedit 中文输出不乱码，见 PIT-005）。
#include <cstdint>
#include <string>

namespace sysrecover {

// 运行 exe + 参数，捕获 stdout+stderr 合并为 UTF-8。
// 返回进程退出码；启动失败返回 -1。
int RunProcess(const std::wstring& exe, const std::wstring& args,
               std::string& outUtf8);

// 取系统工具全路径（GetSystemDirectoryW 拼接）。
// 原因：CreateProcessW 传裸文件名（如 L"bcdedit.exe"）会 FILE_NOT_FOUND，
// 必须给全路径（PIT-010）；32 位进程在 64 位系统上还要走 Sysnative（PIT-082）。
std::wstring SysToolPath(const wchar_t* exeName);

// 本进程从**创建**到现在的毫秒数（GetProcessTimes 的创建时间 vs 当前时间）。
// 用途：启动耗时自检（GUI 在"窗口就绪"、CLI 在"命令分发"处各记一行），便于量化"启动慢"。
uint64_t MsSinceProcessStart();

// 启动耗时自检是否开启：**环境变量 `SYSRECOVER_STARTUP_TIMING=1`** 时为真。
// 排障用，默认关（用户 2026-09-25 要求"暂时关闭，以后合适时再打开"；
// 改自 DreamGrain 的 `DREAMGRAIN_STARTUP_TIMING` —— 用环境变量而非编译期宏，
// 不重编就能开）。
bool StartupTimingEnabled();

}  // namespace sysrecover
