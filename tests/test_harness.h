#ifndef OPENBHZD_TESTS_HARNESS_H
#define OPENBHZD_TESTS_HARNESS_H

/* Minimal assert + counter test harness. Each test_*.c declares a
 * suite_<name>(void) entry point that calls TEST_CASE(name) once per
 * scenario; failed checks bump a global counter and print to stderr.
 *
 * Why a hand-rolled harness vs. Unity / cmocka / etc.? The five
 * modules under test are < 100 LoC each, the suite is < 30 cases, and
 * the firmware toolchain (arm-none-eabi) is the project's only hard
 * dependency. Pulling in a 2000-line vendor framework for this is
 * over-engineering. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern int  g_tests_run;
extern int  g_tests_failed;
extern const char *g_current_case;

#define TEST_CASE(name) do {            \
        g_current_case = (name);        \
        ++g_tests_run;                  \
    } while (0)

#define CHECK(cond) do {                                                \
        if (!(cond)) {                                                  \
            ++g_tests_failed;                                           \
            fprintf(stderr, "FAIL %s:%d in [%s]: %s\n",                 \
                    __FILE__, __LINE__,                                 \
                    g_current_case ? g_current_case : "?", #cond);      \
        }                                                               \
    } while (0)

#define CHECK_EQ_INT(a, b) do {                                         \
        long _a = (long)(a);                                            \
        long _b = (long)(b);                                            \
        if (_a != _b) {                                                 \
            ++g_tests_failed;                                           \
            fprintf(stderr,                                             \
                    "FAIL %s:%d in [%s]: %s == %s -> %ld != %ld\n",     \
                    __FILE__, __LINE__,                                 \
                    g_current_case ? g_current_case : "?",              \
                    #a, #b, _a, _b);                                    \
        }                                                               \
    } while (0)

#define CHECK_EQ_U32(a, b) do {                                         \
        uint32_t _a = (uint32_t)(a);                                    \
        uint32_t _b = (uint32_t)(b);                                    \
        if (_a != _b) {                                                 \
            ++g_tests_failed;                                           \
            fprintf(stderr,                                             \
                    "FAIL %s:%d in [%s]: %s == %s -> 0x%08x != 0x%08x\n", \
                    __FILE__, __LINE__,                                 \
                    g_current_case ? g_current_case : "?",              \
                    #a, #b, (unsigned)_a, (unsigned)_b);                \
        }                                                               \
    } while (0)

void suite_crc16(void);
void suite_crc32(void);
void suite_j1772(void);
void suite_fault(void);
void suite_tlv(void);

#endif
