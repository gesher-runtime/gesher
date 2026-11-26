// True guest ARM32 DT_NEEDED dependency, compiled with no host libc.
static int accumulator = 3;
__attribute__((constructor)) static void init_dependency(void) { accumulator = 7; }
int dependency_increment(int x) { return x + accumulator; }
