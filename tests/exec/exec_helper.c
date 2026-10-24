// libexechelper.so: a DT_NEEDED library of libexectest.so, found through
// LD_LIBRARY_PATH by gesher-exec (docs/exec.md). Its constructor runs before
// the program's main, as bionic's linker runs it.
#include <stdio.h>

static int g_helper_ctor;

__attribute__((constructor)) static void helper_ctor(void) { g_helper_ctor = 1; }

const char* exec_helper_hello(void) {
  return g_helper_ctor ? "hello from libexechelper (constructor ran)" : "hello from libexechelper (no constructor)";
}
