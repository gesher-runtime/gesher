// android_dlopen_ext (<android/dlext.h>) as bionic's linker implements it,
// from inside an Android library (x86-64 through elf_load_test, arm64-v8a
// through arm64_elf_test; each also with GESHER_MMAP_NO_PLACEHOLDERS=1, where
// the loader copies segments instead of mapping file views):
//   - extended flags are checked first; the valid flags gesher does not
//     implement fail with dlerror, also for a library already loaded;
//     FORCE_LOAD loads a library not yet loaded and fails for a loaded one;
//   - RESERVED_ADDRESS loads the library at the start of the caller's
//     reservation (also with a non-zero first p_vaddr and with a gap between
//     segments, which stays PROT_NONE), or fails without touching it, with
//     bionic's message; only the span counts (reserved_size SIZE_MAX loads);
//   - RESERVED_ADDRESS_HINT falls back to any address; with RESERVED_ADDRESS
//     too, it is strict;
//   - a load failing after mapping into the reservation leaves it PROT_NONE
//     and the caller's; a span reaching unmapped address space fails cleanly;
//   - Chrome's pattern: a 0xc000000 reservation whose tail is unmapped after
//     the load, then the library's code and data used;
//   - the last dlclose of a reserved library unloads it, so the reservation
//     can be unmapped and nothing finds the library any more.
//
// The libraries it loads (tests/native/dlext_target.c) are in the search
// directory the driver passes: gesher_set_search_dir() (elf_load_test) or
// gesher_selftest(dir) (arm64_elf_test). Prints "dlext_test: all passed".

#include <android/dlext.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
static size_t g_page;

static void check(const char* what, int ok) {
  printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

// A step that prepares a check: reported (and failing the run) only when it
// does not work.
static int setup(const char* what, int ok) {
  if (!ok) {
    printf("  SETUP FAILED: %s\n", what);
    ++failures;
  }
  return ok;
}

static void path_of(char* out, size_t n, const char* name) {
  snprintf(out, n, "%s/%s", g_dir, name);
}

static void* open_ext_info(const char* name, const android_dlextinfo* info, char* error,
                           size_t error_size) {
  char path[600];
  path_of(path, sizeof(path), name);
  dlerror();
  void* handle = android_dlopen_ext(path, RTLD_NOW, info);
  const char* e = dlerror();
  snprintf(error, error_size, "%s", e ? e : "");
  if (handle == NULL) printf("    (%s: %s)\n", name, error);
  return handle;
}

// android_dlopen_ext on g_dir/name; *error gets dlerror()'s text.
static void* open_ext(const char* name, uint64_t flags, void* addr, size_t size, char* error,
                      size_t error_size) {
  android_dlextinfo info;
  memset(&info, 0, sizeof(info));
  info.flags = flags;
  info.reserved_addr = addr;
  info.reserved_size = size;
  info.relro_fd = -1;
  info.library_fd = -1;
  return open_ext_info(name, &info, error, error_size);
}

// Whether [lo, hi) is mapped and inaccessible ("---p") in /proc/self/maps.
// Reserved address space also shows so: range_is_taken tells them apart.
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

// Whether a mapping that must not replace anything is refused (EEXIST) at
// every page of [lo, hi): all of it is still mapped by someone.
static int range_is_taken(uintptr_t lo, uintptr_t hi) {
  for (uintptr_t p = lo; p < hi; p += g_page) {
    void* r = mmap((void*)p, g_page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (r != MAP_FAILED) {
      munmap(r, g_page);
      return 0;
    }
    if (errno != EEXIST) return 0;
  }
  return 1;
}

// Whether no page of [lo, hi) is mapped in Linux terms: a mapping that must
// not replace anything succeeds at each (pages munmap left in a reservation).
static int range_is_free(uintptr_t lo, uintptr_t hi) {
  for (uintptr_t p = lo; p < hi; p += g_page) {
    void* r = mmap((void*)p, g_page, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (r == MAP_FAILED) return 0;
    munmap(r, g_page);
  }
  return 1;
}

// Whether no line of /proc/self/maps covers any of [lo, hi): nothing at all
// (not even reserved address space) is there. A fresh mapping cannot test
// this here: gesher places new mappings at 64 KiB boundaries only.
static int range_is_unmapped(uintptr_t lo, uintptr_t hi) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (f == NULL) return 0;
  char line[512];
  int ok = 1;
  while (fgets(line, sizeof(line), f) != NULL) {
    unsigned long long a, b;
    if (sscanf(line, "%llx-%llx", &a, &b) != 2) continue;
    if (a < hi && b > lo) { ok = 0; break; }
  }
  fclose(f);
  return ok;
}

// A reservation still exactly as the caller made it: PROT_NONE and mapped.
static int untouched(const uint8_t* r, size_t size) {
  return r != NULL && range_is_prot_none((uintptr_t)r, (uintptr_t)r + size) &&
         range_is_taken((uintptr_t)r, (uintptr_t)r + size);
}

// A reservation as an app makes one for android_dlopen_ext.
static uint8_t* reserve(size_t size) {
  void* r = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return r == MAP_FAILED ? NULL : (uint8_t*)r;
}

// The load span of g_dir/name from its program headers, as the linker
// computes it: page_end(max p_vaddr + p_memsz) - page_start(min p_vaddr).
static size_t span_of(const char* name, uintptr_t* min_vaddr) {
  char path[600];
  path_of(path, sizeof(path), name);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return 0;
  Elf64_Ehdr eh;
  size_t span = 0;
  if (pread(fd, &eh, sizeof(eh), 0) == (ssize_t)sizeof(eh) && eh.e_phnum <= 64) {
    Elf64_Phdr ph[64];
    if (pread(fd, ph, eh.e_phnum * sizeof(Elf64_Phdr), (off_t)eh.e_phoff) ==
        (ssize_t)(eh.e_phnum * sizeof(Elf64_Phdr))) {
      uint64_t lo = UINT64_MAX, hi = 0;
      for (int i = 0; i < eh.e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr < lo) lo = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > hi) hi = ph[i].p_vaddr + ph[i].p_memsz;
      }
      if (lo != UINT64_MAX) {
        lo &= ~(uint64_t)(g_page - 1);
        span = (size_t)(((hi + g_page - 1) & ~(uint64_t)(g_page - 1)) - lo);
        if (min_vaddr) *min_vaddr = (uintptr_t)lo;
      }
    }
  }
  close(fd);
  return span;
}

