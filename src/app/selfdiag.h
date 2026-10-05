#pragma once
// 自诊断与痕迹收集（用户 2026-10-03 规格）—— 用户排错只需发 **logs 文件夹**：
//   · 启动即把 diag.txt（固件/Secure Boot/启动项/工具/SMART）与 list.txt（磁盘表）
//     写进 `<日志根>\logs\`，用户不用再敲 `diag` / `list`；
//   · 把"我们留在磁盘上的部署文件"收进 `logs\collected\<盘>\`（盘根的 grldr/
//     grldr.mbr/menu.lst/_zjresy*.log/zjrestore-boot.log/restore-task.conf +
//     `ZJRESTORE\{bootfix,scripts,logs}` 子树 + 目录清单；内核/initramfs 这类大件
//     只进清单不搬运）。
#include <string>

namespace sysrecover {

// diag 文本（与 `diag` 命令同源）。返回 wimlib 初始化结果（0=OK）。
int DiagReportText(std::string& out);

// 磁盘/分区表文本（UTF-8，CRLF；与 `list` 同源）。
void DiskListText(std::string& out);

// 写 diag.txt + list.txt 到 logsDir（失败静默，仅记日志）。
void WriteDiagFiles(const std::wstring& logsDir);

// 把部署痕迹收集到 dstDir。onlyDrive=0 → 扫描所有固定盘（启动时）；
// 否则只收该盘（暂存完成后立即收一次）。返回收集到的文件数。
// 另收：盘根黑匣子三件套（ZJRESTORE-last/probe/status）、ZJRESTORE-logs 兜底日志窝、
// drive-map.txt（盘符↔物理身份对照）、ESP 的 \EFI\ZJRESTORE\logs（用户 2026-10-04 规格）。
int CollectDeployArtifacts(const std::wstring& dstDir, wchar_t onlyDrive);

// 救援层失败回执（黑匣子）：在 logs\collected 里找 ZJRESTORE-status.txt
// （result=FAILED），返回 3 行用户提示（失败步骤/时间/版本/日志位置）。
// 同一回执只报一次（logs\.last-rescue-status 去重）。无失败或已报过返回空串。
std::wstring CheckLastRescueFailure();

// 支持包出包成功后清理散落在各盘的日志（用户规格 4：不该到处放）：
// <盘根>\ZJRESTORE-{last.log,probe.txt,status.txt}、zjrestore-boot.log、
// ZJRESTORE-logs\、非活动 <盘>\ZJRESTORE\logs\、ESP 的 \EFI\ZJRESTORE\logs\*。
// 契约（restore-task.* / _zjresy*）与引导文件不动。返回删除条目数。
int CleanupStrayLogs();

// 扫描带 NTFS 扩展属性（EA）的文件（用户 2026-10-05：客户 Linux 侧还原的
// apply.out 报 `Ignoring extended attributes of 804 files` —— EA 只在 Linux
// 还原时丢失，Windows 侧（PE 就地）保留）。列出 路径 + EA 名，供排查/取证；
// 结果同时写进 outPath（UTF-8）。返回带 EA 的文件数。
int ScanEaFiles(const std::wstring& root, const std::wstring& outPath,
                std::string& err);

// ── 「日志」按钮 / `support` 命令：支持包 ─────────────────────────────────
struct SupportBundle {
    std::wstring zipPath;  // 成功 = 包路径；失败 = 空
    std::wstring error;    // 失败原因（宽串，可直接展示）
    int logFiles = 0;      // 收进包的日志文件数（含各盘 ZJRESTORE\logs）
    int dumps = 0;         // explorer 转储个数
    int eventFiles = 0;    // 事件日志导出个数
};

// 收集：①当前日志目录 logs\（递归，含 collected）②各固定盘 <X>:\ZJRESTORE\logs
// ③explorer 进程转储（MiniDumpNormal|WithThreadInfo）④事件日志文本（Application
// Hang/Error、System 存储/错误级）⑤diag.txt / list.txt / version.json /
// bundle-info.txt → 一个 zip。
// outZip 为空 → 桌面 SysRecover-logs-<时间>.zip（拿不到桌面则回退日志目录）。
SupportBundle BuildSupportBundle(const std::wstring& outZip);

}  // namespace sysrecover
