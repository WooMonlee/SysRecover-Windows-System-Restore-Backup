#pragma once
// 运行目录搬迁（用户 2026-10-03 规格）—— 把日志统一到「程序目录\logs」一个位置：
//   · 程序在**光盘/写保护介质**上：无法写日志 → 复制整包到本地固定分区再运行；
//   · 程序在**U盘**上（即便可写）：GUI 询问、CLI 自动复制（GUI 保留"留在原处"选项）；
//   · 目的地 = **装了 Windows 的分区之外的第一顺序可写固定分区**（系统在 C: 就选 D:，
//     在 D: 就选 C:；含本机离线 Windows 分区，PE 下同样避开未来还原目标）；
//     没有别的可写分区时退回系统分区本身（GUI 文案会说明日志保不住）。
// 复制跳过 `logs\`（目的地已有日志只增不减）；新副本由原进程启动后原进程退出
// （GUI 不等待、互斥由副本接手；CLI 等待并透传退出码）。
#include <string>
#include <vector>

namespace sysrecover {

struct RelocatePlan {
    bool needed = false;          // 是否应搬迁（CD / U盘 / 目录不可写）
    std::wstring why;             // "cdrom" / "removable" / "readonly"
    std::wstring destDrive;       // 目标盘根（"D:\\"）；空 = 没有可用目的地
    std::wstring destRoot;        // 目标目录（"D:\\ZJRESTORE"）；空 = 没有可用目的地
    bool onlySystemDrive = false; // 目的地是系统分区（单分区机器；还原系统盘时保不住）
    unsigned long long needBytes = 0;
};

// 纯逻辑（单测）：候选取"系统分区之外的第一顺序"；都在系统分区上则用系统分区。
// 大小写不敏感；返回 0 = 无可用。
wchar_t SelectRelocateDrive(const std::vector<wchar_t>& candidates,
                            wchar_t systemLetter);

// 检测（只读探测，不写盘）：是否需要搬迁、有没有可搬的目的地。
RelocatePlan CheckRelocate();

// 复制整包（跳过 logs\）并启动新副本。
//   wait=true（CLI）：等子进程结束，退出码写入 *exitCode；
//   wait=false（GUI）：立刻返回，调用方应随后退出（单实例互斥由子进程接手）。
// 成功返回 true（调用方应立即退出）。err = 可展示给用户的失败原因（UTF-8）。
bool DoRelocate(const RelocatePlan& plan, bool wait, int* exitCode,
                std::string* err);

// 整包大小（递归，跳过 logs\）—— 用于目标盘剩余空间检查。
unsigned long long AppTreeSize();

// 日志/数据根 —— 用户 2026-10-17 规格（四条规则，逐条尝试，前者可用即止）：
//   ① 已安装的程序目录 `<系统盘>\Program Files\SysRecover`（可写就用它；
//      与「克隆后首启把救援层日志搬回程序目录」是同一个位置）；
//   ② 程序所在文件夹（U盘/绿色目录；PE 的 X:\ 是 RAM 盘 → 跳过，见下）；
//   ③ 只读介质（光盘/写保护）→ 除系统盘外**第一个可写固定分区**的 `\ZJRESTORE`
//      （优先不含 Windows 的盘；该目录设**隐藏**属性）；
//   ④ 一个都没有 → 退回程序目录并置 LogHomeDegraded()（GUI/CLI 提示日志保不住）。
// PE 例外：PE 下 `GetWindowsDirectoryW()` 在 RAM（X:\Windows），而"已安装的程序
// 目录"可能是**离线系统盘**（还原目标）→ 规则 ① 与"X:\ 上的规则 ②"都跳过，
// 日志一律落数据盘（PIT-105/107 的 PE 行为不变）。
// 返回的目录已尽力建好；调用方只需再建 `\logs`。
std::wstring LogBaseDir();

// 纯逻辑（单测）：是否该用「已安装的程序目录」当日志根（规则 ①）。
// installDir = `<ProgramFiles>\SysRecover`，winDir = GetWindowsDirectoryW()。
// PE 里 winDir 在 RAM 而 Program Files 可能是离线系统盘 → 一律不用。
bool UseInstallDirLogHome(const std::wstring& installDir,
                          const std::wstring& winDir, bool isPe);

// 规则 ④：程序目录与所有固定分区都不可写（日志无处可放）。
bool LogHomeDegraded();

// 克隆后**首启**搬迁（用户 2026-10-17 规格）：把数据盘上的旧日志位置搬回日志根
// （「先搬日志，再询问是否删除旧系统」）。
//   源 = 各固定盘（不含系统盘）的 `<盘>:\ZJRESTORE\logs`（PIT-059 软件目录回退）
//        与 `<盘>:\ZJRESTORE-logs`（PIT-137 黑匣子兜底窝，文件直接摊在根下）；
//   目的 = LogBaseDir() + `\logs`，**仅当规则 ① 命中**（否则日志根就在数据盘上，
//        搬了等于自搬自）；
//   不覆盖同名文件（改名追加 ` (N)`）；跨卷/被占用走复制 + 登记「下次启动删除」；
//   搬空的源目录删掉，没搬空的（还有 bootfix 等）与兜底窝设**隐藏**。
// 返回一行摘要（调用方写日志，便于现场取证）；规则 ① 不命中时返回空串。
std::string MigrateLogsToLogHome();

}  // namespace sysrecover
