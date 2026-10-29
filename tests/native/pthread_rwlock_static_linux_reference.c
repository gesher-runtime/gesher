#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t start,held;
static void *reader(void *p) {
  (void)p; int b=pthread_barrier_wait(&start); if (b && b!=PTHREAD_BARRIER_SERIAL_THREAD) return (void *)(uintptr_t)1;
  if (pthread_rwlock_tryrdlock(&lock)) return (void *)(uintptr_t)2;
  b=pthread_barrier_wait(&held); if (b && b!=PTHREAD_BARRIER_SERIAL_THREAD) return (void *)(uintptr_t)3;
  return (void *)(uintptr_t)pthread_rwlock_unlock(&lock);
}
int main(void) {
  pthread_t a,b; void *ra,*rb;
  if (pthread_barrier_init(&start,NULL,2) || pthread_barrier_init(&held,NULL,2) ||
      pthread_create(&a,NULL,reader,NULL) || pthread_create(&b,NULL,reader,NULL) ||
      pthread_join(a,&ra) || pthread_join(b,&rb) || ra || rb || pthread_rwlock_destroy(&lock) ||
      pthread_barrier_destroy(&start) || pthread_barrier_destroy(&held)) return 1;
  puts("PASS Linux concurrent readers on static rwlock"); return 0;
}
