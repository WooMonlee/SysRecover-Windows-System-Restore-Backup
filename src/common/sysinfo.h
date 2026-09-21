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

// 组合上面两项。**不抛异常、不弹框**：任何一项读不到就留空，由调用方回退。
SystemDescription DescribeRunningSystem();

}  // namespace sysrecover
