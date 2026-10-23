#pragma once
// 「为新硬盘创建分区」向导（2026-10-20，用户规格）。
// 纯 Win32 模态对话框（与删除进度窗同风格；不依赖 Duilib 皮肤），返回用户
// 选定的方案与系统盘容量；实际分区由 disk/partinit.cpp 执行。
#include <windows.h>
#include <string>

#include "disk/partinit.h"

namespace sysrecover {

struct InitDiskRequest {
    EmptyDiskInfo disk;
    bool uefiSession = false;   // 当前 PE 是否以 UEFI 方式启动
    bool win7Family = false;    // 镜像家族（影响默认容量与 GPT 可用性）
    bool mbrAllowed = true;
    bool gptAllowed = true;
    DiskScheme preferred = DiskScheme::Gpt;
    unsigned long long defaultSystemGb = 100;
    unsigned long long minSystemGb = 80;
};

struct InitDiskResult {
    bool ok = false;
    DiskScheme scheme = DiskScheme::Gpt;
    unsigned long long systemGb = 100;
};

InitDiskResult ShowInitDiskDialog(HWND owner, const InitDiskRequest& req);

}  // namespace sysrecover
