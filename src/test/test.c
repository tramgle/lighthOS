#include "test/test.h"
#include "include/io.h"

void test_run_all(void) {
    serial_printf("\n[TEST] ========================================\n");
    serial_printf("[TEST] LighthOS Test Suite\n");
    serial_printf("[TEST] ========================================\n\n");

    int total = 0, passed = 0, failed = 0;
    test_results_t r;

    r = test_string();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_pmm();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_heap();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_vfs();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_task();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_syscall();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_process();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_elf();
    total += r.total; passed += r.passed; failed += r.failed;

    r = test_ansi();
    total += r.total; passed += r.passed; failed += r.failed;

    serial_printf("\n[TEST] ========================================\n");
    serial_printf("[TEST] Total: %d/%d passed\n", passed, total);
    if (failed > 0) {
        serial_printf("[TEST] FAILED: %d tests failed\n", failed);
    } else {
        serial_printf("[TEST] ALL PASSED\n");
    }
    serial_printf("[TEST] ========================================\n");

    /* Shutdown QEMU via ACPI */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    for (;;) __asm__ volatile ("hlt");
}
