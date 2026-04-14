#include "test/test.h"
#include "kernel/task.h"
#include "kernel/timer.h"

/* Flag set by a test task to prove context switching happened */
static volatile int switch_flag = 0;
static volatile uint32_t counter_val = 0;

static void flag_task_entry(void) {
    switch_flag = 1;
    task_exit();
}

static void counter_task_entry(void) {
    while (1) {
        counter_val++;
    }
}

test_results_t test_task(void) {
    TEST_SUITE_BEGIN("task");

    /* task_init was already called in main.c before tests run */
    task_t *t0 = task_current();
    TEST_ASSERT_NEQ((uint32_t)t0, 0, "task_current returns non-null");
    TEST_ASSERT_EQ(t0->id, 0, "task 0 has id 0");

    /* Create a task */
    task_t *t1 = task_create("test_flag", flag_task_entry);
    TEST_ASSERT_NEQ((uint32_t)t1, 0, "task_create returns non-null");
    TEST_ASSERT_EQ(t1->state, TASK_READY, "new task is READY");

    /* Enable scheduling and yield to let the flag task run */
    switch_flag = 0;
    task_enable_scheduling();
    task_yield();  /* triggers timer IRQ -> schedule -> runs flag_task */

    /* Give it a few more ticks to be safe */
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() < start + 5 && !switch_flag) {
        __asm__ volatile ("hlt");
    }

    TEST_ASSERT_EQ(switch_flag, 1, "context switch executed flag task");

    /* Test preemptive scheduling: create a CPU-spinning task */
    counter_val = 0;
    task_t *t2 = task_create("test_counter", counter_task_entry);
    TEST_ASSERT_NEQ((uint32_t)t2, 0, "counter task created");

    /* Wait a few ticks — the counter task should run preemptively */
    start = timer_get_ticks();
    while (timer_get_ticks() < start + 10) {
        __asm__ volatile ("hlt");
    }

    TEST_ASSERT(counter_val > 0, "preemptive scheduling works (counter > 0)");

    /* Clean up: kill the counter task */
    t2->state = TASK_DEAD;

    TEST_SUITE_END();
}
