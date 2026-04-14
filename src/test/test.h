#ifndef TEST_H
#define TEST_H

#include "include/types.h"
#include "lib/kprintf.h"
#include "lib/string.h"

typedef struct {
    int total;
    int passed;
    int failed;
} test_results_t;

#define TEST_SUITE_BEGIN(name) \
    serial_printf("[TEST] === Suite: %s ===\n", name); \
    test_results_t __results = {0, 0, 0};

#define TEST_SUITE_END() \
    serial_printf("[TEST] Suite results: %d/%d passed\n", \
        __results.passed, __results.total); \
    if (__results.failed > 0) \
        serial_printf("[TEST] SUITE FAIL: %d failed\n", __results.failed); \
    return __results;

#define TEST_ASSERT(cond, msg) do { \
    __results.total++; \
    if (cond) { \
        __results.passed++; \
        serial_printf("[TEST] PASS: %s\n", msg); \
    } else { \
        __results.failed++; \
        serial_printf("[TEST] FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg)  TEST_ASSERT((a) == (b), msg)
#define TEST_ASSERT_NEQ(a, b, msg) TEST_ASSERT((a) != (b), msg)
#define TEST_ASSERT_STR_EQ(a, b, msg) TEST_ASSERT(strcmp(a, b) == 0, msg)

/* Individual suite runners — each returns results */
test_results_t test_string(void);
test_results_t test_pmm(void);
test_results_t test_heap(void);
test_results_t test_vfs(void);
test_results_t test_task(void);
test_results_t test_syscall(void);
test_results_t test_process(void);
test_results_t test_elf(void);
test_results_t test_ansi(void);

/* Run all suites */
void test_run_all(void);

#endif
