#include "test/test.h"
#include "mm/pmm.h"

test_results_t test_pmm(void) {
    TEST_SUITE_BEGIN("pmm");

    uint32_t free_before = pmm_get_free_count();
    TEST_ASSERT(free_before > 0, "pmm has free frames");

    /* Alloc a frame */
    uint32_t frame = pmm_alloc_frame();
    TEST_ASSERT_NEQ(frame, 0, "pmm_alloc returns non-zero");
    TEST_ASSERT(frame % PAGE_SIZE == 0, "pmm_alloc returns page-aligned address");
    TEST_ASSERT_EQ(pmm_get_free_count(), free_before - 1, "free count decremented");

    /* Free and realloc */
    pmm_free_frame(frame);
    TEST_ASSERT_EQ(pmm_get_free_count(), free_before, "free count restored after free");

    uint32_t frame2 = pmm_alloc_frame();
    TEST_ASSERT_NEQ(frame2, 0, "pmm_alloc after free returns non-zero");
    /* Clean up */
    pmm_free_frame(frame2);

    /* Alloc multiple */
    uint32_t f1 = pmm_alloc_frame();
    uint32_t f2 = pmm_alloc_frame();
    TEST_ASSERT_NEQ(f1, f2, "consecutive allocs return different frames");
    pmm_free_frame(f1);
    pmm_free_frame(f2);

    TEST_SUITE_END();
}
