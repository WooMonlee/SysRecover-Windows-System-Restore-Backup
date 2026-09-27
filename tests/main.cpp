#include "tiny_test.h"

#include <cstdio>

#include "common/i18n.h"

int main() {
    // 钉死中文界面：断言写的是源语言文本，不能受跑测试那台机器的区域设置影响。
    sysrecover::InitI18n("zh");
    int failed_cases = 0;
    for (const auto& c : tinytest::Registry()) {
        int before = tinytest::FailCount();
        c.fn();
        if (tinytest::FailCount() > before) {
            ++failed_cases;
            std::printf("FAIL %s\n", c.name);
        } else {
            std::printf("ok   %s\n", c.name);
        }
    }
    std::printf("\n%d cases, %d checks, %d failures\n",
                (int)tinytest::Registry().size(), tinytest::CheckCount(),
                tinytest::FailCount());
    return tinytest::FailCount() ? 1 : 0;
}
