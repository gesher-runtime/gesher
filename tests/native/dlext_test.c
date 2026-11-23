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
//   - dlclose keeps a reserved library loaded, as every library in gesher;
//     the caller may then unmap its reservation (or map over it), and the
//     library leaves dl_iterate_phdr, dladdr and dlsym without running its
//     code - only when the munmap/mmap succeeds; its TLS keeps initializing
//     new threads; an iteration in progress skips it;
//   - every mapping of a load must replace one mapping of the caller's: a
//     segment crossing from one of the caller's mappings into another fails
//     before anything is mapped;
//   - FORCE_LOAD with a bare name returns the loaded library (bionic finds it
//     by soname first), with a path it fails (no second copy).
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
#include <pthread.h>
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

// A reservation of two adjacent mappings, [r, r + first) and [r + first,
// r + first + second), each made on its own (sizes multiples of 64 KiB).
static uint8_t* two_mappings(size_t first, size_t second) {
  void* r = mmap(NULL, first + second, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (r == MAP_FAILED || munmap(r, first + second) != 0) return NULL;
  uint8_t* p = (uint8_t*)r;
  if (mmap(p, first, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != p) return NULL;
  if (mmap(p + first, second, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) !=
      p + first) {
    return NULL;
  }
  return p;
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

// A thread that registers a thread_local destructor of a library, then waits.
struct dtor_thread {
  int (*reg)(int*);
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int registered, release, rc;
  volatile int flag;
};
static void* DtorThread(void* arg) {
  struct dtor_thread* t = (struct dtor_thread*)arg;
  int rc = t->reg((int*)&t->flag);
  pthread_mutex_lock(&t->mu);
  t->rc = rc;
  t->registered = 1;
  pthread_cond_broadcast(&t->cv);
  while (!t->release) pthread_cond_wait(&t->cv, &t->mu);
  pthread_mutex_unlock(&t->mu);
  return NULL;
}

// A thread calling dl_iterate_phdr until told to stop, reading every
// program header it is given - those of libdlext_churn.so for a while, so
// the other thread's munmap falls inside such a callback.
struct iterate_thread {
  int stop;
  int iterations;
  unsigned long sum;
};
static int ReadPhdrs(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  struct iterate_thread* t = (struct iterate_thread*)data;
  const char* n = info->dlpi_name ? info->dlpi_name : "";
  size_t ln = strlen(n);
  int rounds = ln >= 17 && strcmp(n + ln - 17, "libdlext_churn.so") == 0 ? 20000 : 1;
  for (int r = 0; r < rounds; ++r) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      t->sum += ((volatile const ElfW(Phdr)*)info->dlpi_phdr)[i].p_type +
                ((volatile const ElfW(Phdr)*)info->dlpi_phdr)[i].p_memsz;
    }
  }
  return 0;
}
static void* IterateThread(void* arg) {
  struct iterate_thread* t = (struct iterate_thread*)arg;
  while (!__atomic_load_n(&t->stop, __ATOMIC_SEQ_CST)) {
    dl_iterate_phdr(ReadPhdrs, t);
    __atomic_add_fetch(&t->iterations, 1, __ATOMIC_SEQ_CST);
  }
  return NULL;
}

// A thread that only runs.
static void* PlainThread(void* arg) {
  *(int*)arg = 1;
  return NULL;
}

// dl_iterate_phdr: the first callback unmaps a reservation; later ones note
// whether the library that was in it is still passed.
struct unmap_in_callback {
  uint8_t* res;
  size_t size;
  int unmapped, calls, seen_after;
};
static int UnmapInCallback(struct dl_phdr_info* info, size_t size, void* data) {
  (void)size;
  struct unmap_in_callback* u = (struct unmap_in_callback*)data;
  ++u->calls;
  if (!u->unmapped) {
    u->unmapped = munmap(u->res, u->size) == 0;
    return 0;
  }
  const char* n = info->dlpi_name ? info->dlpi_name : "";
  size_t ln = strlen(n);
  if (ln >= 17 && strcmp(n + ln - 17, "libdlext_churn.so") == 0) ++u->seen_after;
  return 0;
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
    check("FORCE_LOAD of a loaded library by path fails (no second copy), not the first copy",
          again == NULL && strstr(error, "not supported") != NULL);
    {
      android_dlextinfo info;
      memset(&info, 0, sizeof(info));
      info.flags = ANDROID_DLEXT_FORCE_LOAD;
      dlerror();
      again = android_dlopen_ext("libdlext_a.so", RTLD_NOW, &info);
      check("FORCE_LOAD of a loaded library by bare name returns it (found by soname first)",
            again == h && h != NULL);
    }
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
          hh == NULL && strstr(error, "is not one mapping of the caller's") != NULL);
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

  // --- dlclose keeps the library loaded; munmap after it retires it ----------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hc = open_ext("libdlext_close.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    int (*marker)(void) = hc ? (int (*)(void))dlsym(hc, "gesher_dlext_close_marker") : NULL;
    if (!setup("a library loaded into a reservation", hc != NULL && marker != NULL)) {
      return failures;
    }
    check("dlclose of a library in a reservation returns 0", dlclose(hc) == 0);
    check("  it stays loaded and works (gesher never unloads)",
          query("libdlext_close.so").found && marker() == 7 && answer_of(hc) == 42 &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker") == (void*)marker);
    check("  the caller then unmaps its reservation", munmap(r, big) == 0);
    check("  the library leaves dl_iterate_phdr", !query("libdlext_close.so").found);
    check("  and dlsym, by handle and by RTLD_DEFAULT",
          dlsym(hc, "gesher_dlext_close_marker") == NULL &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_close_marker") == NULL);
    Dl_info info;
    check("  and dladdr of its old address finds nothing", dladdr((void*)marker, &info) == 0);
    check("  other libraries are still listed", query("libdlext_a.so").found);
    char path[600];
    path_of(path, sizeof(path), "libdlext_close.so");
    void* again = dlopen(path, RTLD_NOW);
    // A new copy, wherever mmap puts it (possibly in the freed range).
    check("  dlopen loads it again as a new library",
          again != NULL && again != hc && answer_of(again) == 42 &&
              query("libdlext_close.so").found);
  }

  // --- a binding through the global scope, then dlclose ----------------------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hb = open_ext("libdlext_bind.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    char path[600];
    path_of(path, sizeof(path), "libdlext_user.so");
    void* hu = dlopen(path, RTLD_NOW);
    int (*call)(void) = hu ? (int (*)(void))dlsym(hu, "gesher_dlext_call_bound") : NULL;
    if (!setup("libdlext_user.so binds gesher_dlext_bind_marker to the reserved library",
               hb != NULL && call != NULL && call() == 7)) {
      return failures;
    }
    dlclose(hb);
    check("another library's call bound through the global scope works after dlclose",
          call() == 7 && query("libdlext_bind.so").found);
  }

  // --- munmap or MAP_FIXED that fails leaves the library listed --------------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hm = open_ext("libdlext_unmap.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    if (!setup("a library loaded into it", hm != NULL && base_of(hm) == r)) return failures;
    errno = 0;
    int rc = munmap(r + 1, big - g_page);
    check("munmap at an address not page aligned fails (EINVAL)", rc == -1 && errno == EINVAL);
    check("  and the library is still listed and works",
          query("libdlext_unmap.so").found && answer_of(hm) == 42);
    errno = 0;
    void* m = mmap(r, g_page, PROT_READ, MAP_PRIVATE | MAP_FIXED, 12345, 0);
    check("MAP_FIXED of a file with a bad descriptor fails (EBADF)",
          m == MAP_FAILED && errno == EBADF);
    check("  and the library is still listed and works",
          query("libdlext_unmap.so").found && answer_of(hm) == 42 &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_unmap_marker") != NULL);
    // Never closed: unmapping its reservation retires it all the same.
    check("munmap of a reservation holding a library never closed", munmap(r, big) == 0);
    check("  takes it out of dl_iterate_phdr and dlsym",
          !query("libdlext_unmap.so").found && dlsym(hm, "gesher_dlext_unmap_marker") == NULL &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_unmap_marker") == NULL);
  }

  // --- MAP_FIXED over the reservation retires it too ------------------------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hd = open_ext("libdlext_default.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    if (!setup("a library loaded into it", hd != NULL && base_of(hd) == r)) return failures;
    dlclose(hd);
    check("MAP_FIXED anonymous memory over the reservation after dlclose",
          mmap(r, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
              r);
    check("  takes the library out of dl_iterate_phdr and dlsym",
          !query("libdlext_default.so").found &&
              dlsym(RTLD_DEFAULT, "gesher_dlext_default_marker") == NULL);
    munmap(r, big);
  }

  // --- a TLS library whose reservation is unmapped; then a new thread -------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* ht = open_ext("libdlext_tls.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    int (*get)(void) = ht ? (int (*)(void))dlsym(ht, "gesher_dlext_tls_get") : NULL;
    check("a library with thread-locals loads into a reservation",
          ht != NULL && base_of(ht) == r && get != NULL && get() == 5);
    dlclose(ht);
    check("  its reservation is unmapped after dlclose", munmap(r, big) == 0);
    pthread_t thread;
    int value = 0;
    int ok = pthread_create(&thread, NULL, PlainThread, &value) == 0;
    if (ok) pthread_join(thread, NULL);
    check("  a thread created after that starts and runs", ok && value == 1);
  }

  // --- a pending thread_local destructor, dlclose, then the thread ends -----
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* ht = open_ext("libdlext_tdtor.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    int (*reg)(int*) = ht ? (int (*)(int*))dlsym(ht, "gesher_dlext_thread_dtor") : NULL;
    struct dtor_thread t;
    memset(&t, 0, sizeof(t));
    t.reg = reg;
    pthread_mutex_init(&t.mu, NULL);
    pthread_cond_init(&t.cv, NULL);
    pthread_t thread;
    int started = reg != NULL && pthread_create(&thread, NULL, DtorThread, &t) == 0;
    if (started) {
      pthread_mutex_lock(&t.mu);
      while (!t.registered) pthread_cond_wait(&t.cv, &t.mu);
      pthread_mutex_unlock(&t.mu);
    }
    if (!setup("a thread registered a destructor of the reserved library", started && t.rc == 0)) {
      return failures;
    }
    dlclose(ht);
    pthread_mutex_lock(&t.mu);
    t.release = 1;
    pthread_cond_broadcast(&t.cv);
    pthread_mutex_unlock(&t.mu);
    pthread_join(thread, NULL);
    check("a thread_local destructor of a dlclosed reserved library runs at thread exit",
          t.flag == 1 && query("libdlext_tdtor.so").found);
  }

  // --- dlclose + munmap while other threads iterate --------------------------
  {
    struct iterate_thread it;
    memset(&it, 0, sizeof(it));
    pthread_t threads[3];
    int started = 0;
    for (int i = 0; i < 3; ++i) started += pthread_create(&threads[i], NULL, IterateThread, &it) == 0;
    if (!setup("threads calling dl_iterate_phdr in a loop", started == 3)) return failures;
    int loads = 0, placed = 0, retired = 0;
    char path[600];
    path_of(path, sizeof(path), "libdlext_churn.so");
    for (int i = 0; i < 40; ++i) {
      uint8_t* r = reserve(big);
      if (r == NULL) continue;
      void* hc = open_ext("libdlext_churn.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                          sizeof(error));
      if (hc != NULL) {
        ++loads;
        if (base_of(hc) == r && answer_of(hc) == 42) ++placed;
        dlclose(hc);
      }
      munmap(r, big);
      if (!query("libdlext_churn.so").found) ++retired;
    }
    __atomic_store_n(&it.stop, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 3; ++i) pthread_join(threads[i], NULL);
    char line[200];
    snprintf(line, sizeof(line),
             "40 load/dlclose/munmap of a reserved library while 3 threads iterate (%d iterations)",
             it.iterations);
    check(line, loads == 40 && placed == 40 && it.iterations > 0);
    check("  each munmap took the library out of dl_iterate_phdr", retired == 40);
  }

  // --- dl_iterate_phdr skips a library unmapped during the iteration --------
  {
    uint8_t* r = reserve(big);
    if (!setup("a 1 MiB reservation", r != NULL)) return failures;
    void* hi = open_ext("libdlext_churn.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, big, error,
                        sizeof(error));
    if (!setup("a library loaded into it", hi != NULL && base_of(hi) == r)) return failures;
    struct unmap_in_callback u = {r, big, 0, 0, 0};
    dl_iterate_phdr(UnmapInCallback, &u);
    check("a callback that unmaps a reservation: later callbacks skip its library",
          u.unmapped && u.calls > 1 && u.seen_after == 0);
  }

  // --- the caller's reservation made of two mappings ------------------------
  {
    // Two adjacent mappings of the caller's, the boundary 64 KiB in.
    const size_t first = 0x10000;
    size_t span = span_of("libdlext_cross.so", NULL);
    size_t second = (span + 0x10000 + 0xffff) & ~(size_t)0xffff;
    uint8_t* r = two_mappings(first, second);
    if (!setup("a reservation of two adjacent mappings", r != NULL && span > first)) {
      return failures;
    }
    // Its last segment (a 64 KiB .bss) starts before the boundary and ends
    // after it.
    void* hx = open_ext("libdlext_cross.so", ANDROID_DLEXT_RESERVED_ADDRESS, r, first + second,
                        error, sizeof(error));
    check("a segment crossing from one of the caller's mappings into the next fails",
          hx == NULL && strstr(error, "is not one mapping of the caller's") != NULL);
    check("  before mapping anything: both mappings untouched, nothing loaded",
          untouched(r, first + second) && !query("libdlext_cross.so").found);
    munmap(r, first + second);
    // Segments on either side of the boundary, none across it: each mapping
    // of a file-view load replaces pages of one mapping, and it loads; a
    // copied load is one mapping over the whole span, and fails the same way.
    span = span_of("libdlext_split.so", NULL);
    r = two_mappings(first, second);
    if (!setup("a reservation of two adjacent mappings", r != NULL && span != 0)) {
      return failures;
    }
    uint8_t* at = r + first - 0x6000;
    void* hs = open_ext("libdlext_split.so", ANDROID_DLEXT_RESERVED_ADDRESS, at,
                        first + second - (size_t)(at - r), error, sizeof(error));
    if (hs != NULL) {
      check("segments on both sides of the boundary, none across it: loaded at the address",
            base_of(hs) == at && answer_of(hs) == 42 && untouched(r, (size_t)(at - r)));
    } else {
      check("segments on both sides of the boundary, copied as one mapping: fails cleanly",
            strstr(error, "is not one mapping of the caller's") != NULL &&
                untouched(r, first + second) && !query("libdlext_split.so").found);
    }
  }

  // --- a load anywhere with a gap between segments --------------------------
  {
    char path[600];
    path_of(path, sizeof(path), "libdlext_gap2.so");
    void* hg = dlopen(path, RTLD_NOW);
    struct phdr_query q = query("libdlext_gap2.so");
    int gaps = 0, gaps_ok = 1;
    for (int i = 1; i < q.segs; ++i) {
      if (q.seg[i][0] <= q.seg[i - 1][1]) continue;
      ++gaps;
      if (!range_is_prot_none(q.seg[i - 1][1], q.seg[i][0]) ||
          !range_is_taken(q.seg[i - 1][1], q.seg[i][0])) {
        gaps_ok = 0;
      }
    }
    check("dlopen (no reservation) of a library with a gap: it works",
          hg != NULL && answer_of(hg) == 42);
    check("  and the pages between its segments are PROT_NONE and mapped", gaps > 0 && gaps_ok);
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
