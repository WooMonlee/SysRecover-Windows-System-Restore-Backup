# Contributing

Thanks for helping. SysRecover touches disks and boot configuration, so small
regressions can be destructive. Please keep changes focused and testable.

## Before you start

1. Read [README](README.md) and [AGENTS.md](AGENTS.md) (especially the red
   lines in §0 and the pitfall register in §13).
2. Build with MinGW-w64 GCC 14.2 and run the checks:

```powershell
$env:MINGW64 = "C:\mingw64"
$env:MINGW32 = "C:\mingw32"
$env:PYTHON  = "C:\Python313\python.exe"
mingw32-make -f Makefile check
mingw32-make -f Makefile package
```

## Pull requests

- Keep one logical change per commit.
- Do not add customer data, support bundles, disk images, dumps, or logs.
- Add or update a PIT entry in `AGENTS.md` when fixing a field bug or adding a
  non-obvious workaround.
- User-facing strings must use `Tr()`; update `tools/i18n-en.py` and run
  `python tools/i18n-wrap.py --gen-lang` instead of editing `lang/en.lang`.
- Rescue-layer scripts (`bootfiles/*.sh`, `bootfiles/alpine/init`) require an
  initramfs rebuild via `python tools/build-debian-rescue.py`.
- Run `mingw32-make -f Makefile check` and `mingw32-make -f Makefile package`
  before requesting review.

## Commit messages

Use a short imperative subject, for example:

```
fix(restore): keep the backup path after focus changes
docs(readme): document portable toolchain variables
```

## Licensing

By contributing you agree that your changes are licensed under the project's
MIT license. Do not copy GPL source code into `src/`; keep third-party
components separate and documented in `THIRD_PARTY_LICENSES.txt`.
