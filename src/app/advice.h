#pragma once
// "下一步建议"（P8）的**稳定建议码** —— i18n 前置解耦（PLAN §14 M1）。
//
// 原来 ErrorAdvice 靠「中文关键词匹配中文消息」（has("空间不足") 等 6 处），
// 一旦消息被翻译成英文/德文就**全部失配 → 静默不给建议**（fail-open，测不出来）。
// 改为：**错误产生处直接给码**，消息只负责显示；rc 类判定（88 并发修改 / 6 已取消）
// 本来就与语言无关，继续用 rc 判。
//
// 本文件零依赖（只 <string>），可单独进单测（Makefile TEST_UNITS 含 advice.cpp）。
#include <string>

namespace sysrecover {

enum ErrAdvice {
    ADV_NONE = 0,
    ADV_BITLOCKER,        // 目标盘启用了 BitLocker（2026-09-23 后改为弹框提醒，err 不再带它，留码备用）
    ADV_SPACE,            // 目标分区空间不足（还原前预检）
    ADV_INCOMPLETE,       // 镜像"写入未完成"（半截文件，PIT-057）
    ADV_IMAGE_IN_TARGET,  // 镜像放在会被格式化的目标分区内
    ADV_ADMIN,            // 权限不足（写恢复日志/BCD 等需要管理员）
    ADV_VSS,              // VSS 卷影服务不可用（PIT-087：消息自带处理步骤，不追加建议）
};

// 稳定 ASCII 键：日志 / 单测 / 将来 i18n 取词键都用它，**绝不能翻译**。
const char* AdviceKey(ErrAdvice a);

// rc + 建议码 → "下一步怎么办"（UTF-8 中文，M2 起改由 i18n 取词）。
// 匹配不上就返回空串（宁可不给，也不给错的建议）。
std::string ErrorAdvice(int rc, ErrAdvice adv);

}  // namespace sysrecover
