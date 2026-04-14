/* Smoke test for the user-space libc subset that Lua will depend on.
   Prints PASS/FAIL lines so it's easy to eyeball the results. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>

static int failures;

static void pass(const char *name) { printf("PASS %s\n", name); }
static void fail(const char *name, const char *why) {
    printf("FAIL %s (%s)\n", name, why);
    failures++;
}

static void test_malloc_basic(void) {
    void *p = malloc(32);
    if (!p) { fail("malloc_basic", "null"); return; }
    memset(p, 0xAA, 32);
    free(p);
    pass("malloc_basic");
}

static void test_malloc_stress(void) {
    enum { N = 200 };
    void *ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = malloc(16 + (i * 7) % 200);
        if (!ptrs[i]) { fail("malloc_stress", "alloc returned null"); return; }
        memset(ptrs[i], i & 0xFF, 16);
    }
    /* Free every other first, then the rest — exercises coalescing. */
    for (int i = 0; i < N; i += 2) free(ptrs[i]);
    for (int i = 1; i < N; i += 2) free(ptrs[i]);
    pass("malloc_stress");
}

static void test_realloc(void) {
    char *p = malloc(8);
    strcpy(p, "hello");
    p = realloc(p, 32);
    if (!p || strcmp(p, "hello") != 0) { fail("realloc", "content lost"); return; }
    free(p);
    pass("realloc");
}

static jmp_buf jb;
static int jumped_val;

static void do_jump(void) {
    longjmp(jb, 42);
    jumped_val = -1;  /* unreachable */
}

static void test_setjmp(void) {
    int r = setjmp(jb);
    if (r == 0) {
        do_jump();
        fail("setjmp", "returned from longjmp path");
        return;
    }
    if (r == 42) pass("setjmp");
    else fail("setjmp", "wrong value");
}

static void test_snprintf(void) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%d %s %x %05d", -7, "abc", 0xDEAD, 42);
    if (n <= 0 || strcmp(buf, "-7 abc dead 00042") != 0) {
        fail("snprintf", buf);
        return;
    }
    pass("snprintf");
}

static void test_strtod(void) {
    char *end;
    double v = strtod("3.14e2", &end);
    if (v < 313.99 || v > 314.01) { fail("strtod", "value off"); return; }
    long l = strtol("-42", 0, 10);
    if (l != -42) { fail("strtol", "value off"); return; }
    pass("strtod+strtol");
}

static void test_string_extras(void) {
    const char *s = "hello world";
    if (!strchr(s, 'o')) { fail("strchr", ""); return; }
    if (!strstr(s, "world")) { fail("strstr", ""); return; }
    char buf[16] = "abcdef";
    memmove(buf + 2, buf, 4);  /* overlap */
    if (memcmp(buf, "ababcd", 6) != 0) { fail("memmove_overlap", buf); return; }
    pass("string_extras");
}

static void test_ctype(void) {
    if (!isdigit('5') || isdigit('x')) { fail("isdigit", ""); return; }
    if (!isspace(' ') || !isspace('\t') || isspace('x')) { fail("isspace", ""); return; }
    if (tolower('A') != 'a' || toupper('z') != 'Z') { fail("case", ""); return; }
    pass("ctype");
}

static void test_stderr(void) {
    fprintf(stderr, "[libc_test] fprintf to stderr works\n");
    pass("fprintf_stderr");
}

static void test_file_io(void) {
    FILE *f = fopen("/libc_test.tmp", "w");
    if (!f) { fail("fopen_w", ""); return; }
    fwrite("hello\n", 1, 6, f);
    fprintf(f, "num=%d\n", 42);
    fclose(f);

    f = fopen("/libc_test.tmp", "r");
    if (!f) { fail("fopen_r", ""); return; }
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    if (strcmp(buf, "hello\nnum=42\n") != 0) { fail("file_io", buf); return; }
    pass("file_io");
}

int main(void) {
    test_malloc_basic();
    test_malloc_stress();
    test_realloc();
    test_setjmp();
    test_snprintf();
    test_strtod();
    test_string_extras();
    test_ctype();
    test_stderr();
    test_file_io();

    if (failures == 0) printf("[libc_test] ALL PASSED\n");
    else               printf("[libc_test] %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
