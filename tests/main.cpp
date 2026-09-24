#include "tiny_test.h"

#include <cstdio>

int main() {
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
