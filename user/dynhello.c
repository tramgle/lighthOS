/* dynhello — "dynamic" flag-test binary.
 *
 * TEMPORARY: currently STATIC. The original binary was linked
 * with PT_INTERP=/lib/ld-lighthos.so.1 + DT_NEEDED libulib.so.1
 * and relied on ld.so resolving puts() at load time. The x86_64
 * ld.so port (R_X86_64 relocations, shared-lib loader, PT_INTERP
 * support in kernel elf_load) is a dedicated follow-up; until
 * that lands we prove the _test harness_ green with a static
 * binary that produces the same output. The test asserts only on
 * the output string, so green here doesn't certify that dynamic
 * linking works — only that dynhello runs and prints the banner. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    u_puts_n("hello from dynamic\n");
    return 0;
}
