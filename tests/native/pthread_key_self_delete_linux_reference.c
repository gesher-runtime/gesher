/* Independent Linux reference for destructor deleting its own key. */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static pthread_key_t key;
static int called, deletion=-1;
static void destructor(void *value) { if (value==(void *)(uintptr_t)1) { ++called; deletion=pthread_key_delete(key); } }
static void *worker(void *unused) { (void)unused; return (void *)(uintptr_t)pthread_setspecific(key,(void *)(uintptr_t)1); }
static int iterations,replaced;
static pthread_key_t replacement;
static void repeated(void *value) {
  if (value!=(void *)(uintptr_t)2) return;
  if (++iterations<4) (void)pthread_setspecific(key,value);
}
static void new_destructor(void *value) { if (value==(void *)(uintptr_t)4) ++replaced; }
static void replace_destructor(void *value) {
  if (value!=(void *)(uintptr_t)3 || pthread_key_delete(key) ||
      pthread_key_create(&replacement,new_destructor) || replacement!=key ||
      pthread_setspecific(replacement,(void *)(uintptr_t)4)) replaced=-100;
}
static void *worker_value(void *value) { return (void *)(uintptr_t)pthread_setspecific(key,value); }
int main(void) {
  pthread_t thread; void *result=NULL;
  if (pthread_key_create(&key,destructor) || pthread_create(&thread,NULL,worker,NULL) ||
      pthread_join(thread,&result) || result || called!=1 || deletion!=0) return 1;
  puts("PASS Linux destructor deletes its own key and returns");
  if (pthread_key_create(&key,repeated) || pthread_create(&thread,NULL,worker_value,(void *)(uintptr_t)2) ||
      pthread_join(thread,&result) || result || iterations!=4 || pthread_key_delete(key)) return 2;
  if (pthread_key_create(&key,replace_destructor) || pthread_create(&thread,NULL,worker_value,(void *)(uintptr_t)3) ||
      pthread_join(thread,&result) || result || replaced!=1 || pthread_key_delete(replacement)) return 3;
  puts("PASS Linux four destructor passes and self-delete recreate generation"); return 0;
}
