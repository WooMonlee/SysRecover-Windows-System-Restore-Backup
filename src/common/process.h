#pragma once
// 进程执行器：CreateProcessW 直调（无 PowerShell 解析问题），
// 输出按 OEM 代码页转 UTF-8（bcdedit 中文输出不乱码，见 PIT-005）。
#include <string>

namespace sysrecover {

// 运行 exe + 参数，捕获 stdout+stderr 合并为 UTF-8。
// 返回进程退出码；启动失败返回 -1。
int RunProcess(const std::wstring& exe, const std::wstring& args,
               std::string& outUtf8);

// 取系统工具全路径（GetSystemDirectoryW 拼接）。
// 原因：CreateProcessW 传裸文件名（如 L"bcdedit.exe"）会 FILE_NOT_FOUND，
// 必须给全路径（PIT-010）。
std::wstring SysToolPath(const wchar_t* exeName);

}  // namespace sysrecover
