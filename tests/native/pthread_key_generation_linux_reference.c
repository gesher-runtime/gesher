/* Linux reference: captured destructor, concurrent deletion and key reuse. */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static pthread_key_t old_key,new_key;
static int entered,released,deleted,delete_result=-1,old_calls,new_calls;
static void old_destructor(void *p) {
  if (p!=(void *)(uintptr_t)1) return;
  pthread_mutex_lock(&mutex); ++old_calls; entered=1; pthread_cond_broadcast(&cv);
  while (!released) pthread_cond_wait(&cv,&mutex);
  pthread_mutex_unlock(&mutex);
}
static void new_destructor(void *p) { if (p==(void *)(uintptr_t)2) ++new_calls; }
static void *old_worker(void *p) { (void)p; return (void *)(uintptr_t)pthread_setspecific(old_key,(void *)(uintptr_t)1); }
static void *new_worker(void *p) { (void)p; return (void *)(uintptr_t)pthread_setspecific(new_key,(void *)(uintptr_t)2); }
static void *deleter(void *p) {
  (void)p; int r=pthread_key_delete(old_key);
  pthread_mutex_lock(&mutex); delete_result=r; deleted=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mutex); return NULL;
}
static int wait_flag(int *flag) {
  struct timespec deadline; if (clock_gettime(CLOCK_REALTIME,&deadline)) return 0; deadline.tv_sec+=5;
  pthread_mutex_lock(&mutex);
  while (!*flag) if (pthread_cond_timedwait(&cv,&mutex,&deadline)) { pthread_mutex_unlock(&mutex); return 0; }
  pthread_mutex_unlock(&mutex); return 1;
}
int main(void) {
  pthread_t old_thread,new_thread,delete_thread; void *result=NULL;
  if (pthread_key_create(&old_key,old_destructor) || pthread_create(&old_thread,NULL,old_worker,NULL) || !wait_flag(&entered)) return 1;
  if (pthread_create(&delete_thread,NULL,deleter,NULL)) return 2;
  if (!wait_flag(&deleted)) {
    pthread_mutex_lock(&mutex); released=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mutex);
    pthread_join(delete_thread,NULL); pthread_join(old_thread,NULL); return 3;
  }
  if (delete_result || pthread_key_create(&new_key,new_destructor) || new_key!=old_key ||
      pthread_create(&new_thread,NULL,new_worker,NULL) || pthread_join(new_thread,&result) || result || new_calls!=1) return 4;
  pthread_mutex_lock(&mutex); released=1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mutex);
  if (pthread_join(delete_thread,NULL) || pthread_join(old_thread,&result) || result || old_calls!=1 || pthread_key_delete(new_key)) return 5;
  puts("PASS Linux held destructor, concurrent delete, same-slot reuse and both generations"); return 0;
}
