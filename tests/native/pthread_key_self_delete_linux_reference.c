/* Independent Linux reference for destructor deleting its own key. */
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
static pthread_key_t key;
static int called, deletion=-1;
static void destructor(void *value) { if (value==(void *)(uintptr_t)1) { ++called; deletion=pthread_key_delete(key); } }
static void *worker(void *unused) { (void)unused; return (void *)(uintptr_t)pthread_setspecific(key,(void *)(uintptr_t)1); }
int main(void) {
  pthread_t thread; void *result=NULL;
  if (pthread_key_create(&key,destructor) || pthread_create(&thread,NULL,worker,NULL) ||
      pthread_join(thread,&result) || result || called!=1 || deletion!=0) return 1;
  puts("PASS Linux destructor deletes its own key and returns"); return 0;
}
