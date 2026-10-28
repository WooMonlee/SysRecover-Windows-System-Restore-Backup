#pragma once
// 自足引导（2026-10-27，用户反馈 2）。
//
// 场景：极简 WinPE（如 Win8PE）里**没有 bcdboot.exe / bcdedit.exe / bootsect.exe**
// ——以前分区、还原后修引导都会在这里中止（用户实测：分区其实已经做完并激活，
// 只是软件报"缺少文件"；装完系统又报"缺少文件"，只能用镜像里的 fixboot.exe 修）。
//
// 本模块用**随包资产 + Win32 文件操作**做完整套引导，不依赖任何系统工具：
//   bootfiles\ntfs-boot-code.bin  微软 NTFS PBR 代码段（426B，写分区 offset 0x54）
//   bootfiles\ntfs-boot-cont.bin  PBR 后续扇区（4096B，写分区 offset 512）
//   bootfiles\mbr-boot-code.bin   磁盘 MBR 引导代码（440B，见 disk/mbrcode.h）
//   bootfiles\bcd\bios.bcd        BIOS 用 BCD 模板（{bootmgr}/{default} device boot，
//                                 完全静态，无需任何 GUID 替换）
//   bootfiles\bcd\uefi.bcd        UEFI 用 BCD 模板（两处 16B 分区 GUID 占位，运行时
//                                 用真实 ESP / 系统分区 GUID 替换）
// 两个模板 2026-10-27 均在 QEMU 实测：SeaBIOS → Windows 登录界面；
// OVMF → Windows 欢迎界面（引导链本身与工具无关）。
//
// 纪律：本文件里的 log/err **只用 ASCII 技术描述**（进日志）；面向用户的中文提示
// 由调用方用 Tr() 组装（避免 i18n 词条散落在底层模块里）。
#include <string>
#include <vector>

namespace sysrecover {

// ── NTFS 分区引导区（PBR）────────────────────────────────────────
// 分区首扇区 OEM 名是否 "NTFS    "（写之前必须验，否则会把 NTFS 引导代码写到
// FAT/其它分区上）。detail 进日志。
bool IsNtfsVolume(const std::wstring& volRoot, std::string& detail);

// 确保分区引导区是**微软** NTFS 引导代码：0x54 处 426B + 扇区 1..8（4096B）。
// 幂等，format.com / mkntfs 之后每次都要跑（它们会写自己的占位代码）。
// 实现要点（2026-10-27/28 实测）：
//   · 卷句柄裸读写必须 OVERLAPPED + 扇区对齐；
//   · **安全软件会拦卷句柄写引导扇区**（火绒「写磁盘保留扇区」→
//     ERROR_ACCESS_DENIED）→ 自动改走 `\\.\PhysicalDriveN` 直写兜底；
//   · 0x54 已是 `fa 33 c0` 就不写第一扇区（不必惊动安全软件）。
// 返回 false = 没写成（log 里是原因）；非 NTFS 分区会跳过并返回 true。
bool EnsureNtfsBootRegion(const std::wstring& volRoot,
                          const std::wstring& exeDir, std::string& log);

// ── BCD 文件（纯逻辑，便于单测）──────────────────────────────────
// BCD 是注册表 hive；用「UTF-16LE 里出现 winload」判"有条目"（不依赖 bcdedit.exe，
// 极简 PE 里没有它）。detail 进日志。
bool BcdFileLooksUsable(const std::wstring& bcdPath, std::string& detail);

// 把 BCD 缓冲里的 16 字节 from 全部替换成 to，返回替换次数。
// UEFI 模板的两处分区 GUID 占位（ESP 5 次 / 系统卷 10 次）就靠它。
int PatchBcdBytes(std::vector<unsigned char>& buf, const unsigned char from[16],
                  const unsigned char to[16]);

// 读卷所在分区的 GPT 分区 GUID（**原始 16 字节内存布局**，与 BCD 设备元素里的
// 表示一致：Data1/2/3 小端 + Data4）。volRoot 形如 L"S:\\"。
bool GetVolumeGuidRaw(const std::wstring& volRoot, unsigned char out[16],
                      std::string& log);

// 只生成引导文件树（\bootmgr + \Boot\*），**不写分区引导区**：
// 用途 ① 就地还原直接写目标盘根（EnsureBiosBootFiles 内部调用）；
//      ② 暂存阶段写到 <recoveryDir>\bootfix\（Linux 侧再拷进格式化后的目标盘）。
bool StageBiosBootTree(const std::wstring& destRoot, const std::wstring& winDir,
                       const std::wstring& exeDir, bool overwriteBcd,
                       std::string& log, std::string& err);

// ── BIOS/MBR 自足引导 ───────────────────────────────────────────
// 确保 targetRoot 能引导 Windows（BIOS）：\bootmgr + \Boot\BCD + 字体/资源/语言
// （尽力，缺则 WARN）+ NTFS 分区引导区。
//   targetRoot 形如 L"C:\\"；winDir = L"C:\\Windows"（离线/在线均可）。
//   overwriteBcd=true → 无条件写随包模板（就地还原刚格过盘，镜像里的 BCD 是别的
//     机器的，不能信）；false → 已有且看起来可用就**保留**（保住第三方启动项，
//     PIT-119）。
// 返回 false = 关键项失败（err 给原因），调用方决定是报错还是降级。
bool EnsureBiosBootFiles(const std::wstring& targetRoot,
                         const std::wstring& winDir, const std::wstring& exeDir,
                         bool overwriteBcd, std::string& log, std::string& err);

// ── UEFI/GPT 自足引导 ──────────────────────────────────────────
// 确保 ESP 能引导 Windows（UEFI）：
//   \EFI\Microsoft\Boot\{bootmgfw.efi, BCD, bootmgr.efi, Fonts, Resources, <lang>}
//   \EFI\BOOT\BOOTX64.EFI（固件兜底路径）
//   BCD 模板里的两个 GUID 占位 → ESP / 目标系统分区的真实 GUID。
//   overwriteBcd 语义同上（ESP 不被我们格式化，默认应保留有效 BCD）。
// 不写固件启动项（那是 uefi::EnsureWindowsBootEntry 的事，失败不算致命）。
bool EnsureUefiBootFiles(const std::wstring& espRoot,
                         const std::wstring& targetRoot,
                         const std::wstring& winDir, const std::wstring& exeDir,
                         bool overwriteBcd, std::string& log, std::string& err);

}  // namespace sysrecover
