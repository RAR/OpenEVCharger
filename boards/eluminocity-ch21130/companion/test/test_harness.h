/* Minimal dependency-free C test harness. Each test_*.c file is a standalone
 * program: it CHECK()s, then calls TEST_MAIN_END() which returns non-zero on
 * any failure. `make test` compiles and runs each one. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H
#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do {                                                   \
    tests_run++;                                                           \
    if (!(cond)) {                                                         \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
    }                                                                      \
} while (0)

#define CHECK_EQ(a, b) do {                                                \
    tests_run++;                                                           \
    long _a = (long)(a), _b = (long)(b);                                   \
    if (_a != _b) {                                                        \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%ld != %ld)\n",             \
                __FILE__, __LINE__, #a, #b, _a, _b);                       \
    }                                                                      \
} while (0)

#define CHECK_STR(a, b) do {                                               \
    tests_run++;                                                           \
    if (strcmp((a), (b)) != 0) {                                           \
        tests_failed++;                                                    \
        fprintf(stderr, "FAIL %s:%d: \"%s\" == \"%s\"\n",                  \
                __FILE__, __LINE__, (a), (b));                             \
    }                                                                      \
} while (0)

#define TEST_MAIN_END() do {                                               \
    fprintf(stderr, "%s: %d/%d passed\n",                                  \
            __FILE__, tests_run - tests_failed, tests_run);                \
    return tests_failed ? 1 : 0;                                           \
} while (0)

#endif /* TEST_HARNESS_H */