// What dl_iterate_phdr reports for the object whose path ends in `name`.
struct phdr_query {
  const char* name;
  uintptr_t addr;
  int found;
  // Page ranges [start, end) of its PT_LOAD segments, in address order.
  uintptr_t seg[8][2];
  int segs;
};
static int find_phdr(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  struct phdr_query* q = (struct phdr_query*)data;
  const char* n = info->dlpi_name ? info->dlpi_name : "";
  size_t ln = strlen(n), lq = strlen(q->name);
  if (ln < lq || strcmp(n + ln - lq, q->name) != 0 || (ln > lq && n[ln - lq - 1] != '/')) return 0;
  q->addr = info->dlpi_addr;
  q->found = 1;
  for (int i = 0; i < info->dlpi_phnum && q->segs < 8; ++i) {
    const ElfW(Phdr)* ph = &info->dlpi_phdr[i];
    if (ph->p_type != PT_LOAD) continue;
    q->seg[q->segs][0] = (info->dlpi_addr + ph->p_vaddr) & ~(uintptr_t)(g_page - 1);
    q->seg[q->segs][1] =
        (info->dlpi_addr + ph->p_vaddr + ph->p_memsz + g_page - 1) & ~(uintptr_t)(g_page - 1);
    ++q->segs;
  }
  return 1;
}
static struct phdr_query query(const char* name) {
  struct phdr_query q;
  memset(&q, 0, sizeof(q));
  q.name = name;
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

// Loaded at the reservation's start, as bionic places it, and working.
static void check_loaded_at(void* h, const char* name, uint8_t* res) {
  uintptr_t min_vaddr = 0;
  span_of(name, &min_vaddr);
  struct phdr_query q = query(name);
  check("  the ELF header is at the reservation's start (as Chrome reads it)",
        h != NULL && memcmp(res + 0, "\177ELF", 4) == 0);
  check("  dladdr's dli_fbase is the reservation's start", h != NULL && base_of(h) == res);
  check("  dl_iterate_phdr's dlpi_addr is reservation - page_start(min p_vaddr)",
        q.found && q.addr == (uintptr_t)res - min_vaddr);
  check("  its code and data work", h != NULL && answer_of(h) == 42);
}

static int run(void) {
  char error[1024];
  const size_t big = 1 << 20;  // ample for the small libdlext_*.so
  g_page = (size_t)sysconf(_SC_PAGESIZE);

  // --- the extended flags are checked first --------------------------------
  void* h = open_ext("libdlext_a.so", 0x80, NULL, 0, error, sizeof(error));
  check("an unknown flag fails with \"invalid extended flags to android_dlopen_ext: 0x80\"",
        h == NULL && strstr(error, "invalid extended flags to android_dlopen_ext: 0x80") != NULL);
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
  h = open_ext("libdlext_d.so", ANDROID_DLEXT_USE_RELRO, NULL, 0, error, sizeof(error));
  check("USE_RELRO fails with dlerror (not supported)",
        h == NULL && strstr(error, "not supported") != NULL);
  {
    char path[600];
    path_of(path, sizeof(path), "libdlext_d.so");
    android_dlextinfo info;
    memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = open(path, O_RDONLY);
    h = open_ext_info("libdlext_d.so", &info, error, sizeof(error));
    check("USE_LIBRARY_FD fails with dlerror (not supported)",
          h == NULL && strstr(error, "not supported") != NULL);
    if (info.library_fd >= 0) close(info.library_fd);
  }
  check("  and the library was not loaded", !query("libdlext_d.so").found);

  // --- a null extinfo is dlopen ----------------------------------------------
  {
    char path[600];
    path_of(path, sizeof(path), "libdlext_d.so");
    h = android_dlopen_ext(path, RTLD_NOW, NULL);
    check("a null extinfo loads like dlopen", h != NULL && answer_of(h) == 42);
    dlclose(h);
    check("  dlclose of a library not in a reservation keeps it loaded",
          query("libdlext_d.so").found && answer_of(h) == 42);
  }

  // --- FORCE_LOAD of a library not loaded yet is an ordinary load -----------
  h = open_ext("libdlext_e.so", ANDROID_DLEXT_FORCE_LOAD, NULL, 0, error, sizeof(error));
  check("FORCE_LOAD of a library not loaded yet loads it", h != NULL && answer_of(h) == 42);

  // --- RESERVED_ADDRESS too small -------------------------------------------
  uint8_t* small = reserve(g_page);
  if (!setup("a one-page reservation", small != NULL)) return failures;
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, small, g_page, error,
               sizeof(error));
  check("RESERVED_ADDRESS into a reservation too small fails", h == NULL);
  {
    char path[600], real[PATH_MAX], expect[1400];
    path_of(path, sizeof(path), "libdlext_a.so");
    size_t span = span_of("libdlext_a.so", NULL);
    snprintf(expect, sizeof(expect),
             "dlopen failed: reserved address space %zd smaller than %zd bytes needed for \"%s\"",
             (ssize_t)g_page - (ssize_t)span, (ssize_t)span,
             realpath(path, real) ? real : path);
    check("  with bionic's message: reserved_size - span, span and the file's path",
          span != 0 && strcmp(error, expect) == 0);
    if (strcmp(error, expect) != 0) printf("    expected: %s\n", expect);
  }
  check("  and the library was not loaded anywhere", !query("libdlext_a.so").found);
  check("  and the reservation is untouched (PROT_NONE, still mapped)", untouched(small, g_page));
  check("  and the caller can unmap it", munmap(small, g_page) == 0);

