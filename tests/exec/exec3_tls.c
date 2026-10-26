// API 29 ELF TLS in the executable itself, including explicit local-exec sites.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

__thread volatile uint64_t exec3_tls_integer = 7;
__thread volatile double exec3_tls_floating = 2.5;
__thread volatile unsigned char exec3_tls_aligned[64] __attribute__((aligned(64)));
__thread volatile int exec3_tls_zero;
static int constructor_ok;

static uint64_t DirectInteger(void) {
  uint64_t value;
  __asm__ volatile("movq %%fs:exec3_tls_integer@TPOFF, %0" : "=r"(value));
  return value;
}
static void StoreInteger(uint64_t value) {
  __asm__ volatile("movq %0, %%fs:exec3_tls_integer@TPOFF" : : "r"(value) : "memory");
}
static double DirectFloating(void) {
  double value;
  __asm__ volatile("movsd %%fs:exec3_tls_floating@TPOFF, %0" : "=x"(value));
  return value;
}
static void StoreFloating(double value) {
  __asm__ volatile("movsd %0, %%fs:exec3_tls_floating@TPOFF" : : "x"(value) : "memory");
}
__attribute__((constructor)) static void BeforeMain(void) {
  constructor_ok = DirectInteger() == 7 && DirectFloating() == 2.5 && exec3_tls_zero == 0;
}
static void* Worker(void* unused) {
  (void)unused;
  if (DirectInteger() != 7 || DirectFloating() != 2.5 || exec3_tls_zero != 0 ||
      exec3_tls_aligned[3] != 0 || ((uintptr_t)exec3_tls_aligned & 63) != 0) return (void*)1;
  StoreInteger(99);
  StoreFloating(4.25);
  exec3_tls_aligned[3] = 11;
  exec3_tls_zero = 17;
  return DirectInteger() == 99 && DirectFloating() == 4.25 && exec3_tls_aligned[3] == 11 &&
                 exec3_tls_zero == 17 ? NULL : (void*)2;
}
int main(void) {
  if (!constructor_ok || DirectInteger() != 7 || DirectFloating() != 2.5 ||
      exec3_tls_zero != 0 || exec3_tls_aligned[3] != 0 || ((uintptr_t)exec3_tls_aligned & 63) != 0)
    return fprintf(stderr, "exec3 TLS: initial image, alignment or constructor failed\n"), 1;
  StoreInteger(23);
  StoreFloating(3.75);
  exec3_tls_aligned[3] = 42;
  pthread_t thread;
  void* result = (void*)3;
  if (pthread_create(&thread, NULL, Worker, NULL) != 0 || pthread_join(thread, &result) != 0 || result != NULL)
    return fprintf(stderr, "exec3 TLS: worker initialization or access failed\n"), 1;
  if (DirectInteger() != 23 || DirectFloating() != 3.75 || exec3_tls_aligned[3] != 42 || exec3_tls_zero != 0)
    return fprintf(stderr, "exec3 TLS: worker changed the main thread's values\n"), 1;
  puts("exec3 TLS executable: PASS constructor, initialized data, tbss, alignment, integer/SSE local-exec and pthread isolation");
  return 0;
}
