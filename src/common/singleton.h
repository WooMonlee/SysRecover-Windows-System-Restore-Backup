#pragma once
// 单实例互斥：备份/还原等写操作同时只允许一个实例。
// 读操作（list/verify/images/version/diag）不受限。
namespace sysrecover {

// 尝试持有全局锁。成功返回 true。
// CLI：持有到进程退出即可；GUI：操作结束应调 ReleaseOpLock。
bool AcquireOpLock();

// 释放全局锁（未持有时为空操作）。
void ReleaseOpLock();

// 锁名（CLI/GUI 共用，见 AGENTS.md §9）。
inline const wchar_t* OpLockName() {
    return L"Global\\SysRecover_SingleInstance";
}

}  // namespace sysrecover
