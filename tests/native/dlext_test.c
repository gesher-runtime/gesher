// android_dlopen_ext (<android/dlext.h>) as bionic's linker implements it,
// from inside an Android library (x86-64 through elf_load_test, arm64-v8a
// through arm64_elf_test): extended flags are checked, RESERVED_ADDRESS loads
// the library at the start of the caller's reservation or fails without
// touching it, RESERVED_ADDRESS_HINT falls back to any address, a load that
// fails after mapping into the reservation leaves it PROT_NONE and still the
// caller's, and the valid flags gesher does not implement fail with dlerror.
//
// The libraries it loads (tests/native/dlext_target.c) are libdlext_a.so ...
// libdlext_d.so and libdlext_tlsfail.so in the search directory the driver
// passes: gesher_set_search_dir() (elf_load_test) or gesher_selftest(dir)
// (arm64_elf_test). Prints "dlext_test: all passed" when every check passes.

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int failures = 0;
static char g_dir[512] = ".";

static void check(const char* what, int ok) {
  printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

static void path_of(char* out, size_t n, const char* name) {
  snprintf(out, n, "%s/%s", g_dir, name);
}

// Calls android_dlopen_ext on g_dir/name; *error gets dlerror()'s text.
static void* open_ext(const char* name, uint64_t flags, void* addr, size_t size,
                      char* error, size_t error_size) {
  char path[600];
  path_of(path, sizeof(path), name);
  android_dlextinfo info;
  memset(&info, 0, sizeof(info));
  info.flags = flags;
  info.reserved_addr = addr;
  info.reserved_size = size;
  dlerror();
  void* handle = android_dlopen_ext(path, RTLD_NOW, &info);
  const char* e = dlerror();
  snprintf(error, error_size, "%s", e ? e : "");
  if (handle == NULL) printf("    (%s: %s)\n", name, error);
  return handle;
}

// Whether [lo, hi) is mapped and inaccessible ("---p") in /proc/self/maps.
static int range_is_prot_none(uintptr_t lo, uintptr_t hi) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (f == NULL) return 0;
  char line[512];
  uintptr_t at = lo;
  int ok = 1;
  while (at < hi && fgets(line, sizeof(line), f) != NULL) {
    unsigned long long a, b;
    char perms[8];
    if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) != 3) continue;
    if (b <= at || a > at) continue;
    if (strcmp(perms, "---p") != 0) { ok = 0; break; }
    at = (uintptr_t)b;
  }
  fclose(f);
  return ok && at >= hi;
}

// Whether every page of [lo, hi) is still mapped by someone: a mapping that
// must not replace anything is refused there (EEXIST).
static int range_is_taken(uintptr_t lo, uintptr_t hi) {
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  for (uintptr_t p = lo; p < hi; p += page) {
    void* r = mmap((void*)p, page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (r != MAP_FAILED) {
      munmap(r, page);
      return 0;
    }
    if (errno != EEXIST) return 0;
  }
  return 1;
}

// A reservation as an app makes one for android_dlopen_ext.
static uint8_t* reserve(size_t size) {
  void* r = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return r == MAP_FAILED ? NULL : (uint8_t*)r;
}

// What dl_iterate_phdr reports for the object whose path ends in `name`.
struct phdr_query {
  const char* name;
  uintptr_t addr;
  uintptr_t min_vaddr;
  uintptr_t load_size;
  int found;
};
static int find_phdr(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  struct phdr_query* q = (struct phdr_query*)data;
  const char* n = info->dlpi_name ? info->dlpi_name : "";
  size_t ln = strlen(n), lq = strlen(q->name);
  if (ln < lq || strcmp(n + ln - lq, q->name) != 0) return 0;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  uintptr_t lo = UINTPTR_MAX, hi = 0;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD) continue;
    if (ph->p_vaddr < lo) lo = ph->p_vaddr;
    if (ph->p_vaddr + ph->p_memsz > hi) hi = ph->p_vaddr + ph->p_memsz;
  }
  q->addr = info->dlpi_addr;
  q->min_vaddr = lo & ~(page - 1);
  q->load_size = ((hi + page - 1) & ~(page - 1)) - q->min_vaddr;
  q->found = 1;
  return 1;
}
static struct phdr_query query(const char* name) {
  struct phdr_query q = {name, 0, 0, 0, 0};
  dl_iterate_phdr(find_phdr, &q);
  return q;
}

