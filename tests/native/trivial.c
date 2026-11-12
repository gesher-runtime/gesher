// A self-contained x86-64 Android shared object for the ELF loader's first
// check: no libc, no TLS, no stack protector, so it exercises only mapping,
// RELATIVE/RELR relocations and the init array. Built by the NDK in
// .github/workflows/native-libs.yml with -nostdlib, so it has no DT_NEEDED.

static int g_ctor_ran = 0;
static int g_value = 42;
static int g_fini_order = 0;
static int* g_fini_observer = 0;

// A pointer to a global: a R_X86_64_RELATIVE (or DT_RELR) relocation, so the
// loader must apply the load bias.
static int* const g_value_ptr = &g_value;

// Goes in DT_INIT_ARRAY; the loader must run it before the library is used.
__attribute__((constructor)) static void ctor(void) { g_ctor_ran = 1; }

// DT_FINI_ARRAY is traversed in reverse order on both native and translated ABIs.
__attribute__((destructor(101))) static void fini_first(void) {
  g_fini_order = g_fini_order * 10 + 1;
  if (g_fini_observer) *g_fini_observer = g_fini_order;
}
__attribute__((destructor(102))) static void fini_second(void) {
  g_fini_order = g_fini_order * 10 + 2;
  if (g_fini_observer) *g_fini_observer = g_fini_order;
}

// The caller owns the observation storage; no export is entered after Close.
__attribute__((visibility("default"))) void gesher_observe_fini(int* result) {
  g_fini_observer = result;
  *result = g_fini_order;
}

__attribute__((visibility("default"))) int gesher_ctor_ran(void) { return g_ctor_ran; }

__attribute__((visibility("default"))) int gesher_answer(void) { return *g_value_ptr; }

__attribute__((visibility("default"))) int gesher_add(int a, int b) { return a + b; }
