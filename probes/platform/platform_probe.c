// Gesher platform probe.
//
// ART relies on a handful of Linux kernel behaviours. Before porting it we
// verify that each one has a working Windows equivalent. Every probe prints
// PASS/FAIL; the process exit code is the number of failed probes.
//
//   npe        implicit null checks: fault on a near-null load, resume at a
//              landing address (ART: SIGSEGV handler -> NullPointerException)
//   stack      stack overflow detection with a guaranteed reserve to recover
//              (ART: guard page + SIGSEGV -> StackOverflowError)
//   futex      wait/wake on an address (ART: futex() for locks and monitors)
//   lowmem     reserve heap below 4 GiB (ART: 32-bit compressed references)
//   filemap    map a file at a chosen low address (ART: boot image / .oat)
//   jit        write code, flip to executable, run it (ART: JIT code cache)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void report(const char *name, int ok, const char *detail) {
  printf("%-8s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
  if (!ok) failures++;
}

// --- npe -------------------------------------------------------------------

extern char gesher_npe_resume[];
static volatile LONG npe_hits;

__attribute__((noinline)) static int64_t npe_load(int64_t *p) {
  int64_t v = 7;
  __asm__ volatile("movq 8(%1), %0\n\t"
                   ".globl gesher_npe_resume\n"
                   "gesher_npe_resume:\n\t"
                   : "+r"(v)
                   : "r"(p)
                   : "memory");
  return v;
}

static LONG CALLBACK fault_handler(EXCEPTION_POINTERS *ep) {
  EXCEPTION_RECORD *er = ep->ExceptionRecord;
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      er->NumberParameters >= 2 && er->ExceptionInformation[1] < 0x10000) {
    InterlockedIncrement(&npe_hits);
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)gesher_npe_resume;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

static void probe_npe(void) {
  int rounds = 1000, ok = 1;
  for (int i = 0; i < rounds; i++)
    if (npe_load(NULL) != 7) ok = 0;
  char buf[64];
  snprintf(buf, sizeof buf, "%ld/%d faults recovered", npe_hits, rounds);
  report("npe", ok && npe_hits == rounds, buf);
}

// --- stack -----------------------------------------------------------------

static volatile LONG overflow_seen;

static void overflow_exit(void) { ExitThread(42); }

static LONG CALLBACK overflow_handler(EXCEPTION_POINTERS *ep) {
  if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
    return EXCEPTION_CONTINUE_SEARCH;
  InterlockedExchange(&overflow_seen, 1);
  CONTEXT *c = ep->ContextRecord;
  c->Rsp = (c->Rsp & ~(DWORD64)0xF) - 8;  // ABI alignment at function entry
  c->Rip = (DWORD64)(uintptr_t)overflow_exit;
  return EXCEPTION_CONTINUE_EXECUTION;
}

__attribute__((noinline)) static int recurse(volatile char *prev, int depth) {
  volatile char frame[512];
  frame[0] = prev ? prev[0] + 1 : 0;
  return depth > (1 << 30) ? 0 : recurse(frame, depth + 1) + frame[1];
}

static DWORD WINAPI overflow_thread(void *arg) {
  (void)arg;
  ULONG reserve = 64 * 1024;
  SetThreadStackGuarantee(&reserve);
  recurse(NULL, 0);
  return 0;
}

