#pragma once
// UEFI「固件启动项」引导层（成熟法，同 Rufus / efibootmgr / bcfg）：
//   往 NVRAM 写 Boot####（FilePathList 直指内核 EFI stub；OptionalData = 内核
//   命令行，UTF-16LE）并挂进 BootOrder，由**固件**直接加载内核 —— 全程不经
//   bootmgr，因此没有 bootapp 的 0xc000007b 问题。
// 条目按固定英文/中文描述识别；可持久（出现在固件启动菜单，Windows 坏了也能进）
// 也可 BootNext 单次启动。删除即抹掉 Boot#### + BootOrder 里的一项。
#include <string>

namespace sysrecover {

// 部署 UEFI 救援文件到 ESP 并创建固件启动项（幂等：已存在则更新）。
// espRoot 形如 L"S:\\"（mountvol X: /s 挂上的 ESP 根），exeDir 提供 bootfiles。
bool InstallUefiBootEntry(const std::wstring& espRoot,
                          const std::wstring& exeDir, std::string& log);

// 是否已安装（按描述匹配 Boot####）。detail 给出解释，供 GUI/CLI 显示。
bool UefiBootEntryExists(std::string& detail);

// 删除固件启动项（Boot#### + BootOrder 项 + 指向我们的 BootNext）。
bool RemoveUefiBootEntry(std::string& log);

// 单次启动：BootNext = 我们的条目号（下一次开机进救援，之后自动回到 Windows）。
bool SetUefiBootNext(std::string& log);

// 固件 Secure Boot 是否开启（读 `SecureBoot` 固件变量）。
// 开着时：固件只加载微软签名的镜像 → 我们的内核（未签名）会被拒，
// 必须走 shim（微软签名）+ MOK 那条链（见 PIT-062）。
bool IsSecureBootEnabled();

// P6：读固件 db，看它信任哪张微软 UEFI CA（只做字符串扫描，证书 CN 在 DER 里是明文）。
// 返回位掩码：kFirmwareCa2011 / kFirmwareCa2023；0 = 读不到（非 UEFI / 无权限）。
// 用途：我们当前的 Secure Boot 链是 CA2011 签名 —— 若固件只信任 CA2023，
// 救援环境可能起不来，应当**提前**告诉用户（见 PLAN.md §11）。
const int kFirmwareCa2011 = 1;
const int kFirmwareCa2023 = 2;
int FirmwareTrustedUefiCas();

// 我们的 MOK 公钥是否已注册进固件（即用户是否做过那次一次性注册）。
// exeDir 提供 bootfiles\sb\zj-mok.cer 用来在 MokListRT 里比对。
// 没注册时 shim 会弹 MokManager，用户必须先重启一次并注册，救援才跑得起来。
bool IsMokEnrolled(const std::wstring& exeDir);

// ── Secure Boot 下的引导走法（PIT-063/PIT-066 对比）──────────────
//   Grub    ：固件启动项 → shimx64.efi（微软签名）→ grubx64.efi(= **Canonical
//             签名的 Ubuntu GRUB**，shim 内嵌的正是同一把证书，**无需 MOK 注册**)
//             → grub.cfg 用普通 `linux`/`initrd` 命令加载我们的内核 + initramfs。
//             ⭐ 如果 GRUB 的普通命令不校验内核签名，这就是**零注册零成本**的正解。
//   Shim    ：固件启动项 → shimx64.efi → grubx64.efi(= **我们签名的 UKI**)。
//             标准机制，但首次要在 MokManager 注册我们的公钥。
//   Bootapp ：BCD bootapp → efiloader.efi → UKI。**实测在 SB 开启时走不通**
//             （`nointegritychecks` 被策略保护，PIT-065）。
// 环境变量 `ZJ_SB_MODE` = `shim`（默认）| `grub` | `bootapp`。
enum class SbMode { None, Shim, Bootapp, Grub };
SbMode CurrentSbMode();
const char* SbModeName(SbMode m);

}  // namespace sysrecover
