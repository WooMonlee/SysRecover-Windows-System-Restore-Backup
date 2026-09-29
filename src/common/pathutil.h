#pragma once
// 路径小工具（纯逻辑，可单测；见 tests/unit_tests.cpp 的 pathutil_* 用例）。
// 起因：2026-09-28 用户 CLI 备份 `--source C:` 报错 —— Win32 里 `C:` 的语义是
// “该盘的**当前目录**”，既不等于根，也不带 wimlib 要求的尾斜杠。
#include <string>

namespace sysrecover {

// 是否是盘符根：`C:` / `C:\` / `C:/`（大小写不敏感，可带首尾空白）。
bool IsVolumeRoot(const std::wstring& p);

// 盘符根 → `X:/`（大写盘符 + 尾斜杠，wimlib 的整盘源路径写法）；
// 不是盘符根则原样返回（调用方自己的路径不替用户改）。
std::wstring NormalizeVolumeRoot(const std::wstring& p);

// 目录的父路径（最后一个 `\` / `/` 之前的部分；没有分隔符时返回空）。
// 例：`D:\backup\a.wim` → `D:\backup`；`D:\a.wim` → `D:\`（盘符根必须带分隔符，
// 否则 GetFileAttributesW/`MakeDirTree` 会拿到 `D:` 这种“当前目录”语义）。
std::wstring ParentDir(const std::wstring& path);

// 递归建目录（已存在且是目录 → 成功；已存在但是文件 → 失败）。返回是否就绪。
bool MakeDirTree(const std::wstring& dir);

}  // namespace sysrecover
