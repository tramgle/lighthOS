#include "test/test.h"

test_results_t test_string(void) {
    TEST_SUITE_BEGIN("string");

    TEST_ASSERT_EQ(strlen("hello"), 5, "strlen(\"hello\") == 5");
    TEST_ASSERT_EQ(strlen(""), 0, "strlen(\"\") == 0");

    TEST_ASSERT_EQ(strcmp("abc", "abc"), 0, "strcmp equal strings");
    TEST_ASSERT(strcmp("abc", "abd") < 0, "strcmp less than");
    TEST_ASSERT(strcmp("abd", "abc") > 0, "strcmp greater than");

    char buf[32];
    memset(buf, 'A', 8);
    TEST_ASSERT_EQ(buf[0], 'A', "memset first byte");
    TEST_ASSERT_EQ(buf[7], 'A', "memset last byte");

    char src[] = "test";
    char dst[8];
    memcpy(dst, src, 5);
    TEST_ASSERT_STR_EQ(dst, "test", "memcpy copies string");

    TEST_ASSERT(strchr("hello", 'l') != NULL, "strchr finds char");
    TEST_ASSERT_EQ(strchr("hello", 'l') - "hello", 2, "strchr returns correct position");
    TEST_ASSERT(strchr("hello", 'z') == NULL, "strchr returns NULL for missing char");

    char mv_buf[16] = "abcdefgh";
    memmove(mv_buf + 2, mv_buf, 6);
    TEST_ASSERT_EQ(mv_buf[2], 'a', "memmove overlapping forward");

    TEST_SUITE_END();
}
