# Architecture

SysRecover is a Windows backup/restore front end plus a small Linux rescue
layer. It does not replace the Windows boot manager; it only stages a restore
task and boots the rescue layer when the target is the running system disk.

## Layers

```
src/cli, src/gui
        |
        v
src/app        orchestration, safety checks, staging, history
   |        |        |
   v        v        v
src/disk  src/wim  src/boot
   |        |        |
   v        v        v
Win32 API  libwim   BCD / GRUB4DOS / UEFI NVRAM
        |
        v
bootfiles/  kernel + initramfs + rescue scripts
```

- `src/common`: logging, paths, process runner, i18n, crash handling,
  relocation, VSS, ZIP, CPU cap, EA/refscan helpers.
- `src/disk`: disk/partition enumeration via `DeviceIoControl` (no WMI),
  partition style, space checks, new-disk planning.
- `src/wim`: RAII wrapper around `libwim-15.dll`; capture, apply, list,
  verify, image metadata; exclusions.
- `src/boot`: BCD editing, GRUB4DOS deployment, UEFI Boot#### entries,
  bootfix generation, boot-path policy.
- `src/app`: backup/restore flows, safety checks, A-engine swap/returnold,
  universal restore, support bundles.
- `bootfiles/`: Debian-signed kernel and modules, Alpine userland, init,
  `zjrestore-lite.sh`, `zjswap.sh`, `zjswap-undo.sh`.

## Restore engines

- **A engine (`swap`)**: Windows extracts the image to `<target>:\~new` and
  writes a completion marker; the rescue layer renames the old tree to `~old`
  and swaps `~new` into place. This is the default for a running system disk.
- **B engine (`block`)**: the rescue layer formats the target and applies the
  image with wimlib. It is used when the target is not the running system or
  when explicitly selected.
- **In-place restore**: when the target is not the running system disk (or in
  PE), Windows applies the image directly and runs `bcdboot`; no reboot.

## Contracts

`restore-task.conf` and `_zjresy*.log` are the cross-layer contract. The rescue
layer validates `contract_version` before touching the target. Incremental keys
must not change `contract_version`; breaking changes require both sides to be
released together.

## Safety boundaries

- The target disk is identified by offset, size, and serial (when available),
  not only by drive letter.
- GPT targets are restored with UEFI unless the user explicitly selects an
  in-place path.
- The rescue layer is fail-closed: unknown contracts, unsupported images,
  non-settable reparse points, and EFS-encrypted content are rejected before
  formatting.
- Logs and support bundles are local; no telemetry.
