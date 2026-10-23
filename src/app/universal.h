#pragma once
// 异机还原（2026-10-21，0.9）：还原到不同硬件/控制器。
// 范围：仅 Win10/11（Win7/8.1 明确拒绝并提示）；只处理“启动关键驱动”：
//   · 最小化（零体积）：离线启用 Windows 自带的启动存储驱动（storahci/stornvme/
//     pciide/atapi/amdsata/amdxata/usbstor，Start=0）；
//   · 可选：用户提供驱动文件夹，DISM 离线 Add-Driver（不随包内置厂商驱动）。
// 只适用于 Windows 侧路径（A 引擎解压到 ~new / PE 就地还原）；B 引擎（Linux
// apply）没有 DISM，调用方在暂存阶段拒绝。
#include <string>

namespace sysrecover {

// 纯逻辑：镜像 OS 版本是否支持异机（Win10/11 = major 10）。
bool IsUniversalOsSupported(int major, int minor);

// 从镜像抽取 ntoskrnl.exe 读版本与位数（只读镜像；不写目标）。
// 返回 0 成功；非 0 失败（err=UTF-16）。
int ProbeImageOs(const std::wstring& image, int index, int* major, int* minor,
                 bool* is64, std::wstring& err);

// 驱动文件夹校验：存在、递归至少一个 .inf。infCount 返回数量。
bool ValidateDriverFolder(const std::wstring& dir, int* infCount,
                          std::wstring& err);

// 统一入口：启用 in-box 驱动 +（可选）DISM 注入驱动包。
// offlineRoot = 离线 Windows 根目录（A 引擎 ~new / 就地还原的 X:\）。
bool PrepareUniversal(const std::wstring& offlineRoot,
                      const std::wstring& driverDir, std::string& log,
                      std::wstring& err);

// 当前宿主是否为 64 位（PE/系统）；x64 镜像需要 64 位环境才能用 DISM。
bool IsHost64();

}  // namespace sysrecover
