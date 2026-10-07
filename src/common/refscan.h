#pragma once
// 悬空引用扫描/清理（PIT-132，2026-10-07）
//
// 背景：备份捕获注册表，但排除"易失目录"（Temp 等）→ 注册表里指向这些目录的
// 引用在还原后悬空。实测客户案例（PIT-130/131 同期）：豆包便携版把右键扩展
// DLL 注册在 AppData\Local\Temp → 我们的还原后 DLL 缺失、注册仍在 → 每次
// 右键卡死（等待链：UI 线程等 shell 工作线程；diag 标 *** MISSING ***）。
//
// 方案：
//   备份时 ScanDanglingRefs() 统计（>0 → 主镜像并入 ZJEA 修复包，携带补写器）；
//   目标系统首启（登录前、SYSTEM）由补写器 CleanAllDanglingRefs() **保守清理**
//   —— 只删"路径位于易失目录 **且** 文件已缺失"的项，绝不误删其它。
//
// 覆盖：shell 扩展（右键处理器 / 覆盖图标 / ShellExecuteHooks，HKLM 双视图）
//       + 启动项（Run / RunOnce，HKLM 双视图 + 各用户 hive）。
// 只依赖 Win32（advapi32）；可被 x86 补写器与 x64 主程序共用。
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace sysrecover {
namespace refscan {

struct DanglingRef {
    std::wstring display;    // 供日志/备份：如 HKLM\...\Run!xxx -> C:\...\Temp\a.exe
    HKEY hroot = nullptr;    // 删除用根（HKLM/HKEY_USERS）
    REGSAM view = 0;         // 打开根时附加视图（KEY_WOW64_*；0=进程默认）
    std::wstring subKey;     // 相对 hroot 的键路径（子键形态时=被删子键全路径）
    std::wstring valueName;  // 值形态：值名；空 = 子键形态（删整棵子键）
    std::wstring filePath;   // 解析出的目标文件（已展开环境变量、去引号）
    // 文件当前状态：true=已缺失（首启清理对象）；false=存在但在易失目录
    // （备份视角：还原后必然缺失 → 也要打进修复包）。
    bool missingNow = false;
};

struct CleanStats {
    int found = 0;    // 见到的"易失目录引用"总数（含两种状态）
    int cleaned = 0;  // 已删除（仅 missingNow 的）
    int failed = 0;   // 删除失败
    int atRisk = 0;   // 文件仍存在（未动；备份视角的"将损坏"项）
};

// ── 纯逻辑（单测）───────────────────────────────────────────────────────
// 路径是否位于"易失目录"（备份必然排除、还原后必然消失）。
// 判据为路径片段（大小写不敏感、容忍 / 与反斜杠、容忍 \??\ 前缀）：
// AppData\Local\Temp、Windows\Temp、Windows\CbsTemp、
// Windows\winsxs\InstallTemp、AppData\Local\Microsoft\Windows\INetCache。
bool IsVolatilePath(const std::wstring& path);

// 从命令行取可执行文件路径：先剥引号；无引号时截到首个 ".exe"（容忍路径
// 中的空格），没有 ".exe" 则截到首个空格。不做环境变量展开（调用方做）。
std::wstring ExtractExecutable(const std::wstring& command);

// ── 扫描 ────────────────────────────────────────────────────────────────
// 扫描本机悬空引用（机器级双视图；includeLoadedUserHives=true 时也扫
// HKEY_USERS 下**已加载**的用户 hive —— 备份/诊断用；首启清理见下）。
std::vector<DanglingRef> ScanDanglingRefs(bool includeLoadedUserHives);

// 删除一条引用（值形态删值；子键形态删整棵子键）。返回是否成功。
bool RemoveRef(const DanglingRef& r, std::wstring& err);

// 首启清理：机器级 + 磁盘上各用户 hive（加载→扫描→删除→卸载）。
// usersDir 为空时用 <SystemDrive>\Users。log 输出过程行（可空）；
// backupOut 累积被删条目文本（供恢复，可空）。绝不抛异常。
CleanStats CleanAllDanglingRefs(const std::function<void(const std::string&)>& log,
                                std::string& backupOut,
                                const std::wstring& usersDir = std::wstring());

}  // namespace refscan
}  // namespace sysrecover
