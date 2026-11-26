// Real guest ARM32 DT_NEEDED dependency with constructor and finalizer guards.
static int accumulator = 3;
static int consumer_finalized;
__attribute__((constructor)) static void init_dependency(void) { accumulator = 7; }
int dependency_increment(int x) { return x + accumulator; }
void dependency_mark_shutdown(void) { consumer_finalized = 1; }
__attribute__((destructor)) static void fini_dependency(void) {
  // A wrong finalization order is an actual guest fault, not a host mock.
  if (!consumer_finalized) __builtin_trap();
}
