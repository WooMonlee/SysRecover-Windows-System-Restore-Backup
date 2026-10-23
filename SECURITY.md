# Security Policy

SysRecover can format partitions and modify boot configuration. Treat it as a
destructive system tool, not as a general-purpose file utility.

## Reporting a vulnerability

Please **do not** open a public issue for a vulnerability that could lead to
data loss, privilege escalation, boot failure, or exposure of user data.

Use GitHub's **Security advisories** for this repository:

https://github.com/WooMonlee/SysRecover-Windows-System-Restore-Backup/security/advisories/new

Include, when possible:

- SysRecover version (`SysRecover.exe version`) and the contents of
  `version.json`;
- Windows version, firmware mode (BIOS/UEFI), and Secure Boot state;
- exact steps, expected result, actual result;
- a `support` bundle or the `logs/` folder, with sensitive paths redacted.

## Supported versions

Security fixes are applied to the latest release. Older versions may not be
patched.

## Scope notes

- The Linux rescue layer is assembled from signed distribution binaries and
  third-party components; see `THIRD_PARTY_LICENSES.txt`.
- The program requires administrator rights because it reads disks, writes
  boot configuration, and applies images.
- No telemetry is sent. Logs are written locally; users decide whether to
  share a support bundle.
