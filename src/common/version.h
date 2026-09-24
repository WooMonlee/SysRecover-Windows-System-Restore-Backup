#pragma once
// SysRecover 版本号 —— **唯一来源**（改这一个文件，下面各处自动跟随）：
//   • CLI `version` / `diag` 输出
//   • `_zjresy*.log` 的 software_version 字段（src/boot/task.cpp）
//   • GUI 标题栏副标题（CTitleLabelUI 用 SYSRECOVER_VERSION_W 初始化）
//   • dist/version.json（Makefile 调 tools/version.py 生成）
//
// 规则（见 PLAN.md「版本号规则」，自 0.1.3 起生效）：
//   主版本.次版本 **由人指定**（产品口径变化、架构/契约变更时才动）；
//   修订号 **每解决一个问题 +1** —— 提交前执行 `python tools/version.py --bump`。
//   一次发布 = 一个 git tag（vX.Y.Z），三处（version.h / git tag / version.json）同源。
// 说明：契约版本另计（SYSRECOVER_CONTRACT_VERSION，改跨层字段才动）。
#define SYSRECOVER_NAME "SysRecover"
#define SYSRECOVER_VERSION "0.3.1"
// 宽串形态：把窄串字面量加上 L 前缀（两级宏，避免 ## 阻止展开）。
#define SYSRECOVER_WIDEN2(x) L##x
#define SYSRECOVER_WIDEN(x) SYSRECOVER_WIDEN2(x)
#define SYSRECOVER_VERSION_W SYSRECOVER_WIDEN(SYSRECOVER_VERSION)
#define SYSRECOVER_CONTRACT_VERSION 1
