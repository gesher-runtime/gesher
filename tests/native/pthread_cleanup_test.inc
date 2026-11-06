// pthread_cleanup_* invokes callbacks in the calling ELF library. Keep these
// in libc_test.c so x86-64 and translated arm64 exercise the same lifecycle.
static int cleanup_trace[8];
static int cleanup_trace_count;
static pthread_key_t cleanup_key;

static void cleanup_record(void* value) {
  if (cleanup_trace_count < 8) cleanup_trace[cleanup_trace_count++] = (int)(intptr_t)value;
}

static void* cleanup_exit_thread(void* ignored) {
  (void)ignored;
  pthread_setspecific(cleanup_key, (void*)30);
  pthread_cleanup_push(cleanup_record, (void*)10);
  pthread_cleanup_push(cleanup_record, (void*)20);
  pthread_exit((void*)0x42);
  pthread_cleanup_pop(0);
  pthread_cleanup_pop(0);
  return NULL;
}

static void test_pthread_cleanup(void) {
  cleanup_trace_count = 0;
  pthread_cleanup_push(cleanup_record, (void*)1);
  pthread_cleanup_push(cleanup_record, (void*)2);
  pthread_cleanup_pop(0);
  pthread_cleanup_pop(1);
  check("pthread_cleanup_pop skips or executes handler", cleanup_trace_count == 1 &&
        cleanup_trace[0] == 1);

  cleanup_trace_count = 0;
  pthread_key_create(&cleanup_key, cleanup_record);
  pthread_t thread;
  int started;
  int joined = -1;
  void* result = NULL;
  pthread_cleanup_push(cleanup_record, (void*)40);
  started = pthread_create(&thread, NULL, cleanup_exit_thread, NULL);
  if (started == 0) joined = pthread_join(thread, &result);
  check("pthread_exit runs cleanup LIFO before key dtor", started == 0 && joined == 0 &&
        result == (void*)0x42 && cleanup_trace_count == 3 && cleanup_trace[0] == 20 &&
        cleanup_trace[1] == 10 && cleanup_trace[2] == 30);
  pthread_cleanup_pop(1);
  check("pthread cleanup stacks belong to their threads", cleanup_trace_count == 4 &&
        cleanup_trace[3] == 40);
  pthread_key_delete(cleanup_key);
}