static void probe_stack(void) {
  HANDLE t = CreateThread(NULL, 1 << 20, overflow_thread, NULL,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  DWORD code = 0;
  WaitForSingleObject(t, 10000);
  GetExitCodeThread(t, &code);
  CloseHandle(t);
  char buf[64];
  snprintf(buf, sizeof buf, "overflow caught=%ld thread exit=%lu",
           overflow_seen, code);
  report("stack", overflow_seen && code == 42, buf);
}

// --- futex -----------------------------------------------------------------

static volatile LONG turn;
enum { PINGPONG = 20000 };

static DWORD WINAPI pong(void *arg) {
  (void)arg;
  for (int i = 0; i < PINGPONG; i++) {
    LONG expect = 0;
    while (turn == 0) WaitOnAddress(&turn, &expect, sizeof turn, INFINITE);
    InterlockedExchange(&turn, 0);
    WakeByAddressSingle((void *)&turn);
  }
  return 0;
}

static void probe_futex(void) {
  HANDLE t = CreateThread(NULL, 0, pong, NULL, 0, NULL);
  ULONGLONG start = GetTickCount64();
  for (int i = 0; i < PINGPONG; i++) {
    InterlockedExchange(&turn, 1);
    WakeByAddressSingle((void *)&turn);
    LONG expect = 1;
    while (turn == 1) WaitOnAddress(&turn, &expect, sizeof turn, INFINITE);
  }
  DWORD w = WaitForSingleObject(t, 10000);
  CloseHandle(t);
  char buf[64];
  snprintf(buf, sizeof buf, "%d round trips in %llu ms", PINGPONG,
           GetTickCount64() - start);
  report("futex", w == WAIT_OBJECT_0, buf);
}

// --- lowmem ----------------------------------------------------------------

static void *reserve_low(SIZE_T size) {
  for (uintptr_t a = 0x10000000; a + size <= 0x100000000ull; a += 0x4000000) {
    void *p = VirtualAlloc((void *)a, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p) return p;
  }
  return NULL;
}

static void probe_lowmem(void) {
  SIZE_T size = 512u << 20;
  char *base = reserve_low(size);
  int ok = base && (uintptr_t)base + size <= 0x100000000ull;
  if (ok) {
    char *page = VirtualAlloc(base + (64u << 20), 1 << 20, MEM_COMMIT,
                              PAGE_READWRITE);
    ok = page != NULL;
    if (ok) page[12345] = 1;
  }
  char buf[64];
  snprintf(buf, sizeof buf, "512 MiB reserved at %p", (void *)base);
  report("lowmem", ok, buf);
  if (base) VirtualFree(base, 0, MEM_RELEASE);
}

// --- filemap ---------------------------------------------------------------

static void probe_filemap(void) {
  char path[MAX_PATH], dir[MAX_PATH];
  GetTempPathA(sizeof dir, dir);
  GetTempFileNameA(dir, "gsh", 0, path);
  HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                         NULL);
  const char magic[] = "art\n\0gesher-image";
  DWORD written = 0;
  WriteFile(f, magic, sizeof magic, &written, NULL);
  SetFilePointer(f, 1 << 20, NULL, FILE_BEGIN);
  SetEndOfFile(f);
  HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);

  // Find a free low address, release it, then map the view exactly there.
  void *want = reserve_low(1 << 20);
  if (want) VirtualFree(want, 0, MEM_RELEASE);
  void *view = m ? MapViewOfFileEx(m, FILE_MAP_READ, 0, 0, 0, want) : NULL;
  int ok = view && view == want && memcmp(view, magic, sizeof magic) == 0;
  char buf[64];
  snprintf(buf, sizeof buf, "view at %p (wanted %p)", view, want);
  report("filemap", ok, buf);
  if (view) UnmapViewOfFile(view);
  if (m) CloseHandle(m);
  CloseHandle(f);
}

// --- jit -------------------------------------------------------------------

static void probe_jit(void) {
  static const unsigned char code[] = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
  unsigned char *mem =
      VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  memcpy(mem, code, sizeof code);
  DWORD old;
  int ok = VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), mem, sizeof code);
  int r = ok ? ((int (*)(void))(uintptr_t)mem)() : -1;
  char buf[64];
  snprintf(buf, sizeof buf, "generated code returned %d", r);
  report("jit", r == 42, buf);
  VirtualFree(mem, 0, MEM_RELEASE);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  void *h1 = AddVectoredExceptionHandler(1, fault_handler);
  void *h2 = AddVectoredExceptionHandler(1, overflow_handler);
  probe_npe();
  probe_stack();
  probe_futex();
  probe_lowmem();
  probe_filemap();
  probe_jit();
  RemoveVectoredExceptionHandler(h2);
  RemoveVectoredExceptionHandler(h1);
  printf("\n%d probe(s) failed\n", failures);
  return failures;
}
