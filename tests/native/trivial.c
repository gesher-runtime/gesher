// A self-contained x86-64 Android shared object for the ELF loader's first
// check: no libc, no TLS, no stack protector, so it exercises only mapping,
// RELATIVE/RELR relocations and the init array. Built by the NDK in
// .github/workflows/native-libs.yml with -nostdlib, so it has no DT_NEEDED.

static int g_ctor_ran = 0;
static int g_value = 42;

// A pointer to a global: a R_X86_64_RELATIVE (or DT_RELR) relocation, so the
// loader must apply the load bias.
static int* const g_value_ptr = &g_value;

// Goes in DT_INIT_ARRAY; the loader must run it before the library is used.
__attribute__((constructor)) static void ctor(void) { g_ctor_ran = 1; }

__attribute__((visibility("default"))) int gesher_ctor_ran(void) { return g_ctor_ran; }

__attribute__((visibility("default"))) int gesher_answer(void) { return *g_value_ptr; }

__attribute__((visibility("default"))) int gesher_add(int a, int b) { return a + b; }