static int answer_of(void* handle) {
  int (*answer)(void) = (int (*)(void))dlsym(handle, "gesher_dlext_answer");
  return answer ? answer() : -1;
}

static void* base_of(void* handle) {
  void* sym = dlsym(handle, "gesher_dlext_answer");
  Dl_info info;
  if (sym == NULL || dladdr(sym, &info) == 0) return NULL;
  return info.dli_fbase;
}

static int run(void) {
  char error[512];
  const size_t big = 1 << 20;  // ample for libdlext_*.so
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);

  // --- the extended flags are checked first --------------------------------
  void* h = open_ext("libdlext_a.so", 0x80, NULL, 0, error, sizeof(error));
  check("an unknown flag fails", h == NULL);
  check("  with \"invalid extended flags to android_dlopen_ext: 0x80\"",
        strstr(error, "invalid extended flags to android_dlopen_ext: 0x80") != NULL);
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_USE_LIBRARY_FD_OFFSET, NULL, 0, error,
               sizeof(error));
  check("USE_LIBRARY_FD_OFFSET without USE_LIBRARY_FD fails",
        h == NULL && strstr(error, "ANDROID_DLEXT_USE_LIBRARY_FD_OFFSET without") != NULL);
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_USE_NAMESPACE, NULL, 0, error, sizeof(error));
  check("USE_NAMESPACE with a null namespace fails",
        h == NULL && strstr(error, "library_namespace is null") != NULL);

  // --- valid flags gesher does not implement fail; they are not ignored ----
  h = open_ext("libdlext_d.so", ANDROID_DLEXT_WRITE_RELRO, NULL, 0, error, sizeof(error));
  check("WRITE_RELRO fails with dlerror (not supported)",
        h == NULL && strstr(error, "not supported") != NULL);
  h = open_ext("libdlext_d.so", ANDROID_DLEXT_FORCE_LOAD, NULL, 0, error, sizeof(error));
  check("FORCE_LOAD fails with dlerror (not supported)",
        h == NULL && strstr(error, "not supported") != NULL);
  check("  and the library was not loaded", !query("libdlext_d.so").found);

  // --- a null extinfo is dlopen ----------------------------------------------
  {
    char path[600];
    path_of(path, sizeof(path), "libdlext_d.so");
    h = android_dlopen_ext(path, RTLD_NOW, NULL);
    check("a null extinfo loads like dlopen", h != NULL && answer_of(h) == 42);
  }

  // --- RESERVED_ADDRESS --------------------------------------------------
  uint8_t* small = reserve(page);
  check("a one-page reservation", small != NULL);
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, small, page, error,
               sizeof(error));
  check("RESERVED_ADDRESS into a reservation too small fails", h == NULL);
  check("  with \"reserved address space ... smaller than ... bytes needed\"",
        strstr(error, "reserved address space ") != NULL &&
            strstr(error, " bytes needed for \"") != NULL);
  check("  and the library was not loaded anywhere", !query("libdlext_a.so").found);
  check("  and the reservation is untouched (still mapped, PROT_NONE)",
        small != NULL && range_is_taken((uintptr_t)small, (uintptr_t)small + page) &&
            range_is_prot_none((uintptr_t)small, (uintptr_t)small + page));
  check("  and the caller can unmap it", small != NULL && munmap(small, page) == 0);

  uint8_t* res = reserve(big);
  check("a 1 MiB reservation", res != NULL);
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, res + 1, big - page, error,
               sizeof(error));
  check("RESERVED_ADDRESS at an address not page aligned fails", h == NULL);
  check("  and the reservation is untouched",
        res != NULL && range_is_prot_none((uintptr_t)res, (uintptr_t)res + big));

  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, res, big, error, sizeof(error));
  check("RESERVED_ADDRESS into a large enough reservation loads", h != NULL);
  struct phdr_query qa = query("libdlext_a.so");
  check("  the ELF header is at the reservation's start",
        h != NULL && memcmp(res, "\177ELF", 4) == 0);
  check("  dladdr's dli_fbase is the reservation's start", h != NULL && base_of(h) == res);
  check("  dl_iterate_phdr's dlpi_addr is reservation - page_start(min p_vaddr)",
        qa.found && qa.addr == (uintptr_t)res - qa.min_vaddr);
  check("  its code and data work", h != NULL && answer_of(h) == 42);
  check("  the rest of the reservation is still mapped PROT_NONE",
        qa.found && qa.load_size < big &&
            range_is_prot_none((uintptr_t)res + qa.load_size, (uintptr_t)res + big) &&
            range_is_taken((uintptr_t)res + qa.load_size, (uintptr_t)res + big));
  {
    uint8_t* other = reserve(big);
    void* again = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, other, big, error,
                           sizeof(error));
    check("loading it again returns the same handle, not moved",
          again == h && h != NULL && base_of(again) == res);
    check("  and leaves the other reservation untouched",
          other != NULL && range_is_prot_none((uintptr_t)other, (uintptr_t)other + big));
    if (other != NULL) munmap(other, big);
  }

  // --- RESERVED_ADDRESS_HINT -------------------------------------------------
  small = reserve(page);
  h = open_ext("libdlext_b.so", ANDROID_DLEXT_RESERVED_ADDRESS_HINT, small, page, error,
               sizeof(error));
  check("RESERVED_ADDRESS_HINT too small loads elsewhere",
        h != NULL && small != NULL && base_of(h) != NULL && base_of(h) != small &&
            answer_of(h) == 42);
  check("  and leaves the reservation untouched",
        small != NULL && range_is_prot_none((uintptr_t)small, (uintptr_t)small + page));
  check("  which the caller can unmap", small != NULL && munmap(small, page) == 0);

  // --- a load that fails after mapping into the reservation -----------------
  uint8_t* res2 = reserve(big);
  check("another 1 MiB reservation", res2 != NULL);
  h = open_ext("libdlext_tlsfail.so", ANDROID_DLEXT_RESERVED_ADDRESS, res2, big, error,
               sizeof(error));
  check("a load failing at relocation (undefined TLS) fails", h == NULL);
  check("  and the reservation is PROT_NONE again and still mapped",
        res2 != NULL && range_is_prot_none((uintptr_t)res2, (uintptr_t)res2 + big) &&
            range_is_taken((uintptr_t)res2, (uintptr_t)res2 + big));

  // The same reservation, with a hint that fits: at its start.
  h = open_ext("libdlext_c.so", ANDROID_DLEXT_RESERVED_ADDRESS_HINT, res2, big, error,
               sizeof(error));
  check("RESERVED_ADDRESS_HINT that fits loads at the reservation's start",
        h != NULL && base_of(h) == res2 && answer_of(h) == 42);

  if (failures == 0) {
    printf("dlext_test: all passed\n");
  } else {
    printf("dlext_test: %d check(s) failed\n", failures);
  }
  fflush(stdout);
  return failures;
}

void gesher_set_search_dir(const char* dir) { snprintf(g_dir, sizeof(g_dir), "%s", dir); }

#if defined(__aarch64__)
// arm64_elf_test passes its first search directory.
int gesher_selftest(const char* dir) {
  if (dir != NULL) gesher_set_search_dir(dir);
  return run();
}
#else
// elf_load_test calls gesher_set_search_dir() first.
int gesher_selftest(void) { return run(); }
#endif
