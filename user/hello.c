#include "syscall.h"

int main(void) {
    sys_write(1, "Hello from user space!\n", 22);
    return 0;
}
