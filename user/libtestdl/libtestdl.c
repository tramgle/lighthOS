/* libtestdl.so.1 — trivial shared library used only by dlopentest.
   Exports two functions so the dlsym lookup has something
   recognizable to verify. No libulib dependency — keeps DT_NEEDED
   empty so the recursive load path is exercised only by the
   dlopen test binary itself. */

int test_add(int a, int b) { return a + b; }
int test_answer(void) { return 42; }
