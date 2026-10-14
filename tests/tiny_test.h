#pragma once
// 极简单元测试框架（零依赖，不引 gtest/Catch）—— 用法见 tests/unit_tests.cpp。
//
// 为什么自己写：本产品坚持零第三方依赖（体积/许可），单元测试只需要"断言 + 统计
// 失败"，几十行足够。`make check` 会编译 tests/*.cpp 并运行。
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace tinytest {

struct Case {
    const char* name;
    std::function<void()> fn;
};

std::vector<Case>& Registry();
int& FailCount();
int& CheckCount();
void ReportFail(const char* file, int line, const std::string& msg);

// 失败信息里把值转成可打印的窄串
std::string Show(const std::string& s);
std::string Show(const char* s);
std::string Show(const std::wstring& s);
std::string Show(const wchar_t* s);
inline std::string Show(bool v) { return v ? "true" : "false"; }
template <class T,
          typename std::enable_if<std::is_arithmetic<T>::value, int>::type = 0>
std::string Show(T v) {
    return std::to_string(v);
}

struct Reg {
    Reg(const char* n, std::function<void()> f) {
        Registry().push_back(Case{n, std::move(f)});
    }
};

}  // namespace tinytest

#define TEST(name)                                     \
    static void name();                                \
    static ::tinytest::Reg tt_reg_##name(#name, name); \
    static void name()

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++::tinytest::CheckCount();                                         \
        if (!(cond))                                                        \
            ::tinytest::ReportFail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        ++::tinytest::CheckCount();                                         \
        auto tt_a = (a);                                                    \
        auto tt_b = (b);                                                    \
        if (!(tt_a == tt_b))                                                \
            ::tinytest::ReportFail(                                         \
                __FILE__, __LINE__,                                         \
                std::string("CHECK_EQ(" #a ", " #b ") -> ") +               \
                    ::tinytest::Show(tt_a) + " != " + ::tinytest::Show(tt_b)); \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                         \
    do {                                                                    \
        ++::tinytest::CheckCount();                                         \
        std::string tt_h = ::tinytest::Show(hay);                           \
        std::string tt_n = ::tinytest::Show(needle);                        \
        if (tt_h.find(tt_n) == std::string::npos)                           \
            ::tinytest::ReportFail(                                         \
                __FILE__, __LINE__,                                         \
                std::string("CHECK_CONTAINS(" #hay ", " #needle             \
                            ") -> missing: ") +                             \
                    tt_n);                                                  \
    } while (0)

#define CHECK_NOT_CONTAINS(hay, needle)                                     \
    do {                                                                    \
        ++::tinytest::CheckCount();                                         \
        std::string tt_h = ::tinytest::Show(hay);                           \
        std::string tt_n = ::tinytest::Show(needle);                        \
        if (tt_h.find(tt_n) != std::string::npos)                           \
            ::tinytest::ReportFail(                                         \
                __FILE__, __LINE__,                                         \
                std::string("CHECK_NOT_CONTAINS(" #hay ", " #needle         \
                            ") -> present: ") +                             \
                    tt_n);                                                  \
    } while (0)
