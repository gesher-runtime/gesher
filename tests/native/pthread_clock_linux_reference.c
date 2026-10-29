#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, stop;
static clockid_t worker_id;
static void *worker(void *unused) {
  (void)unused;
  if (pthread_getcpuclockid(pthread_self(), &worker_id)) return (void *)1;
  pthread_mutex_lock(&lock); ready = 1; pthread_cond_signal(&changed);
  while (!stop) pthread_cond_wait(&changed, &lock);
  pthread_mutex_unlock(&lock); return NULL;
}
int main(void) {
  void *lib = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return 1;
  int (*read_clock)(clockid_t, struct timespec *) = dlsym(lib, "clock_gettime");
  int (*get_id)(pthread_t, clockid_t *) = dlsym(lib, "pthread_getcpuclockid");
  if (!read_clock || !get_id) return 2;
  pthread_t thread; clockid_t main_id, via_symbol; struct timespec value;
  if (pthread_create(&thread, NULL, worker, NULL)) return 3;
  pthread_mutex_lock(&lock); while (!ready) pthread_cond_wait(&changed, &lock);
  if (pthread_getcpuclockid(pthread_self(), &main_id) || get_id(thread, &via_symbol) ||
      via_symbol != worker_id || main_id == worker_id || read_clock(worker_id, &value) ||
      clock_gettime(via_symbol, &value)) return 4;
  stop = 1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
  void *result; if (pthread_join(thread, &result) || result || dlclose(lib)) return 5;
  puts("PASS Linux same-process direct and dynamic-symbol cross-thread clock identity");
  return 0;
}
