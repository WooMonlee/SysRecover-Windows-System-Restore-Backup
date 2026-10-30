SysRecover 异机还原 · 驱动包说明

把驱动放在本文件所在目录（Drivers\）或其子目录里，「异机还原」会自动注入
（仅支持 Windows 10/11）。只需 .inf / .sys / .cat，不要放 .exe 安装包。
程序内「导出本机硬盘驱动供异机还原」写的就是 `Drivers\boot-<时间>\`，
同样自动生效，不需要再手动指定目录。
- 位数：x64 镜像用 x64 驱动（32 位镜像用 x86）。
- 签名：x64 驱动必须有有效签名。
- 没有 .inf 时程序只启用内置启动驱动（零体积），不影响同机还原。

优先需要的“启动关键驱动”（按目标机情况选）：
- Intel RST / VMD（iaStorAC / iaStorVD）：BIOS 里 SATA 模式为 RAID/VMD 时
- AMD RAID（rcbottom / rcraid / rccfg）
- Broadcom/LSI MegaRAID（megasas / megaraid）
- Marvell / Adaptec 等 RAID/HBA
- 虚拟机：virtio-win（viostor / vioscsi）、VMware pvscsi
- 其他不在 Windows 内置列表里的 NVMe/SCSI 控制器

────────────────────────────────────────────────────────────
推荐来源（公开发布、可直接下载；省去自己收集驱动的工作量）
────────────────────────────────────────────────────────────
1) 目标机原来的 Windows（最准确）
   管理员命令提示符（Windows 10/11 自带，任选一条）：
     pnputil /export-driver * <U盘或数据盘>\Drivers
     dism /online /export-driver /destination:<U盘或数据盘>\Drivers
   适用于“旧系统还能启动、只是要换盘/换机”的场景。

2) Dell WinPE 11 驱动包（约 32MB，含 Intel RST、AMD RAID、LSI MegaRAID 等）
   https://www.dell.com/support/kbdoc/en-us/000211541/winpe-11-driver-pack
   （Win10 环境可用 000108642 的 WinPE 10 包，约 134MB）
   下载后是 CAB/EXE，用 7-Zip 解压，把解压出的目录整体放进 Drivers\ 即可。
   说明：`Dell-WinPE11` 是 Dell 官方包的来源目录名；WinPE 是它的用途/目标环境，
   不是“只能在 PE 里运行”。其中的 .inf / .sys / .cat 是标准 Windows 驱动，
   本软件只把它作为异机还原的启动关键驱动兜底；普通 Windows 装好后仍建议
   安装厂商对应机型的完整驱动。Win10 目标优先用目标机导出或 WinPE 10 包。

3) HP WinPE 驱动包
   https://ftp.ext.hp.com/pub/caps-softpaq/cmit/HP_WinPE_DriverPack.html

4) Lenovo WinPE 驱动包
   https://support.lenovo.com/us/en/documents/ht074984

5) 虚拟机 / 开源：virtio-win（BSD-3-Clause，Red Hat 签名，可自由再分发）
   https://github.com/virtio-win/kvm-guest-drivers-windows
   下载发行版 ISO/驱动目录，取 viostor、vioscsi（必要时 netkvm）对应
   Win10/11 的 x64 目录，放进 Drivers\ 即可。

6) 其它厂商/型号：Snappy Driver Installer Origin（开源、离线驱动包管理工具，
   可用来查缺补漏；驱动包本身的授权跟随各厂商）
   https://www.snappy-driver-installer.org/

7) 单个设备的精确驱动：Microsoft Update Catalog（按硬件 ID 搜索）
   https://www.catalog.update.microsoft.com/

────────────────────────────────────────────────────────────
系统爱好者已整理好的“开箱即用”集合（省去逐个厂商找的时间）
────────────────────────────────────────────────────────────
A) OSDeploy 的 WinPE 驱动仓库（社区整理，按厂商/类型分目录）
   https://github.com/adamaayala/WinPEDriverPack
   amd64 目录下可直接取需要的子目录（不必全下 1GB）：
     Intel-RAID  约 1.75MB（Intel RST/VMD）
     AMD         约 0.14MB
     Dell        约 97MB（含 Intel RST、AMD RAID、LSI MegaRAID 等）
     HP          约 126MB
   把选中的子目录整体复制到本目录即可。注意这是社区镜像，非厂商官方页；
   驱动授权仍归各厂商，仅建议个人使用。

B) OSDeploy 官方 PowerShell 模块（自动从厂商官网抓取/展开驱动包）
   https://github.com/OSDeploy/OSD
   在 Windows PowerShell（管理员）里：
     Install-Module OSD -Scope CurrentUser
     Update-OSDeployCoreDrivers -Name dell    # 或 hp / lenovo
   下载/展开后，把对应 storage 目录复制到本目录。

C) 极简 Intel VMD 专用包（社区整理，体积很小，解压即用）
   https://github.com/52250179/WinPEDrivers
   Storage/Intel/VMD/v19.7z（11~13 代）、v20.7z（14 代及以后）
   解压后把 .inf/.sys/.cat 所在目录放进本目录。

提示：社区镜像可能更新不及时；遇到新平台（如最新 Intel VMD）优先用
Intel 官网的 RST/VMD F6 驱动，或 Dell/HP/Lenovo 官方 WinPE 包。

────────────────────────────────────────────────────────────
注意
────────────────────────────────────────────────────────────
- Dell / HP / Lenovo 的 WinPE 驱动包可免费下载，但**请勿随软件再分发**；
  让使用者自行从上述官方链接下载即可（本软件不内置这些包）。
- 恢复后不要更改 BIOS 的磁盘模式（AHCI/RAID 保持与注入时一致），否则仍可能 0x7B。
- 显卡/声卡/网卡不影响启动，进桌面后再装；网卡驱动可一并放入（方便远程）。
- 注入日志：<程序目录>\logs\dism-universal.log
- 目录示例：
    Drivers\
      Intel\        （iaStorAC.inf / iaStorAC.sys / .cat ...）
      AMD\          （rcraid.inf ...）
      MegaRAID\     （megasas.inf ...）
      virtio\       （viostor\w10\amd64\*.inf ...）
