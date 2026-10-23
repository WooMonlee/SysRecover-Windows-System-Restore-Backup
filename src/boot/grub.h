#pragma once
// GRUB4DOS 引导层部署：D:\grldr.mbr + D:\grldr + D:\menu.lst（根目录，隐藏）
// + D:\ZJRESTORE\{boot,scripts,logs}\*（PIT-058）。
#include <string>

namespace sysrecover {

constexpr wchar_t kRecoveryDir[] = L"ZJRESTORE";

// 定位数据盘根目录（如 L"D:\\"），找不到返回空串。优先 D:，回退首个非系统固定盘。
// avoidLetter（2026-10-18 Q1）：非 0 时跳过该盘符（还原时排除目标盘，防契约写进会被格式化的盘）。
std::wstring FindDataDrive(wchar_t avoidLetter = 0);

// 系统盘根目录（Windows 所在盘，如 L"C:\\"）。
std::wstring SystemDrive();

// 当前 exe 所在目录。
std::wstring ExeDir();

// 复制 exe 自带 bootfiles/grldr[.mbr] 到指定盘根目录。返回成功数（0-2）。
int DeployGrldr(const std::wstring& deployDrive, const std::wstring& exeDir,
                std::string& log);

// 生成 <deployDrive>\menu.lst（kernel + initrd，PIT-006）。
bool WriteMenuLst(const std::wstring& deployDrive, std::string& log);

// 复制 vmlinuz/initramfs/restore.sh 到 <deployDrive>\ZJRESTORE\boot|scripts。
bool CopyBootFiles(const std::wstring& deployDrive, const std::wstring& exeDir,
                   std::string& log);

// NeedsInstall 四项检查：目录 + grldr.mbr + menu.lst + BCD 条目。
bool NeedsInstall(std::string& detail);

// 清除引导层（「清除引导项」按钮用）：删 BCD 恢复条目 + 清除 {bootmgr}
// bootsequence + 删除系统盘/数据盘上的 grldr/grldr.mbr/menu.lst 与 ZJRESTORE\。
bool RemoveBootLayer(std::string& log);

// 一键安装引导层到 deployDrive（含 BCD 条目创建）。返回 true 表示全部就绪。
// temporary=true：日常还原的一次性条目（独立 GUID/名称，首启自动清理）；
// temporary=false：用户显式「安装菜单」的常驻条目。
bool InstallBootLayer(const std::wstring& deployDrive,
                      const std::wstring& exeDir, std::string& log,
                      bool temporary = false);

// ── UEFI（GPT）分支 ──────────────────────────────────────────────
// 临时给 ESP 分配一个盘符（mountvol X: /s，需管理员）。成功返回 L"X:\\"。
std::wstring MountEsp(std::string& log);
void UnmountEsp(const std::wstring& espRoot, std::string& log);

}  // namespace sysrecover
