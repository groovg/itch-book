#pragma once

#include <cstdio>

namespace test {
inline int failures = 0;

inline void record(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
        ++failures;
    }
}
}  // namespace test

#define CHECK(cond) ::test::record((cond), #cond, __FILE__, __LINE__)

#define RUN_END()                                                  \
    do {                                                           \
        if (::test::failures != 0) {                               \
            std::fprintf(stderr, "%d check(s) failed\n",           \
                         ::test::failures);                        \
            return 1;                                              \
        }                                                          \
        std::puts("all checks passed");                            \
        return 0;                                                  \
    } while (0)
