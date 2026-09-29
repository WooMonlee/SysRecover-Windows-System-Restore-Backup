#include "cpucap.h"

#include <windows.h>

#include <cstdio>
#include <mutex>

#include "i18n.h"

namespace sysrecover {
namespace {

// UI 线程可在备份进行中改上限，worker 的 guard 也会在起停时改 → 串行化。
std::mutex g_mu;
HANDLE g_job = nullptr;  // 进程首个上限生效时创建；进程退出由系统回收
int g_pct = 0;           // 当前值（0 = 不限制）

// CpuRate 单位 = 1/100 的百分比（10000 = 100%）。>10000 会被驱动拒掉（ERROR_INVALID_PARAMETER=87）。
bool SetRate(DWORD rate, std::string* why) {
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION ci;
    ZeroMemory(&ci, sizeof(ci));
    ci.ControlFlags =
        JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
    ci.CpuRate = rate;
    if (SetInformationJobObject(g_job, JobObjectCpuRateControlInformation, &ci,
                                sizeof(ci)))
        return true;
    if (why) {
        char buf[160];
        snprintf(buf, sizeof(buf), Tr("无法设置 CPU 限制（err=%lu）"),
                 static_cast<unsigned long>(GetLastError()));
        *why = buf;
    }
    return false;
}

}  // namespace

bool SetCpuCap(int pct, std::string* why) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == g_pct)
        return true;
    if (pct == 0) {
        // “不限制”= 把上限拉满。Job 建了以后**不能退出**（Win7 更是不支持嵌套
        // Job），所以只能用速率表达“不限”，等价于不设限。
        if (g_job && !SetRate(10000, why))
            return false;
        g_pct = 0;
        return true;
    }
    if (!g_job) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (!g_job) {
            if (why) {
                char buf[160];
                snprintf(buf, sizeof(buf), Tr("无法创建 CPU 限制任务对象（err=%lu）"),
                         static_cast<unsigned long>(GetLastError()));
                *why = buf;
            }
            return false;
        }
        if (!SetRate(static_cast<DWORD>(pct) * 100, why)) {
            CloseHandle(g_job);
            g_job = nullptr;
            return false;
        }
        if (!AssignProcessToJobObject(g_job, GetCurrentProcess())) {
            // 常见于本进程已被别的 Job 收编（Win7 不支持嵌套 Job）。
            if (why) {
                char buf[192];
                snprintf(buf, sizeof(buf),
                         Tr("无法限制 CPU：程序已在其它任务对象中（err=%lu）"),
                         static_cast<unsigned long>(GetLastError()));
                *why = buf;
            }
            CloseHandle(g_job);
            g_job = nullptr;
            return false;
        }
        g_pct = pct;
        return true;
    }
    if (!SetRate(static_cast<DWORD>(pct) * 100, why))
        return false;
    g_pct = pct;
    return true;
}

int GetCpuCap() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_pct;
}

}  // namespace sysrecover
