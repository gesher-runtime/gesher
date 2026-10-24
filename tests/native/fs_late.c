// A small API-29 x86-64 Android library with ELF TLS, dlopen()ed by fs_test29.so
// while another thread is already running (tests/native/fs_test.c): its TLS
// has to appear in that thread's existing block. Initial-exec and
// general-dynamic variables, and a stack-protected function.

#include <stdio.h>
#include <string.h>

__thread int late_ie __attribute__((tls_model("initial-exec"))) = 42;
__thread long late_gd = 1234;

__attribute__((noinline)) static int guarded(int v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", v);
  return (int)strlen(buf) > 0 ? v : -1;
}

int late_get_ie(void) { return guarded(late_ie); }
void late_set_ie(int v) { late_ie = v; }
long* late_gd_addr(void) { return &late_gd; }
