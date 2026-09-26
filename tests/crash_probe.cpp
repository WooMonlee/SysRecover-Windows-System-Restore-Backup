// 崩溃处理回归探针：安装 CrashHandler → **主动触发访问违例** → 进程崩溃。
// 期望在 <baseDir>\logs\crash\ 下生成 crash-*.dmp + crash-*.txt（+ last.txt）。
// 用法：crash_probe.exe <baseDir>   （路径建用 ASCII，省得转码）
#include <windows.h>

#include <cstdio>

#include "common/crash.h"

int main(int argc, char** argv) {
    const char* base = (argc > 1 && argv[1]) ? argv[1] : "tests\\crash-out";
    wchar_t wb[512] = {};
    MultiByteToWideChar(CP_ACP, 0, base, -1, wb, 512);
    sysrecover::InstallCrashHandler(wb);

    volatile int* p = nullptr;
    *p = 1;  // 触发 EXCEPTION_ACCESS_VIOLATION
    std::printf("should not reach here\n");
    return 0;
}
