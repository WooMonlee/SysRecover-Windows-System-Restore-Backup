# src/ —— 模块地图（改代码先看这里）

分层与依赖方向见 `docs/02-总体架构与模块划分.md`；坑位册见 `AGENTS.md` §13（PIT-xxx）。

```
src/
├─ common/   进程执行 / 日志 / 进度 / 单实例 / 路径 / 配置   （不依赖任何人）
├─ disk/     磁盘与分区枚举（DeviceIoControl，禁 WMI）、固件类型
├─ wim/      libwim 的 RAII 封装（capture/append/apply/verify/probe）、排除清单
├─ boot/     BCD、GRUB4DOS 部署、UEFI 固件启动项、任务/日志写入、bootfix
├─ app/      备份与还原编排（含"就地还原"判定）、安全门禁、快捷方式
├─ cli/      SysRecover.exe：list/backup/restore/verify/images/diag/version
└─ gui/      SysRecoverUI.exe：经典 Duilib（全自绘控件见 ui_skin.cpp）
```

## 改代码时的硬约束

1. **GUI 不直连 `wim`/`boot`** —— 一律经 `app`（保证 CLI/GUI 行为一致）。
2. **`boot` 不依赖 `wim`**；`common` 不被任何人反向依赖。
3. 任何改动都要过：`make all`（**零警告**）+ 相关 QEMU 回归（`tools/vmtest/`）。
4. 涉及契约字段（`restore-task.*` / `progress.json` / `_zjresy*.log`）→ **双端同步**。
5. 新增 `src/*.cpp` 记得加进 `Makefile` 的 `APP_SRC`（CLI/GUI 共用同一静态库）。

## 几个容易踩的点

- `swprintf` 的 `%s` 在 MinGW 下当**窄**字符串用 → 宽字符串必须 `%ls`（PIT-007）。
- `//` 注释**行尾不能是反斜杠**（会行继续吞掉下一行）。
- 经典 Duilib：自绘控件必须用**新标签名**（内建名会被 `UIDlgBuilder` 拦截，PIT-016）。
- 路径/编码：全 `std::wstring` + `CreateProcessW`；GUID 比较用 ASCII。