  // RESERVED_ADDRESS with RESERVED_ADDRESS_HINT: the strict one wins.
  small = reserve(g_page);
  if (!setup("a one-page reservation", small != NULL)) return failures;
  h = open_ext("libdlext_both.so",
               ANDROID_DLEXT_RESERVED_ADDRESS | ANDROID_DLEXT_RESERVED_ADDRESS_HINT, small, g_page,
               error, sizeof(error));
  check("RESERVED_ADDRESS|HINT too small fails (RESERVED_ADDRESS wins)",
        h == NULL && strstr(error, "smaller than") != NULL && !query("libdlext_both.so").found);
  check("  and the reservation is untouched", untouched(small, g_page));
  munmap(small, g_page);

  // --- RESERVED_ADDRESS that fits -------------------------------------------
  uint8_t* res = reserve(big);
  if (!setup("a 1 MiB reservation", res != NULL)) return failures;
  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, res + 1, big - g_page, error,
               sizeof(error));
  check("RESERVED_ADDRESS at an address not page aligned fails", h == NULL);
  check("  and the reservation is untouched", untouched(res, big));

  h = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, res, big, error, sizeof(error));
  check("RESERVED_ADDRESS into a large enough reservation loads", h != NULL);
  check_loaded_at(h, "libdlext_a.so", res);
  size_t span_a = span_of("libdlext_a.so", NULL);
  check("  the rest of the reservation is untouched",
        span_a != 0 && span_a < big && untouched(res + span_a, big - span_a));

  // --- extended flags on a library already loaded ---------------------------
  {
    uint8_t* other = reserve(big);
    if (!setup("another 1 MiB reservation", other != NULL)) return failures;
    void* again = open_ext("libdlext_a.so", ANDROID_DLEXT_RESERVED_ADDRESS, other, big, error,
                           sizeof(error));
    check("RESERVED_ADDRESS of a loaded library returns it, not moved",
          again == h && h != NULL && base_of(again) == res);
    check("  and leaves the other reservation untouched", untouched(other, big));
    munmap(other, big);
    again = open_ext("libdlext_a.so", ANDROID_DLEXT_FORCE_LOAD, NULL, 0, error, sizeof(error));
    check("FORCE_LOAD of a loaded library fails (no second copy), not the first copy",
          again == NULL && strstr(error, "not supported") != NULL);
    again = open_ext("libdlext_a.so", ANDROID_DLEXT_WRITE_RELRO, NULL, 0, error, sizeof(error));
    check("WRITE_RELRO on a loaded library fails, not ignored",
          again == NULL && strstr(error, "not supported") != NULL);
    again = open_ext("libdlext_a.so", ANDROID_DLEXT_USE_LIBRARY_FD, NULL, 0, error,
                     sizeof(error));
    check("USE_LIBRARY_FD on a loaded library fails, not ignored",
          again == NULL && strstr(error, "not supported") != NULL);
    again = open_ext("libdlext_a.so", 0, NULL, 0, error, sizeof(error));
    check("flags 0 on a loaded library returns it", again == h);
  }

  // --- only the span counts: reserved_size SIZE_MAX --------------------------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hs = open_ext("libdlext_huge.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, SIZE_MAX, error,
                        sizeof(error));
    check("RESERVED_ADDRESS with reserved_size SIZE_MAX loads at the reservation's start",
          hs != NULL && base_of(hs) == r && answer_of(hs) == 42);
  }

  // --- a first p_vaddr other than 0 -----------------------------------------
  {
    uintptr_t min_vaddr = 0;
    size_t span = span_of("libdlext_vaddr.so", &min_vaddr);
    uint8_t* r = reserve(big);
    if (!setup("libdlext_vaddr.so has a non-zero first p_vaddr", min_vaddr != 0 && span != 0) ||
        !setup("a 1 MiB reservation", r != NULL)) {
      return failures;
    }
    void* hv = open_ext("libdlext_vaddr.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    check("RESERVED_ADDRESS of a library whose first p_vaddr is not 0 loads", hv != NULL);
    check_loaded_at(hv, "libdlext_vaddr.so", r);
  }

  // --- a gap between segments stays PROT_NONE -------------------------------
  {
    size_t span = span_of("libdlext_gap.so", NULL);
    size_t size = span + 0x10000;
    uint8_t* r = reserve(size);
    if (!setup("a reservation for libdlext_gap.so", r != NULL && span != 0)) return failures;
    void* hg = open_ext("libdlext_gap.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, size, error,
                        sizeof(error));
    check("RESERVED_ADDRESS of a library with a 4 MiB gap between segments loads", hg != NULL);
    check_loaded_at(hg, "libdlext_gap.so", r);
    struct phdr_query q = query("libdlext_gap.so");
    int gaps = 0, gaps_ok = 1;
    for (int i = 1; i < q.segs; ++i) {
      if (q.seg[i][0] <= q.seg[i - 1][1]) continue;
      ++gaps;
      if (!range_is_prot_none(q.seg[i - 1][1], q.seg[i][0]) ||
          !range_is_taken(q.seg[i - 1][1], q.seg[i][0])) {
        gaps_ok = 0;
      }
    }
    check("  the pages between its segments are PROT_NONE and still mapped",
          gaps > 0 && gaps_ok);
    check("  the rest of the reservation is untouched", untouched(r + span, size - span));
  }

  // --- RESERVED_ADDRESS_HINT -------------------------------------------------
  small = reserve(g_page);
  if (!setup("a one-page reservation", small != NULL)) return failures;
  h = open_ext("libdlext_b.so", ANDROID_DLEXT_RESERVED_ADDRESS_HINT, small, g_page, error,
               sizeof(error));
  check("RESERVED_ADDRESS_HINT too small loads elsewhere",
        h != NULL && base_of(h) != NULL && base_of(h) != small && answer_of(h) == 42);
  check("  and leaves the reservation untouched", untouched(small, g_page));
  check("  which the caller can unmap", munmap(small, g_page) == 0);

  // --- a load that fails after mapping into the reservation -----------------
  uint8_t* res2 = reserve(big);
  if (!setup("a 1 MiB reservation", res2 != NULL)) return failures;
  h = open_ext("libdlext_tlsfail.so", ANDROID_DLEXT_RESERVED_ADDRESS, res2, big, error,
               sizeof(error));
  check("a load failing at relocation (undefined TLS) fails", h == NULL);
  check("  and the reservation is PROT_NONE again and still mapped", untouched(res2, big));
  // The same reservation, with a hint that fits: at its start.
  h = open_ext("libdlext_c.so", ANDROID_DLEXT_RESERVED_ADDRESS_HINT, res2, big, error,
               sizeof(error));
  check("RESERVED_ADDRESS_HINT that fits loads at the reservation's start",
        h != NULL && base_of(h) == res2 && answer_of(h) == 42);

  // --- a span reaching address space that is not mapped ---------------------
  {
    size_t span = span_of("libdlext_hole.so", NULL);
    size_t size = span + 0x20000;
    uint8_t* r = reserve(size);
    int ok = r != NULL && span != 0 && munmap(r, size) == 0 &&
             mmap(r, g_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == r;
    if (!setup("a 'reservation' of which only the first page is mapped", ok)) return failures;
    void* hh = open_ext("libdlext_hole.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, size, error,
                        sizeof(error));
    check("RESERVED_ADDRESS over pages nobody mapped fails before mapping anything",
          hh == NULL && strstr(error, "is not all mapped") != NULL);
    check("  the mapped page is untouched", untouched(r, g_page));
    check("  and nothing of the library is left in the rest",
          range_is_unmapped((uintptr_t)r + g_page, (uintptr_t)r + span));
    munmap(r, g_page);
    // A reservation whose tail the caller unmapped, inside one mapping: the
    // span is still the caller's address space, as on Linux.
    r = reserve(size);
    ok = r != NULL && munmap(r + span, size - span) == 0;
    if (!setup("a reservation with its tail unmapped", ok)) return failures;
    hh = open_ext("libdlext_hole.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, size, error,
                  sizeof(error));
    check("RESERVED_ADDRESS whose span is mapped, beyond it not, loads",
          hh != NULL && base_of(hh) == r && answer_of(hh) == 42);
  }

  // --- Chrome's pattern --------------------------------------------------------
  {
    const size_t size = 0xc000000;
    size_t span = span_of("libdlext_chrome.so", NULL);
    uint8_t* r = reserve(size);
    if (!setup("a 0xc000000 reservation", r != NULL && span != 0)) return failures;
    void* hc = open_ext("libdlext_chrome.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, size, error,
                        sizeof(error));
    check("Chrome: RESERVED_ADDRESS into a 0xc000000 reservation loads at its start",
          hc != NULL && base_of(hc) == r && memcmp(r, "\177ELF", 4) == 0);
    check("  the caller unmaps the reservation's tail beyond the span",
          munmap(r + span, size - span) == 0);
    void (*set)(int) = hc ? (void (*)(int))dlsym(hc, "gesher_dlext_set") : NULL;
    if (set != NULL) set(99);
    check("  then its code runs and its .data is written",
          set != NULL && answer_of(hc) == 100);
    check("  and the tail is free address space",
          range_is_free((uintptr_t)r + span, (uintptr_t)r + span + 16 * g_page));
  }

  // --- dlclose of a reserved library, then munmap of the reservation --------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* h1 = open_ext("libdlext_close.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    char path[600];
    path_of(path, sizeof(path), "libdlext_close.so");
    void* h2 = dlopen(path, RTLD_NOW);
    void* marker = dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker");
    check("a library loaded into a reservation, opened twice",
          h1 != NULL && h2 == h1 && base_of(h1) == r && marker != NULL);
    dlclose(h2);
    check("  the first dlclose keeps it loaded",
          query("libdlext_close.so").found && dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker") != NULL);
    dlclose(h1);
    check("  the last dlclose unloads it: dl_iterate_phdr no longer lists it",
          !query("libdlext_close.so").found);
    check("  dlsym(RTLD_DEFAULT) no longer finds its symbols",
          dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker") == NULL);
    check("  dlsym on its old handle finds nothing",
          dlsym(h1, "gesher_dlext_close_marker") == NULL);
    Dl_info info;
    check("  dladdr of its old address finds nothing", dladdr(marker, &info) == 0);
    check("  the reservation is PROT_NONE again and still mapped", untouched(r, big));
    check("  the caller unmaps the reservation", munmap(r, big) == 0);
    check("  after that dl_iterate_phdr and dlsym(RTLD_DEFAULT) still work and do not find it",
          !query("libdlext_close.so").found &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker") == NULL &&
              query("libdlext_a.so").found);
    void* h3 = dlopen(path, RTLD_NOW);
    check("  dlopen loads it again, elsewhere",
          h3 != NULL && answer_of(h3) == 42 &&
              ((uint8_t*)base_of(h3) < r || (uint8_t*)base_of(h3) >= r + big));
  }

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
