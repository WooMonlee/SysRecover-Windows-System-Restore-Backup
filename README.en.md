# SysRecover (Nine-turn Restore)

> One-click Windows system backup and restore  
> **English** | [中文](README.md)

[![ci](https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup/actions/workflows/ci.yml/badge.svg)](https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/WooMonlee/SysRecover-Windows-System-Restore-Backup)](https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup/releases/latest)

**Under 50MB · faster startup than most WinPE · CA2023 support · ESP backup · return to the pre-restore system · installable recovery boot menu · Chinese/English · full CLI**

![Restore mode](docs/img/gui-restore.png)

![Backup mode](docs/img/gui-backup.png)

---

## Pros

- Package under **50MB**; starts faster than most WinPE images
- **Secure Boot CA2011 / CA2023** chain; no need to disable Secure Boot or enroll keys
- Back up the **ESP** together with the system image and restore it automatically
- Keep the old system and **return to the pre-restore system** later
- Install a boot menu as a **recovery fallback** when Windows cannot boot
- Chinese / English UI and **full command-line support**
- **ESD / WIM** images

## Cons

- **64-bit CPUs only**; 32-bit CPUs are no longer supported
- **Windows 7 x64 and later** only; Windows Vista, 2003, XP and earlier are not supported
- Only **ESD / WIM** image formats are supported

---

## 1. What is implemented

1. **wimlib-based core**: hot backup and restore on the Windows side; the Linux rescue layer mainly performs directory exchange and boot repair. ESD/WIM support keeps the package small.
2. **Fast startup**: faster than most PE images, comparable disk I/O, with **CA2011 / CA2023** Secure Boot chains.
3. **Full CLI**: suitable for scripts, labs, and unattended deployment.
4. **Under 50MB** while preserving broad hardware compatibility.
5. **Chinese / English UI**.
6. **ESP backup**.
7. **Installable boot menu**:
   - BIOS: press Up/Down repeatedly before Windows starts to open the menu;
   - UEFI: use the firmware boot-device hotkey (shown in the UI) and select `SysRecover`.
8. **New-disk wizard** for ESP / MSR / system / data partitions.
9. **Universal restore**:
   - Ships with common storage drivers (about 165MB in `Drivers`);
   - Not every machine is covered. Export drivers from the target machine, put them in `Drivers`, and the software injects them offline.
   - You can also export them with built-in Windows 10/11 commands. On the target PC (or a working PC of the same model), open an elevated Command Prompt and run (replace `D:` with a USB drive or data disk, not a partition that will be formatted):
     ```bat
     mkdir D:\Drivers
     dism /online /export-driver /destination:D:\Drivers
     ```
     or `pnputil /export-driver * D:\Drivers`. Merge the exported `Drivers` folder into the program's `Drivers\` folder; `.inf` files are found recursively.
   - This exports third-party drivers only; Windows in-box drivers do not need exporting. If the target PC no longer boots, export from another working PC of the same model. Use OEM drivers locally; do not redistribute them.

## 2. What is no longer supported

1. **64-bit CPUs only**; 32-bit CPUs are not supported.
2. **Windows 7 x64 and later** only; Vista / 2003 / XP and earlier are not supported.

## 3. Why another tool

1. On RAID / server machines, many PE images lack the right RAID drivers. Our rescue layer uses a full Linux kernel plus **494 storage / filesystem modules**.
2. Preparing a USB drive or PE media adds another step and another failure point. The rescue layer is built in: deploy once, then use one chain end to end.
3. Some tools require disabling Secure Boot on Secure Boot machines. We use a Microsoft-signed chain; no BIOS changes and no key enrollment.
4. Many tools only support manual restore and cannot provide a recovery fallback when Windows will not boot.
5. Some tools cannot perform fully automated restores to different systems from the command line.
6. Most tools cannot return to the system that existed before the restore.

## 4. Command-line usage

Show all commands:

```bat
SysRecover.exe help
```

Inspect the machine:

```bat
SysRecover.exe list
SysRecover.exe diag --zip
```

- `list`: disks / partitions / drive letters / ESP
- `diag --zip`: firmware / Secure Boot / boot entries and a diagnostics ZIP

### Back up the current system

```bat
SysRecover.exe backup --dest D:\backup\win10.esd --source C: --compress recovery --verify --esp
```

- `--source`: volume root; `C:` or `C:/`
- `--compress`: `recovery` (smallest) / `maximum` / `fast`
- `--verify`: verify immediately after writing
- `--esp`: include the ESP as a second image in the same file
- Existing destination: add `--yes` to overwrite (mutually exclusive with `--append`)
- `--append`: append another image (mutually exclusive with `--yes`)

### Extract files from an image

```bat
SysRecover.exe extract --file D:\backup\win10.esd --index 1 --path "\Users\*\Desktop\*.docx" --dest D:\out
```

- `--path` may be repeated and supports wildcards; paths start with `\`

### Restore an image

```bat
SysRecover.exe list
SysRecover.exe restore --image D:\backup\win10.esd --disk 0 --part 3 --index 1 --yes
```

- `--yes` is required to confirm overwriting the target partition
- `--index N`: sub-image index, default 1
- `--no-repair-boot`: do not repair boot files automatically
- Automatic mode selection:
  - target is the running system disk: stage the restore, reboot into the rescue layer, then replace the system and repair boot;
  - target is not in use (PE / non-system disk): restore in place without rebooting.
- An image with an ESP sub-image (created with `--esp`) restores the ESP automatically.
- Keep the image on a local partition; the rescue layer cannot access the network.

> PE note: root `SysRecoverUI.exe` is the **x64 entry with icon** and launches `x64\SysRecoverUI.exe`; an x64-only PE (for example NewBee 20210523) can run it directly. On 32-bit Windows use `SysRecoverUI_x86.exe`. That PE also lacks `dism.exe`, so universal driver injection is unavailable; use a full PE/Windows with DISM.

Exit codes:

```text
0 success / 1 failure / 2 bad arguments / 3 administrator required /
4 dangerous target rejected / 5 verification failed / 6 cancelled
```

The UI follows the system language. For English explicitly, use `--lang en` or:

```bat
set SYSRECOVER_LANG=en
```

## 5. Download

- **GitHub Releases (direct download)**:  
  https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup/releases/latest
- **123 Cloud Drive**:  
  https://1812167946.share.123pan.cn/123pan/p00rVv-Jb7jH?pwd=8666#  
  Password: `8666`
- **Baidu Netdisk**:  
  https://pan.baidu.com/s/1Ni0TZgKYkBJd8iCEW0qGUQ?pwd=8666  
  Password: `8666`

> If package size matters, delete the `Drivers` folder (about 165MB). It is only used for universal restore.

## 6. Testing and feedback

Tested on self-owned desktops, laptops and VMs with **Windows 7 / 10 / 11, BIOS, UEFI, x86 and x64**. Feedback from more machines and servers is welcome.

The source will be opened when the project is more mature. Bug reports and feature requests are welcome.

- Email: `ms_w@qq.com`
- QQ group: **1128237828**

Thanks to **公交石头, godworld, tigerhuang, 2010ath26810** for testing feedback.

---

## Development and licensing

- Onboarding / handover: [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md)
- Build and release: [`AGENTS.md`](AGENTS.md)
- Own code: **MIT**, see [`LICENSE`](LICENSE). Third-party notices: [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt)
