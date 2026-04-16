/* env — print environment variables, one per line.
   Future extension: env VAR=val COMMAND... to run COMMAND with
   additions/overrides; skipping that for the first pass. */

#include "syscall.h"
#include "ulib.h"

int main(void) {
    if (!environ) return 0;
    for (int i = 0; environ[i]; i++) {
        puts(environ[i]);
        putchar('\n');
    }
    return 0;
}
