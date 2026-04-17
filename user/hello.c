/* hello: prints the exact 22-byte banner the test suite asserts
 * against in pipes.vsh. No trailing newline. */

#include "syscall_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    const char msg[] = "Hello from user space!";
    sys_write(1, msg, sizeof(msg) - 1);
    return 0;
}
