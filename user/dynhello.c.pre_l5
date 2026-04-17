/* dynhello — pilot dynamically-linked binary.
   References puts from libulib.so.1 (resolved at load time by
   ld-lighthos.so.1). Verifies the full M3 dynamic-linking path
   end-to-end. */

#include "syscall.h"
#include "ulib.h"

int main(void) {
    puts("hello from dynamic\n");
    return 0;
}
