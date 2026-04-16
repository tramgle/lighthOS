#include "syscall.h"
#include "ulib.h"

int main(int argc, char **argv) {
    int newline = 1;
    int first = 1;
    if (first < argc && strcmp(argv[first], "-n") == 0) { newline = 0; first++; }

    for (int i = first; i < argc; i++) {
        if (i > first) sys_write(1, " ", 1);
        sys_write(1, argv[i], strlen(argv[i]));
    }
    if (newline) sys_write(1, "\n", 1);
    return 0;
}
