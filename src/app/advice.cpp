// "下一步建议"实现：建议码 → 文案（原 ops.cpp::ErrorAdvice 的关键词匹配版已废弃，
// 见 advice.h 头部说明）。M2 起本文件的文案改由 i18n 取词（key = AdviceKey）。
#include "common/i18n.h"
#include "advice.h"

namespace sysrecover {

const char* AdviceKey(ErrAdvice a) {
    switch (a) {
        case ADV_BITLOCKER: return "bitlocker";
        case ADV_SPACE: return "space";
        case ADV_INCOMPLETE: return "incomplete";
        case ADV_IMAGE_IN_TARGET: return "image_in_target";
        case ADV_ADMIN: return "admin";
        case ADV_VSS: return "vss";
        case ADV_NONE: break;
    }
    return "none";
}

std::string ErrorAdvice(int rc, ErrAdvice adv) {
    // ① 建议码（由错误产生处给出，与消息语言无关）
    switch (adv) {
        case ADV_BITLOCKER:
            return Tr("\n\n建议：目标盘启用了 BitLocker。请先在「管理员命令提示符」里挂起保护，" "然后重试：\n    manage-bde -protectors -disable X: -rebootcount 1\n" "（X 换成目标盘符。还原会覆盖该盘数据，之后不需要恢复保护。）");
        case ADV_SPACE:
            return Tr("\n\n建议：换一个更大的目标分区，或改用内容更小的镜像。");
        case ADV_INCOMPLETE:
            return Tr("\n\n建议：该镜像上次没有写完（备份中途中断过）。请重新做一次备份。");
        case ADV_IMAGE_IN_TARGET:
            return Tr("\n\n建议：把镜像文件移到别的分区 —— 它不能放在会被格式化的目标分区上。");
        case ADV_ADMIN:
            return Tr("\n\n建议：以管理员身份运行（本程序要读写分区与引导）。");
        case ADV_VSS:
            return {};  // 消息自带 services.msc / sc 两条处理路径，再叠建议纯属冗余（PIT-087）
        case ADV_NONE:
            break;
    }
    // ② 与语言无关的 rc 判定（wimlib/取消码，本来就不看消息文本）
    if (rc == 88)
        return Tr("\n\n建议：备份源里有文件在持续变化（数据库/下载/云同步目录）。" "先停掉这些程序，或把它们所在目录加入排除清单后再备份。");
    if (rc == 6)
        return Tr("\n\n（任务已被用户取消，没有改动目标分区。）");
    return {};
}

}  // namespace sysrecover
