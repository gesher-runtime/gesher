// Probe: can Windows user code set the FS base, and does it survive the things
// that preempt a thread? Android x86-64 .so code reaches thread-local state and
// the stack-protector cookie through %fs (bionic puts its TLS block there, the
// cookie at %fs:0x28). Windows uses %gs for the TEB and leaves %fs base 0, with
// no documented API to set it. x86-64 has RDFSBASE/WRFSBASE, usable from user
// mode only when CR4.FSGSBASE is enabled; the kernel must then also save and
// restore the base across context switches for them to be of any use.
//
// This probe decides the approach of docs/decisions/0014-native-libs-fs-base.md
// empirically on the CI runners, as probes/platform does for ART. It checks the
// base only with RDFSBASE (which touches no memory, so a lost base is detected,
// never crashed into); the single %fs *memory* load it does is guarded by first
// confirming the base is set. A #UD (feature off) is caught by a vectored
// handler. So the probe always prints a verdict and exits cleanly.

#include <windows.h>

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;
volatile bool g_ud = false;

void Report(const char* name, bool ok, const char* detail = "") {
  std::printf("%-46s %s%s%s\n", name, ok ? "ok" : "FAIL", *detail ? "  " : "", detail);
  if (!ok) ++failures;
}

LONG CALLBACK UdFilter(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
    g_ud = true;
    info->ContextRecord->Rip += 5;  // rd/wrfsbase r64 is 5 bytes
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void WriteFsBase(uintptr_t v) {
  __asm__ __volatile__("wrfsbase %0" : : "r"(v) : "memory");
}
inline uintptr_t ReadFsBase() {
  uintptr_t v = 0;
  __asm__ __volatile__("rdfsbase %0" : "=r"(v) : : "memory");
  return v;
}
inline uintptr_t LoadFs(uintptr_t offset) {
  uintptr_t v;
  __asm__ __volatile__("movq %%fs:(%1), %0" : "=r"(v) : "r"(offset));
  return v;
}

}  // namespace

int main() {
  std::printf("fsbase probe: can Windows user code use %%fs for bionic TLS?\n");

  PVOID handler = AddVectoredExceptionHandler(1, UdFilter);

  alignas(64) static uintptr_t block[16] = {0};
  block[0] = reinterpret_cast<uintptr_t>(&block[0]);  // bionic self pointer at %fs:0
  block[5] = 0xC0FFEE11;                               // %fs:0x28, the cookie slot

  const uintptr_t want = reinterpret_cast<uintptr_t>(block);
  g_ud = false;
  WriteFsBase(want);
  if (g_ud) {
    RemoveVectoredExceptionHandler(handler);
    Report("RDFSBASE/WRFSBASE available", false, "#UD: CR4.FSGSBASE off on this host");
    std::printf("\nverdict: %%fs cannot be set with WRFSBASE here; the loader must patch %%fs.\n");
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  Report("WRFSBASE executes (CR4.FSGSBASE on)", true);

  // Was the base actually set? RDFSBASE touches no memory, so this is safe even
  // if WRFSBASE did nothing.
  uintptr_t base = ReadFsBase();
  bool set_ok = (base == want);
  Report("RDFSBASE reads the base back immediately", set_ok);

  // Only load through %fs when the base is confirmed set (so the load is valid).
  if (set_ok) {
    Report("%fs:0x28 reads the cookie", LoadFs(0x28) == 0xC0FFEE11);
  }

  // The decisive test: does the base survive a context switch? Re-assert it,
  // force a kernel transition, and re-read the base (no memory load).
  WriteFsBase(want);
  Sleep(1);
  bool survived_sleep = (ReadFsBase() == want);
  Report("%fs base survives Sleep(1)", survived_sleep,
         survived_sleep ? "Windows preserves it" : "lost on kernel entry");

  WriteFsBase(want);
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  WaitForSingleObject(ev, 2);
  CloseHandle(ev);
  Report("%fs base survives a blocking wait", ReadFsBase() == want);

  WriteFsBase(0);  // leave it clean
  RemoveVectoredExceptionHandler(handler);

  if (failures == 0) {
    std::printf("\nfsbase probe: all passed; %%fs base is usable for bionic TLS\n");
  } else {
    std::printf(
        "\nverdict: the %%fs base is not usable across scheduling here; the loader must\n"
        "patch %%fs accesses instead (see docs/decisions/0014-native-libs-fs-base.md).\n"
        "%d check(s) failed\n",
        failures);
  }
  return failures == 0 ? 0 : 1;
}
