/* dynhello — pilot dynamically-linked binary.
 *
 * Links against libulib.so.1 (DT_NEEDED) via /lib/ld-lighthos.so.1.
 * Full pipe: kernel ELF loader sees PT_INTERP=/lib/ld-lighthos.so.1,
 * loads main + interp, pushes auxv, runs interp; interp loads
 * libulib.so.1, applies R_X86_64 relocations, jumps to main's _start;
 * puts() resolves to libulib.so's copy through the linker's PLT. */
#include "ulib.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    puts("hello from dynamic");
    return 0;
}
