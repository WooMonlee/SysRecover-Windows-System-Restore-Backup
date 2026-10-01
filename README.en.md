# SysRecover (九转还原 · standalone edition)

> **English** | [中文](README.md)

> Version `0.4.1` | **x64 + x86 dual architecture** (the Windows side follows the OS bitness; the Linux rescue layer is always x64) | Windows 7 / 10 / 11 / WinPE | Release package ≈47 MB (≈90% of it is the rescue layer)
> License: own code **MIT** (see [`LICENSE`](LICENSE)); third-party components are *separately distributed* — full list, license texts and source-code provenance in [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt)

In one sentence: **back up a Windows system into a single image file, and restore it in one click when you need it.**

There is no shortage of backup/restore tools out there, so let us put our cards on the table first: **how this project is built, how far it actually gets, and where it still falls short** — all of it below, no exaggeration.

> ⚠️ This repository is an **independent product (standalone edition)**. The earlier `WooMonlee/OnekeyRestore` (C# + VHDX multi-point instant restore) is **a different product**; the two are designed and implemented separately, and their docs are not mixed.
>
> 👋 **First time here? Read [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md) first** — current status (verified / unverified), next priorities, and the document map. For a product overview, see [`docs/00-项目简介`](docs/00-项目简介（给协作者）.md).

![SysRecover main window (restore mode)](docs/img/gui-main.png)

<sub>Main window (restore mode): **step 1** pick the image → **step 2** pick the target partition (a dirty disk is marked "current system") → **step 3** start the restore;
the program version is shown at the top right, progress at the bottom.</sub>

---

## 1. What it is — and what it **is not**

**Is**: system backup (hot backup via VSS) → system restore (pick an image + pick a target partition → one-click rollback).

**Is not** (drawing the boundary up front, so nobody uses it for the wrong job):

| Not this | Notes |
|---|---|
| Partition management / resizing / disk cloning | We are not a disk tool; we only handle "write the system back" |
| Data recovery | Not offered |
| **Multi-point restore / differential instant restore** | This product line has **none** of that, and does not plan to (that belongs to another product line) |
| **32-bit OS** | ✅ **Supported**: the Windows side **follows the OS bitness** (x86 on 32-bit, x64 on 64-bit), the launcher in the package root picks automatically; **the Linux rescue layer is always x64** |
| **Windows 2003 / XP / Vista and earlier** | ✗ **Not supported** (UCRT goes down to Vista SP2 / Win7 SP1+; see "Known limitations") |

---

## 2. Why write yet another one

Because in real jobs you hit the following, and a lot of tools fall over on exactly these:

1. **RAID/RAID-controller machines where the rescue environment cannot see the disk** — most WinPE builds do not ship drivers for those cards.
   → Our rescue layer is a full Linux kernel plus **494 storage/filesystem modules** (`megaraid_sas`/`mpt3sas`/`isci`/`vmd`/`hpsa`/
   `aacraid`/`arcmsr`/`pm80xx`/`mvsas`/`lpfc`/`qla2xxx`/`virtio_scsi`… all present), so it works on RAID machines too.
2. **Having to prepare a USB stick / WinPE boot disk** — one more step, one more thing that can go wrong.
   → The rescue layer is **built into the software**: deploy once, then ride the chain all the way through.
3. **Needing to enter BIOS and disable Secure Boot** (what some competing tools ask for).
   → We use the **Microsoft-signed chain** (details in §4 below): the user **neither disables Secure Boot nor enrols any key**.
4. **"Successful" restore that still will not boot** — universal images missing boot files, an incomplete NTFS boot sector, a BCD still pointing at the old drive letter…
   → We **do all of that for the user** (§5).
5. **Getting a half-written image from a previous run and black-screening halfway through the restore.**
   → We **verify first, act second**: an incomplete image is **refused outright**, no coin-flipping.

---

## 3. How it works

```
┌─────────── Windows side (SysRecover.exe / SysRecoverUI.exe) ──────────┐
│  list / diag   inspect the machine (read-only)                        │
│  backup        hot VSS backup → write WIM/ESD (write <target>.tmp     │
│                first, rename only on success)                         │
│  restore       four safety checks → image usability check → pick one: │
│                   ├─ in-place:  format + apply + bcdboot → no reboot  │
│                   └─ staged:    stage task + install boot layer →     │
│                                 reboot                                │
└──────────────────────────────┬─────────────────────────────────────────┘
                               ↓ reboot (only for the "restore the running system disk" path)
┌──────────── built-in Linux rescue layer (vmlinuz + initramfs + restore)┐
│  load storage modules → find target partition and image → format →     │
│  apply → fix boot → reboot                                             │
└────────────────────────────────────────────────────────────────────────┘
```

- CLI and GUI **share the same `app` layer**, so they behave identically (not two implementations).
- Rescue-layer logs go to **`logs/` in the program folder** (kept even on success) — if something breaks, ask the user to send that folder back.

---

## 4. Three boot chains (details in [`AGENTS.md` §7](AGENTS.md))

| Firmware / mode | Chain |
|---|---|
| **BIOS / MBR** | `MBR → bootmgr → BCD (real-mode boot sector → \grldr.mbr) → \grldr → \menu.lst → kernel + initramfs` |
| **UEFI (Secure Boot off)** | We write a **firmware boot entry** (NVRAM `Boot####`) and let the firmware load the kernel, with the command line carried in `OptionalData` |
| **UEFI + Secure Boot on** | `firmware → shimx64.efi (Microsoft dual-signed CA2011+CA2023) → grubx64.efi (Debian-signed GRUB) → Debian-signed kernel + our initramfs` |

On that last one: **nothing is bypassed** — every executable in the chain carries a legitimate signature, and it is the same path Debian itself takes on boot.
So **the user does not enrol MOK and does not turn Secure Boot off**. The shim is **dual-signed CA2011+CA2023**, which covers 2026-era firmware.

---

## 5. Reliability design (why it is hard to mess up)

| Mechanism | How |
|---|---|
| **Writing an image survives interruption** | Write `<target>.tmp` first, rename only on success; an interrupt leaves at worst a temp file, **the original image is untouched** |
| **Bad images are refused** | Before touching anything we check whether the image is "fully written" (set `WRITE_IN_PROGRESS` flag / integrity fails → refuse) |
| **It cannot wipe the wrong disk** | Four-element verification (GUID / disk serial / offset / size); **four restore checks**: target is ESP, BitLocker, a recovery partition, or the image file lives inside the target partition → **any hit is refused** (exit code 4) |
| **The MBR is never touched** | Only the target partition is formatted; rescue files go onto the target partition; **nothing new at the root of data disks** |
| **It boots after the restore** | Writes back the complete NTFS boot region (426 B in sector 0 + sectors 1..8); adds `\bootmgr` and `C:\Boot\BCD`; BCD uses `device boot` (independent of drive letters / disk numbers) |
| **A run can be cancelled** (`0.1.4`) | Closing the window mid-backup/restore offers "abort and exit", stops within a second and deletes the incomplete temp file |

---

## 6. Support matrix (verified vs. **untested**, listed separately)

| Scenario | Status |
|---|---|
| BIOS / MBR full-chain restore | ✅ Verified 2026-09-19 (real hardware / VM) |
| UEFI / GPT full-chain restore (Secure Boot off) | ✅ Verified 2026-09-19 |
| UEFI / GPT + **Secure Boot on** (zero enrolment, zero interaction) | ✅ Verified 2026-09-19 |
| Hot backup (VSS) → boots into the OS afterwards | ✅ Verified 2026-09-19 |
| Running the tool on a **Win7 host** (after installing the VC++ runtime) | ✅ Verified 2026-09-20 |
| **Win7 host restoring a Win10 image** (cross-version) | ✅ Verified 2026-09-20, boots normally afterwards |
| RAID/HBA drivers packaged (Debian kernel, all key HBAs covered) | ✅ Loaded successfully in QEMU (**real hardware pending**) |
| QEMU automated regression (UEFI rescue boot, screen output, PBR probe, BIOS/GRUB4DOS, full drill…) | ✅ See [`docs/07`](docs/07-测试矩阵与回归记录.md) |
| Full regression after **rescuing-layer module trimming** (779→494, dist 54→39 MB) | ✅ Verified 2026-09-24 (incl. end-to-end restore drill, PIT-079/080) |
| **In-place restore from PE / to a non-system disk (no reboot)** | 🚧 **Not yet field-tested** (code ready; checklist in [`docs/09`](docs/09-PE直装验收清单.md)) |
| **"Abort and exit" while busy** (`0.1.4`) | 🚧 **Not yet field-tested** |
| Server RAID on **real hardware**, **zero-install** Win7 | 🚧 Pending (mechanism ready) |
| 32-bit OS (Win7/Win10 x86) | ✅ **Supported**: dual-architecture package, root launcher picks automatically; a 32-bit process re-execs itself from `x64\` on a 64-bit OS (see [`docs/08`](docs/08-32位支持（评估与实现）.md)) |

> Rows marked 🚧 are **not** to be treated as working — that is our own rule: nothing gets a ✅ until it actually ran end to end.

---

## 7. Getting started: GUI

1. Unzip the release package and **double-click `SysRecoverUI.exe`** (one UAC prompt appears, since it must read/write partitions and boot data).
2. **Backup**: switch to "System → File" → choose where to save → click "Back up the system".
3. **Restore**: switch to "File → System" → pick the image (you can also **drag and drop an .esd/.wim into the window**) → pick the target partition →
   click "Restore the system" → choose "Exit and restart" → from there it is fully automatic, ending up in the new system.
   - For batch/unattended runs, tick **`Silent mode`**: no dialogs at all, straight through.

---

## 8. Command-line usage

> CLI and GUI share the same `app` layer and behave identically.
> The CLI also embeds a `requireAdministrator` manifest: in an **elevated command prompt / scheduled task (highest privileges) / PsExec `-s` / SCCM**
> the process is already high-integrity, so it runs **fully silently with no UAC prompt** (that is the path used for lab-style batch deployment).
>
> All commands: `SysRecover.exe help`; per-command help: `SysRecover.exe help backup`.
> There are also `repair-boot` (fix boot without reinstalling) and `history` (operation log), among others.

**Path style (backslash vs forward slash)**:

- Windows file/directory options (`--dest`, `--image`, `--file`, output dir) use **backslashes**: `D:\backup\win10.esd`
- `--source` drive root: `C:`, `C:\` or `C:/` **all work** (normalized to `C:/` internally)
- `--path` in-image path: Windows style, **starts with `\`**, wildcards supported

First, inspect the machine (read-only, safe to run any time):

```cmd
SysRecover.exe list
SysRecover.exe diag
```

- `list`: disks / partitions / file systems / drive letters / ESP / system markers
- `diag`: firmware type, Secure Boot state, whether the boot entry is installed, wimlib self-check; add `--zip` to export a diagnostics bundle (diag text + `logs/` + contract files) for bug reports

### Case 1 · Hot-back-up the current system into an image

```cmd
SysRecover.exe backup --dest D:\backup\win10-20260920.esd --source C: --compress recovery --verify --name "Win10 factory image" --esp
```

- `--source C:`: drive root → automatic VSS hot backup (VSS snapshot + exclusion list); `C:` or `C:/` both fine
- `--compress`: `recovery` (smallest .esd) / `maximum` / `fast`
- `--verify` checks the file right after writing; `--name` sets the sub-image name
- `--esp`: bakes the ESP partition into the **same image** (a second sub-image named ESP) and it is **restored automatically** when you restore the system; skipped automatically if the machine has no ESP
- An existing destination needs `--yes` to overwrite, or use `--append` to add it as a new sub-image of the same WIM
- Helpers: `images --file <image>` lists sub-images (size and description); `verify --image <image>` verifies one
- **Extract single files from an image** (without a full restore — peek inside, or pull out one document):

  ```cmd
  SysRecover.exe extract --file D:\backup\win10.esd --index 1 --path "\Users\*\Desktop\*.docx" --dest D:\out
  ```

  `--path` is repeatable and supports wildcards; files land under `--dest` using their in-image directory tree.

### Case 2 · Restore a universal image to C:

Confirm the disk/partition numbers first:

```cmd
SysRecover.exe list
```

Then restore (`--yes` is **mandatory** = confirm overwriting the target partition):

```cmd
SysRecover.exe restore --image D:\backup\wannei-win10.esd --disk 0 --part 3 --index 1 --yes
```

The execution mode is **chosen automatically**:

| Situation | Behaviour |
|---|---|
| Target is the **running system disk** | Stage the task → **reboot** into the built-in rescue layer → format + apply + fix boot → auto-reboot into the new system |
| Target is **not in use** (in **WinPE**, or restoring to a **non-system disk**) | **In-place restore**: format + apply + `bcdboot` → **done, no reboot** |

- `--index N` picks a sub-image; `--no-repair-boot` skips boot repair
- If the image carries an **ESP sub-image** (made with `--esp`), it is restored **automatically** along with the system — no extra flag needed
- The image must live on a **local disk**: the rescue layer the machine reboots into cannot reach the network (UNC / mapped drives)
- Exit codes: `0` success / `1` generic failure / `2` bad arguments / `3` needs admin / `4` dangerous target refused / `5` image verification failed / `6` cancelled

### Case 3 · An unattended, silent restore entry point

Idea: **stage** the restore task (with the always-present boot module installed), then trigger it from the **boot menu** or a **one-time boot entry**, fully automatically.

1. (Recommended) Install the always-present boot module once via the GUI's "Install boot restore" (UEFI: writes a firmware boot entry; BIOS: a real-mode boot sector entry in the BCD).
2. Stage a silent restore: safety checks → image usability check → write contract → refresh boot layer → arm the one-time boot:

   ```cmd
   SysRecover.exe restore --image D:\backup\wannei-win10.esd --disk 0 --part 3 --index 1 --yes
   ```

3. Reboot — nothing to do from here on:

   ```cmd
   shutdown /r /t 0
   ```

- **One-time semantics**: that boot entry is consumed after one use; normal boots go to Windows as usual.
- **Always-present semantics (UEFI)**: the firmware entry sits at the end of `BootOrder`, selectable from the boot menu at any time;
  the task contract lives on a **data disk** (never formatted), so choosing it again runs the restore once more — an always-present one-click restore in the boot menu.
- ⚠️ **Not always-present under BIOS**: the rescue files are formatted away together with the target partition, so that menu entry **only works for that one run**; use UEFI if you need it to persist.
---

## 9. Building (for people who want to compile it)

```bash
mingw32-make -f Makefile package        # ★ release package → dist/ (root = full x86 set + x64/ + shared assets)
mingw32-make -f Makefile all            # x64 build only → dist/x64
mingw32-make -f Makefile ARCH=x86 all   # x86 build only → dist (root)
mingw32-make -f Makefile check          # unit tests (pure logic, zero dependencies)
mingw32-make -f Makefile clean
```

- Toolchains: **x64 = MinGW-w64 GCC 14.2** (`mingw64`), **x86 = winlibs i686 UCRT GCC 14.2** (`mingw32`);
  `package` needs both (x64 uses the `g++` on PATH, x86 uses the absolute path hard-coded in the `Makefile`).
- Rescue layer assembly: `tools/build-debian-rescue.py` (**Debian-signed kernel + signed modules** + Alpine userland)
- Regression tests: `tools/vmtest/*.ps1` (UEFI/SB, BIOS/GRUB4DOS, screen output, direct firmware boot…) plus `mk-drill.py`/`run-drill.ps1` (end-to-end restore drill); see [`docs/07`](docs/07-测试矩阵与回归记录.md)
- New machine / new environment: see [`docs/10-新环境交接说明`](docs/10-新环境交接说明.md)

### What is in the release package (`dist/`, about 46 MB)

```
SysRecover.exe / SysRecoverUI.exe   # full x86 set (entry point): the real programs + libwim-15.dll + x86 UCRT
x64/{SysRecover.exe, SysRecoverUI.exe, libwim-15.dll, UCRT(16)}
bootfiles/{grldr, grldr.mbr, vmlinuz-zjrestore, initramfs-zjrestore.cpio.gz, zjrestore-lite.sh}
bootfiles/sb/{shimx64.efi, grubx64.efi, grub.cfg}         # Secure Boot chain (x64, independent of host bitness)
skin/ resources/ version.json THIRD_PARTY_LICENSES.txt
```

> **Bitness policy**: the Windows side **follows the OS bitness** (x86 on 32-bit, x64 on 64-bit — mostly for backup compression speed) —
> the package root is the full x86 set, and a 32-bit process on a 64-bit OS **re-executes itself from `x64\`** (`src/common/selfarch.cpp`),
> so **no separate launcher is needed**. **The Linux rescue layer is always x86_64** (independent of the host). Details in [`docs/08`](docs/08-32位支持（评估与实现）.md) §0.

---

## 10. Known limitations and items pending verification

- **32-bit Windows**: supported (the Windows side follows the OS bitness; the package launcher picks automatically). **Real-hardware Win7 x86 testing still pending**;
  the cost: on 32-bit OSes backup compression is slower than 64-bit (`fast`≈0~10%, `recovery`≈20~35%), **restore is 0% slower**. See [`docs/08`](docs/08-32位支持（评估与实现）.md) §0.
- **Secure Boot on 2026 hardware**: we now ship the **Debian dual-signed shim (CA2011+CA2023)**, covering firmware that only trusts the new certificate (CA2023) —
  which Ubuntu's single-signature shim cannot do. Design in [`PLAN.md` §11.1](PLAN.md). Verified by a user on VMware (SB on) ✓;
  "firmware that only trusts CA2023" is still **pending real-hardware verification**.
- **Zero-install on Windows 7**: the exes and `libwim-15.dll` depend on the **UCRT** (not built into Win7) → the release package **ships both x64 and x86 UCRT**
  (`third_party/ucrt/{x64,x86}`, distributed automatically by `make package`), so Win7 needs no extra VC++ runtime install.
- **Windows 2003 / XP / Vista and earlier are unsupported** (including Server 2003): the exes depend on the **UCRT** (Microsoft's redistributable UCRT only covers
  Vista SP2 / Win7 SP1+ / Server 2008 R2 SP1+, **not 2003/XP**), and the official `libwim-15.dll` also needs the UCRT.
  In testing Win2003 fails with `api-ms-win-core-errorhandling-l1-1-0.dll not found`. The product support matrix is **Win7 / Win10 / Win11 / WinPE**.
- **ReFS volumes are unsupported** (a hard limitation of the same class — documented rather than runtime-blocked):
  **do not put the image file on a ReFS partition** — a staged restore runs in the Linux rescue layer, which has **no ReFS driver**
  (no `refs`/`refs3` module in the initramfs; `parse-initramfs.py list` confirms empty), so the apply step cannot read the image and fails
  **with the target partition possibly already quick-formatted** (an expensive failure). **The target partition cannot be ReFS either** (ReFS cannot be a
  Windows boot volume anyway, and the `_zjresy*.log` contract is written to the target root, which the rescue layer also cannot read → task discovery stops).
  **What to do**: keep images on **NTFS** (FAT32/exFAT data partitions are fine as image disks). The in-place path is read and written by Windows itself
  and is not affected; the support matrix remains **NTFS system volumes**.
- **Win7 as target on UEFI firmware is unstable**: Win7's own UEFI support is weak, and during a UEFI restore we **do not refresh the boot file versions on the ESP**
  (the rescue layer runs Linux and cannot execute `bcdboot`) → in testing, restoring Win7 inside a "Win10-configured UEFI VM" produced a **boot loop**.
  **Recommend restoring Win7 targets via BIOS/MBR** (the mainstream Win7 shape). On the todo list (see `docs/14` §3: run `bcdboot` on first boot after the restore).
- **PE in-place restore / RAID on real hardware / closing while busy**: code ready, **pending field testing** ([`docs/11` §3](docs/11-接手指南（读我优先）.md) has the checklist).
- `ZJ_ENABLE_MOK_PATH` (the alternative path: our self-signed UKI + MOK enrolment) is **not compiled and not shipped by default**
  (change it to 1 in `src/boot/uefi.cpp` to enable).

---

## 11. License and third-party components

- **Own code: MIT** (see [`LICENSE`](LICENSE)) — covers `src/`, `skin/`, `tools/`, `tests/` and the build files written for this project;
  feel free to learn from it, modify it and redistribute it (keep the copyright notice).
- This product's own code **statically links no GPL component and modifies no third-party source** (`libwim-15.dll` is **LGPL, dynamically linked**,
  the rest are *separately distributed* aggregates) → not subject to the copyleft derivative-work clauses.
- Third-party components (Debian kernel and modules, GRUB, shim, GRUB4DOS, busybox, musl, ntfs-3g, util-linux,
  wimlib, Duilib, UCRT…) each keep their own licenses. **All are unmodified upstream binaries**; how to obtain their sources
  (Debian pool / Alpine CDN / wimlib site / official GRUB4DOS release page) is documented in
  [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt), section "三、第三方源码获取方式" (that file ships with the package).
- Build/test-only tools (MinGW-w64, osslsigncode, QEMU/OVMF, mtools) are **not distributed with the product**.

---

## 12. Document map

Most documents are written in Chinese.

| Document | Contents |
|---|---|
| [`docs/11-接手指南（读我优先）`](docs/11-接手指南（读我优先）.md) | **Read this first**: status, next steps, document map |
| [`docs/00`](docs/00-项目简介（给协作者）.md) … [`docs/10`](docs/10-新环境交接说明.md) | Requirements / architecture / boot design / cross-layer contract / disk & security / build & compliance / test matrix / 32-bit evaluation / PE acceptance / new environment |
| [`docs/12-相对优势与竞品对比`](docs/12-相对优势与竞品对比.md) | Where we win and lose against similar tools (incl. talking points for customers, and a section on Image for Windows) |
| [`docs/13-开源同类调研（Clonezilla-Rescuezilla-FOG）`](docs/13-开源同类调研（Clonezilla-Rescuezilla-FOG）.md) | Research on open-source peers (Clonezilla / Rescuezilla / FOG) |
| [`docs/14-成熟技术借鉴（可靠性机制调研）`](docs/14-成熟技术借鉴（可靠性机制调研）.md) | Mature reliability mechanisms (Windows `recoverysequence`, Android A/B, RAUC…) — which ones we already use, which we should adopt |
| [`AGENTS.md`](AGENTS.md) | Operator manual: §0 five red lines, §7 boot SOP, **§13 pitfall register (PIT-001~089)**, §18 i18n discipline |
| [`PLAN.md`](PLAN.md) | Roadmap, version-number rules, open decisions |
| [`LICENSE`](LICENSE) / [`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt) | MIT for our own code; third-party list, license texts and source provenance |
