#include "test/test.h"
#include "kernel/syscall.h"
#include "kernel/task.h"
#include "kernel/tss.h"
#include "kernel/gdt.h"
#include "mm/vmm.h"
#include "mm/pmm.h"

/* Test: invoke int 0x80 from ring 0 with SYS_GETPID */
static void test_syscall_ring0(test_results_t *r) {
    uint32_t pid;
    __asm__ volatile (
        "mov $20, %%eax\n"   /* SYS_GETPID = 20 */
        "int $0x80\n"
        : "=a"(pid)
        :
        : "memory"
    );
    r->total++;
    if (pid == task_current()->id) {
        r->passed++;
        serial_printf("[TEST] PASS: int 0x80 from ring 0 returns correct pid (%u)\n", pid);
    } else {
        r->failed++;
        serial_printf("[TEST] FAIL: int 0x80 returned pid %u, expected %u\n", pid, task_current()->id);
    }
}

/*
 * Test: ring 3 transition.
 * We allocate a user-accessible page, write a small machine code program to it
 * that does SYS_WRITE("R3OK\n") then SYS_EXIT, and jump to it in ring 3.
 *
 * The "program" is assembled as raw bytes:
 *   mov eax, 4        ; SYS_WRITE
 *   mov ebx, 1        ; fd = stdout
 *   mov ecx, <msg>    ; buf = address of message
 *   mov edx, 5        ; count = 5
 *   int 0x80
 *   mov eax, 1        ; SYS_EXIT
 *   xor ebx, ebx      ; code = 0
 *   int 0x80
 *   [message: "R3OK\n"]
 */

/* Flag set by ring 3 write syscall handler to confirm it worked */
static volatile int ring3_write_seen = 0;

/* We'll check serial output for "R3OK" after the test.
   But since the test task exits via SYS_EXIT, we need to run it as a
   separate task and waitpid-like mechanism. For now, we'll create a task
   that jumps to ring 3 and check a flag. */

extern void jump_to_usermode(uint32_t entry, uint32_t user_stack);

/* User-mode code page address (must be in a range we can map as USER) */
#define USER_CODE_PAGE  0x00800000  /* 8MB — outside kernel's 0-4MB range */
#define USER_STACK_TOP  0x00802000  /* stack page at 8MB + 4KB, grows down */
#define USER_STACK_PAGE 0x00801000

static void ring3_task_entry(void) {
    /* This runs in ring 0 as a kernel task.
       Set up user pages and jump to ring 3. */

    /* Allocate and map code page as user-accessible */
    uint32_t code_phys = pmm_alloc_frame();
    vmm_map_page(USER_CODE_PAGE, code_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);

    /* Allocate and map stack page as user-accessible */
    uint32_t stack_phys = pmm_alloc_frame();
    vmm_map_page(USER_STACK_PAGE, stack_phys, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);

    /* Write the user-mode program as raw machine code */
    uint8_t *code = (uint8_t *)USER_CODE_PAGE;
    int i = 0;

    /* mov eax, 4 (SYS_WRITE) */
    code[i++] = 0xB8; code[i++] = 0x04; code[i++] = 0x00; code[i++] = 0x00; code[i++] = 0x00;
    /* mov ebx, 1 (fd=stdout) */
    code[i++] = 0xBB; code[i++] = 0x01; code[i++] = 0x00; code[i++] = 0x00; code[i++] = 0x00;
    /* mov ecx, USER_CODE_PAGE + 40 (address of message string) */
    uint32_t msg_addr = USER_CODE_PAGE + 40;
    code[i++] = 0xB9;
    code[i++] = msg_addr & 0xFF;
    code[i++] = (msg_addr >> 8) & 0xFF;
    code[i++] = (msg_addr >> 16) & 0xFF;
    code[i++] = (msg_addr >> 24) & 0xFF;
    /* mov edx, 5 (count) */
    code[i++] = 0xBA; code[i++] = 0x05; code[i++] = 0x00; code[i++] = 0x00; code[i++] = 0x00;
    /* int 0x80 */
    code[i++] = 0xCD; code[i++] = 0x80;

    /* mov eax, 1 (SYS_EXIT) */
    code[i++] = 0xB8; code[i++] = 0x01; code[i++] = 0x00; code[i++] = 0x00; code[i++] = 0x00;
    /* xor ebx, ebx (exit code 0) */
    code[i++] = 0x31; code[i++] = 0xDB;
    /* int 0x80 */
    code[i++] = 0xCD; code[i++] = 0x80;

    /* hlt (should not reach) */
    code[i++] = 0xF4;

    /* Write message at offset 40 */
    code[40] = 'R'; code[41] = '3'; code[42] = 'O'; code[43] = 'K'; code[44] = '\n';

    /* Jump to user mode! */
    jump_to_usermode(USER_CODE_PAGE, USER_STACK_TOP);

    /* Should never reach here */
    for (;;) __asm__ volatile ("hlt");
}

test_results_t test_syscall(void) {
    TEST_SUITE_BEGIN("syscall");

    /* Test 1: int 0x80 from ring 0 */
    test_syscall_ring0(&__results);

    /* Test 2: ring 3 transition
       Create a task that sets up user pages and jumps to ring 3.
       The ring 3 code does SYS_WRITE("R3OK\n") then SYS_EXIT.
       We verify by checking serial output (the write goes through our syscall handler). */
    task_t *t = task_create("ring3_test", ring3_task_entry);
    TEST_ASSERT_NEQ((uint32_t)t, 0, "ring 3 test task created");

    /* Yield and wait for the ring 3 task to complete */
    uint32_t start = 0;
    /* Simple busy-wait: yield a bunch of times */
    for (int i = 0; i < 200; i++) {
        task_yield();
        if (t->state == TASK_DEAD) break;
    }

    TEST_ASSERT_EQ(t->state, TASK_DEAD, "ring 3 task completed (SYS_EXIT worked)");

    TEST_SUITE_END();
}
