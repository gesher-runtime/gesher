/* CI-only standalone DLL-provider control for the routing experiment. */
#include <pthread.h>
#include <stdio.h>
static void *worker(void *unused) { (void)unused; return (void *)1; }
int main(void) {
  pthread_t id, self = pthread_self(); void *result = NULL;
  if (!self || !pthread_equal(self,self) || pthread_create(&id,NULL,worker,NULL) ||
      pthread_join(id,&result) || result != (void *)1) return 1;
  puts("PASS routing probe standalone DLL local provider"); return 0;
}
