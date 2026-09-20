// 单实例互斥实现。
#include "singleton.h"

#include <windows.h>

namespace sysrecover {
namespace {
HANDLE g_held = nullptr;
}

bool AcquireOpLock() {
    if (g_held)
        return true;
    HANDLE m = CreateMutexW(nullptr, TRUE, OpLockName());
    if (!m)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return false;
    }
    g_held = m;
    return true;
}

void ReleaseOpLock() {
    if (!g_held)
        return;
    ReleaseMutex(g_held);
    CloseHandle(g_held);
    g_held = nullptr;
}

}  // namespace sysrecover
