#pragma once
// 位数自举：**32 位程序在 64 位 Windows 上自动切换成 <ExeDir>\x64\<同名程序> 再跑**。
//
// 为什么：Windows 侧程序的位数**跟随系统**（32 位系统跑 x86、64 位系统跑 x64），
// 主要是为了**备份压缩速度**（64 位对 LZMS/LZX 明显更快）；还原由 Linux 救援层执行，
// 与宿主位数无关，**救援层固定 x86_64**。
//
// 发布包布局（"外面一套 x86、里面一套 x64"）：
//   <root>\SysRecover.exe|SysRecoverUI.exe + libwim-15.dll + UCRT   ← x86 整套（入口）
//   <root>\x64\{同上}                                                ← x64 整套
//   由本函数完成"换成 x64"这一步；不需要独立启动器，也不需要 x86\ 子目录。
//
// 见 docs/08 §0 与 AGENTS.md §3。

namespace sysrecover {

// 满足「本进程是 32 位」+「系统是 64 位」+「存在 <ExeDir>\x64\<同名>」时：
// 启动它、等它结束、把退出码写入 *exitCode 并返回 true（调用方应立即返回）。
// 否则返回 false（继续走本进程）。转发失败一律返回 false（退回本进程，不弹框）。
bool ReexecX64IfNeeded(int* exitCode);

}  // namespace sysrecover
