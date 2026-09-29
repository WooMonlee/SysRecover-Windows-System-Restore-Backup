#pragma once
// 备份时限制**本进程**的 CPU 占用率（Windows Job 对象 CPU_RATE_CONTROL_HARD_CAP）。
// wimlib 的压缩是多线程的，默认会吃满所有核；给用户一个“别把机器卡死”的开关。
#include <string>

namespace sysrecover {

// pct：1..100 = 硬上限（占**整机** CPU 的百分比，实测误差 ±3~5%）；0 = 不限制。
// 首次设 >0 时把本进程纳入自己创建的 Job（一次性；之后只改速率，可随时改）。
// 返回 false = 设置失败（本进程已在别的 Job 里 / API 不可用），*why 为中文原因。
// 失败**非致命**：调用方记日志继续即可，只是没限速。
bool SetCpuCap(int pct, std::string* why);

// 当前生效值（0 = 未限制 / 未建 Job）。
int GetCpuCap();

}  // namespace sysrecover
