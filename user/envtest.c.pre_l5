/* envtest — exercise envp end-to-end:
   1. setenv/getenv/unsetenv round-trip in the running process.
   2. setenv a var, spawn /bin/env, confirm the child inherits it.
   Prints "OK" on success; FAIL messages + non-zero exit on failure. */

#include "syscall.h"
#include "ulib.h"

int main(void) {
    /* Local setenv/getenv round-trip. */
    if (setenv("FOO", "bar", 1) != 0) { puts("FAIL setenv\n"); return 1; }
    char *v = getenv("FOO");
    if (!v || strcmp(v, "bar") != 0) { puts("FAIL getenv\n"); return 1; }

    /* Overwrite flag. */
    if (setenv("FOO", "baz", 0) != 0) { puts("FAIL setenv_no_overwrite\n"); return 1; }
    v = getenv("FOO");
    if (!v || strcmp(v, "bar") != 0) { puts("FAIL no_overwrite_kept_old\n"); return 1; }

    /* unsetenv. */
    if (unsetenv("FOO") != 0) { puts("FAIL unsetenv\n"); return 1; }
    if (getenv("FOO") != 0)   { puts("FAIL unsetenv_still_present\n"); return 1; }

    /* Local tests passed — announce before the spawn because
       inherited fds share a file but not a write offset, so the
       child's output and any post-spawn parent writes would
       overlap in the redirect file. */
    puts("OK\n");

    /* Cross-process inheritance: set a distinctive var, spawn
       /bin/env, let the parent-inherited envp reach the child.
       All post-spawn parent writes are avoided so child output
       appends cleanly to the redirect file. */
    if (setenv("ENVTEST_MARKER", "inherit", 1) != 0) {
        return 1;
    }
    char *argv[] = { "/bin/env", 0 };
    int pid = sys_spawn("/bin/env", argv);
    if (pid < 0) return 1;
    int status = 0;
    sys_waitpid((uint32_t)pid, &status);
    return 0;
}
