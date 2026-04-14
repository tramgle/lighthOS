#include "test/test.h"
#include "mm/heap.h"

test_results_t test_heap(void) {
    TEST_SUITE_BEGIN("heap");

    /* Basic allocation */
    void *p1 = kmalloc(64);
    TEST_ASSERT_NEQ((uint32_t)p1, 0, "kmalloc(64) returns non-null");

    /* Write and read back */
    memset(p1, 0xAB, 64);
    TEST_ASSERT_EQ(*(uint8_t *)p1, 0xAB, "kmalloc'd memory is writable");

    /* Second allocation is different */
    void *p2 = kmalloc(128);
    TEST_ASSERT_NEQ((uint32_t)p2, 0, "kmalloc(128) returns non-null");
    TEST_ASSERT_NEQ((uint32_t)p1, (uint32_t)p2, "two allocs return different pointers");

    /* Free and reuse */
    uint32_t free_before = heap_get_free();
    kfree(p1);
    uint32_t free_after = heap_get_free();
    TEST_ASSERT(free_after > free_before, "kfree increases free space");

    kfree(p2);

    /* Zero-size allocation */
    void *p3 = kmalloc(0);
    TEST_ASSERT_EQ((uint32_t)p3, 0, "kmalloc(0) returns null");

    /* Large allocation */
    void *p4 = kmalloc(4096);
    TEST_ASSERT_NEQ((uint32_t)p4, 0, "kmalloc(4096) returns non-null");
    kfree(p4);

    TEST_SUITE_END();
}
