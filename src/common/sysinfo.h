#pragma once
// 运行中系统的可读描述 —— 备份时用来生成「镜像信息」与默认文件名。
// 用户规格（2026-09-21）：
//   镜像信息（显示在文件名下方）：`20260921 Windows 10 IoT 企业版 LTSC 21H2 19044.4046 备份`
//   默认文件名：`20260921Win10.19044备份.esd`
// 数据来源：HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion 的注册表值。
#include <string>

namespace sysrecover {

struct SystemDescription {
    std::wstring full;      // "Windows 10 IoT 企业版 LTSC 21H2 19044.4046"（取不到则空）
    std::wstring shortTag;  // "Win10.19044"（文件名用；取不到则空）
};

// 注册表原始值（DescribeRunningSystem 读出来；单独抽出来是为了**可单元测试**——
// 组合逻辑不碰 Windows API）。字段对应
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion 下的同名值。
struct RawSysInfo {
    std::wstring productName;         // ProductName
    std::wstring editionId;           // EditionID
    std::wstring displayVersion;      // DisplayVersion（Win10 2004+："21H2"）
    std::wstring releaseId;           // ReleaseId（旧版："1909"）
    std::wstring csdVersion;          // CSDVersion（Win7："Service Pack 1"）
    std::wstring currentBuildNumber;  // CurrentBuildNumber
    std::wstring currentBuild;        // CurrentBuild（兜底）
    bool hasUbr = false;              // UBR 是否存在
    unsigned long ubr = 0;            // UBR（修订号）
};

// **纯函数**：把原始值组合成描述（不读注册表、不抛异常、不弹框）。
SystemDescription ComposeSystemDescription(const RawSysInfo& raw);

// 组合上面两项。**不抛异常、不弹框**：任何一项读不到就留空，由调用方回退。
SystemDescription DescribeRunningSystem();

}  // namespace sysrecover
