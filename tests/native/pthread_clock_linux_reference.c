#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, stop, worker_error;
static clockid_t worker_id;
static void *worker(void *unused) {
  (void)unused;
  int error = pthread_getcpuclockid(pthread_self(), &worker_id);
  pthread_mutex_lock(&lock); worker_error = error; ready = 1; pthread_cond_signal(&changed);
  if (error) { pthread_mutex_unlock(&lock); return (void *)1; }
  while (!stop) pthread_cond_wait(&changed, &lock);
  pthread_mutex_unlock(&lock); return NULL;
}
int main(void) {
  void *lib = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return 1;
  int (*read_clock)(clockid_t, struct timespec *) = dlsym(lib, "clock_gettime");
  int (*get_id)(pthread_t, clockid_t *) = dlsym(lib, "pthread_getcpuclockid");
  int (*get_sched)(pthread_t, int *, struct sched_param *) = dlsym(lib, "pthread_getschedparam");
  int (*set_sched)(pthread_t, int, const struct sched_param *) = dlsym(lib, "pthread_setschedparam");
  if (!read_clock || !get_id || !get_sched || !set_sched) return 2;
  pthread_t thread; clockid_t main_id, main_via_symbol, via_symbol; struct timespec value;
  if (pthread_create(&thread, NULL, worker, NULL)) return 3;
  pthread_mutex_lock(&lock); while (!ready) pthread_cond_wait(&changed, &lock);
  if (worker_error || pthread_getcpuclockid(pthread_self(), &main_id) ||
      get_id(pthread_self(), &main_via_symbol) || main_id != main_via_symbol || get_id(thread, &via_symbol) ||
      via_symbol != worker_id || main_id == worker_id || read_clock(worker_id, &value) ||
      clock_gettime(via_symbol, &value)) return 4;
  int direct_policy=-1, dynamic_policy=-2;
  struct sched_param direct_param={0}, dynamic_param={0};
  if (pthread_getschedparam(thread,&direct_policy,&direct_param) ||
      get_sched(thread,&dynamic_policy,&dynamic_param) || direct_policy!=dynamic_policy ||
      direct_param.sched_priority!=dynamic_param.sched_priority ||
      set_sched(thread,direct_policy,&direct_param)) return 6;
  puts("PASS Linux same-process direct and dynamic-symbol live scheduling");
  stop = 1; pthread_cond_signal(&changed); pthread_mutex_unlock(&lock);
  void *result; if (pthread_join(thread, &result) || result || dlclose(lib)) return 5;
  puts("PASS Linux same-process direct and dynamic-symbol cross-thread clock identity");
  return 0;
}
