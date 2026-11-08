// Included by libc_test.c: the same checks run as x86-64 and translated
// ARM64 ELF calls. The host envelope also compiles the actual shim memory
// section and its registration table to exercise these failure paths without
// a Windows/AOSP build. See tools/test_bionic_allocations.py.
#include <malloc.h>

// A libc contract test must call the exported implementation. At -O2 Clang
// may remove malloc/calloc/memalign/free pairs, assume their errno effects are
// unobservable, or fold realloc(p, 0). Volatile import slots retain the real
// calls and observations without changing optimization of the shared library.
static void* (*volatile allocation_malloc)(size_t) = malloc;
static void* (*volatile allocation_calloc)(size_t, size_t) = calloc;
static void* (*volatile allocation_memalign)(size_t, size_t) = memalign;
static void* (*volatile allocation_realloc)(void*, size_t) = realloc;
static void (*volatile allocation_free)(void*) = free;

static void test_calloc_zero_size(void) {
  volatile size_t excessive_size = SIZE_MAX;
  const size_t requests[][2] = {{0, 0}, {0, 1}, {1, 0}, {0, excessive_size}, {excessive_size, 0}};
  for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); ++i) {
    void* p = allocation_calloc(requests[i][0], requests[i][1]);
    check("calloc zero dimension returns a freeable allocation", p != NULL);
    allocation_free(p);
  }
  errno = 0;
  void* p = allocation_calloc(excessive_size, 2);
  check("calloc nonzero overflow still fails with ENOMEM", p == NULL && errno == ENOMEM);
  allocation_free(p);
}

static void test_allocation_overflow(void) {
  // Keep the request dynamic so compiler allocation-size diagnostics do not
  // prevent deliberately exercising libc's rejection of this input.
  volatile size_t excessive_size = SIZE_MAX;
  const size_t alignments[] = {32, 256, 4096};
  for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
    int sentinel;
    void* out = &sentinel;
    errno = EDOM;
    int result = posix_memalign(&out, alignments[i], excessive_size);
    check("posix_memalign overflow preserves output/errno",
          result == ENOMEM && out == &sentinel && errno == EDOM);
    if (result == 0) allocation_free(out);

    errno = 0;
    void* p = allocation_memalign(alignments[i], excessive_size);
    check("memalign overflow fails with ENOMEM", p == NULL && errno == ENOMEM);
    allocation_free(p);
  }
  int sentinel;
  void* out = &sentinel;
  errno = EDOM;
  int invalid = posix_memalign(&out, 7, 128);
  check("posix_memalign invalid alignment leaves outputs",
        invalid == EINVAL && out == &sentinel && errno == EDOM);
  errno = EDOM;
  int valid = posix_memalign(&out, 256, 128);
  check("posix_memalign success preserves errno/alignment",
        valid == 0 && errno == EDOM && ((uintptr_t)out & 255) == 0);
  if (valid == 0) allocation_free(out);
}

static void test_realloc_failure(void) {
  volatile size_t excessive_size = SIZE_MAX;
  const size_t alignments[] = {64, 4096};
  for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); ++i) {
    unsigned char* p = (unsigned char*)allocation_memalign(alignments[i], 128);
    check("aligned realloc failure fixture allocated", p != NULL);
    if (p == NULL) continue;
    memset(p, 0x5a, 128);
    errno = 0;
    void* failed = allocation_realloc(p, excessive_size);
    check("aligned realloc rejects impossible growth", failed == NULL && errno == ENOMEM);
    if (failed != NULL) {
      allocation_free(failed);
      continue;
    }
    int intact = malloc_usable_size(p) >= 128;
    for (size_t j = 0; j < 128; ++j) intact &= p[j] == 0x5a;
    check("failed realloc keeps original storage usable", intact);
    // Success after failure requires the old ownership record to survive too.
    unsigned char* grown = (unsigned char*)allocation_realloc(p, 256);
    int copied = grown != NULL;
    if (grown != NULL) {
      for (size_t j = 0; j < 128; ++j) copied &= grown[j] == 0x5a;
    }
    check("realloc can retry after aligned allocation failure", copied);
    allocation_free(grown != NULL ? grown : p);
  }
}

static void test_reallocarray_contract(void) {
  // reallocarray was added in API 29; dlsym exercises the runtime export
  // even though the shared regression fixture targets API 26.
  typedef void* (*reallocarray_fn)(void*, size_t, size_t);
  reallocarray_fn resize = (reallocarray_fn)dlsym(RTLD_DEFAULT, "reallocarray");
  check("reallocarray is a callable runtime export", resize != NULL);
  if (resize == NULL) return;

  unsigned char* p = (unsigned char*)allocation_memalign(256, 128);
  check("reallocarray fixture allocated", p != NULL);
  if (p == NULL) return;
  memset(p, 0x36, 128);
  const size_t count = (SIZE_MAX / 2) + 5;
  errno = 0;
  void* failed = resize(p, count, 2);
  check("reallocarray rejects wrapped multiplication", failed == NULL && errno == ENOMEM);
  if (failed != NULL) {
    allocation_free(failed);
    return;
  }
  int intact = malloc_usable_size(p) >= 128;
  for (size_t i = 0; i < 128; ++i) intact &= p[i] == 0x36;
  check("reallocarray overflow retains aligned input", intact);
  errno = 0;
  failed = resize(p, 2, count);
  check("reallocarray rejects both overflow operand orders", failed == NULL && errno == ENOMEM);
  if (failed != NULL) {
    allocation_free(failed);
    return;
  }
  unsigned char* grown = (unsigned char*)resize(p, 16, 16);
  int copied = grown != NULL;
  if (grown != NULL) {
    for (size_t i = 0; i < 128; ++i) copied &= grown[i] == 0x36;
  }
  check("reallocarray preserves contents on valid growth", copied);
  allocation_free(grown != NULL ? grown : p);

  void* fresh = resize(NULL, 8, 8);
  check("reallocarray allocates for null input", fresh != NULL);
  if (fresh != NULL) {
    void* zero = resize(fresh, 0, 8);
    check("reallocarray zero count releases its allocation", zero == NULL);
    allocation_free(zero);
  }
}

static void test_realloc_zero_size(void) {
  void* p = allocation_memalign(256, 128);
  check("zero-size aligned realloc fixture allocated", p != NULL);
  if (p != NULL) {
    void* zero = allocation_realloc(p, 0);
    check("realloc aligned pointer to zero frees and returns null", zero == NULL);
    allocation_free(zero);
  }
  p = allocation_malloc(128);
  check("zero-size ordinary realloc fixture allocated", p != NULL);
  if (p != NULL) {
    void* zero = allocation_realloc(p, 0);
    check("realloc ordinary pointer to zero returns null", zero == NULL);
    allocation_free(zero);
  }
  p = allocation_realloc(NULL, 0);
  check("realloc null/zero returns a freeable allocation", p != NULL);
  allocation_free(p);
}

static void test_allocation_contracts(void) {
  test_calloc_zero_size();
  test_allocation_overflow();
  test_realloc_failure();
  test_reallocarray_contract();
  test_realloc_zero_size();
}
