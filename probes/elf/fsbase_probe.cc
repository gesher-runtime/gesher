// Probe: can Windows user code set the FS base, and does it survive the things
// that preempt a thread? Android x86-64 .so code reads thread-local state and
// the stack-protector cookie through %fs (bionic puts its TLS block there, the
// cookie at %fs:0x28). Windows uses %gs for the TEB and leaves %fs base 0, with
// no documented API to set it. x86-64 has RDFSBASE/WRFSBASE, usable from user
// mode only when CR4.FSGSBASE is enabled; the kernel must then also save and
// restore the base across context switches for them to be of any use.
//
// This probe decides the approach of docs/decisions/0014-native-libs-fs-base.md
// empirically on the CI runners, as probes/platform does for ART's primitives:
//   1. Is WRFSBASE/RDFSBASE available (no #UD)?
//   2. After WRFSBASE, does a %fs-relative load read what we wrote?
//   3. Does the base survive a Sleep (a context switch), a blocking wait, and
//      an APC, i.e. does Windows preserve it?
//   4. Does each thread keep its own base?
//
// Exit 0 and "all passed" if %fs is usable this way; otherwise it reports which
// step failed, and the loader must fall back to patching %fs accesses instead.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void Report(const char* name, bool ok, const char* detail = "") {
  std::printf("%-40s %s%s%s\n", name, ok ? "ok" : "FAIL", *detail ? "  " : "", detail);
  if (!ok) ++failures;
}

// RDFSBASE/WRFSBASE under SEH, so a #UD (feature off) is caught, not fatal.
bool g_wrfsbase_faulted = false;

LONG CALLBACK IllegalInstructionFilter(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {
    g_wrfsbase_faulted = true;
    // Skip the faulting instruction. wrfsbase %rax is 3 bytes (F3 48 0F AE D0),
    // rdfsbase %rax is 5 (F3 48 0F AE C0); we only need not to loop forever,
    // and both probes set the flag, so advance past the longest.
    info->ContextRecord->Rip += 5;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

inline void WriteFsBase(uintptr_t v) {
  __asm__ __volatile__("wrfsbase %0" : : "r"(v) : "memory");
}

inline uintptr_t ReadFsBase() {
  uintptr_t v;
  __asm__ __volatile__("rdfsbase %0" : "=r"(v) : : "memory");
  return v;
}

// A load at %fs:offset.
inline uintptr_t LoadFs(uintptr_t offset) {
  uintptr_t v;
  __asm__ __volatile__("movq %%fs:(%1), %0" : "=r"(v) : "r"(offset));
  return v;
}

bool TryInstructions() {
  g_wrfsbase_faulted = false;
  PVOID handler = AddVectoredExceptionHandler(1, IllegalInstructionFilter);
  volatile uintptr_t block[8] = {0};
  WriteFsBase(reinterpret_cast<uintptr_t>(block));
  if (!g_wrfsbase_faulted) {
    (void)ReadFsBase();
  }
  RemoveVectoredExceptionHandler(handler);
  return !g_wrfsbase_faulted;
}

}  // namespace

int main() {
  std::printf("fsbase probe: can Windows user code use %%fs for bionic TLS?\n");

  if (!TryInstructions()) {
    Report("RDFSBASE/WRFSBASE available", false, "#UD: CR4.FSGSBASE off on this host");
    std::printf("\n%d check(s) failed; %%fs must be patched, not set\n", failures);
    return failures == 0 ? 0 : 1;
  }
  Report("RDFSBASE/WRFSBASE available", true);

  // A block whose words at a few offsets we can recognise through %fs.
  alignas(64) uintptr_t block[16] = {0};
  block[0] = reinterpret_cast<uintptr_t>(&block[0]);  // bionic's self pointer at %fs:0
  block[5] = 0xC0FFEE11;                               // %fs:0x28, the cookie slot
  WriteFsBase(reinterpret_cast<uintptr_t>(block));

  Report("rdfsbase reads back the base",
         ReadFsBase() == reinterpret_cast<uintptr_t>(block));
  Report("%fs:0 reads the block", LoadFs(0) == reinterpret_cast<uintptr_t>(&block[0]));
  Report("%fs:0x28 reads the cookie", LoadFs(0x28) == 0xC0FFEE11);

  // Survives a Sleep (yields and is very likely rescheduled).
  Sleep(1);
  Report("base survives Sleep(1)", ReadFsBase() == reinterpret_cast<uintptr_t>(block),
         "Windows preserves %fs base across a context switch");

  // Survives a blocking kernel wait.
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  WaitForSingleObject(ev, 2);
  CloseHandle(ev);
  Report("base survives a blocking wait", ReadFsBase() == reinterpret_cast<uintptr_t>(block));

  // Survives an APC delivered to this thread.
  static std::atomic<bool> apc_ran{false};
  QueueUserAPC([](ULONG_PTR) { apc_ran.store(true); }, GetCurrentThread(), 0);
  SleepEx(5, TRUE);
  Report("base survives an APC",
         apc_ran.load() && ReadFsBase() == reinterpret_cast<uintptr_t>(block));

  // Each thread keeps its own base; the main thread's is unchanged after.
  std::atomic<int> thread_failures{0};
  std::thread t([&] {
    if (!TryInstructions()) {  // re-arm per thread is unnecessary, but harmless
    }
    alignas(64) uintptr_t other[16] = {0};
    other[0] = 0xABCDEF;
    WriteFsBase(reinterpret_cast<uintptr_t>(other));
    Sleep(2);
    if (ReadFsBase() != reinterpret_cast<uintptr_t>(other)) ++thread_failures;
    if (LoadFs(0) != 0xABCDEF) ++thread_failures;
  });
  t.join();
  Report("another thread keeps its own base", thread_failures.load() == 0);
  Report("this thread's base unchanged by the other",
         ReadFsBase() == reinterpret_cast<uintptr_t>(block));

  // Leave %fs base at 0 again: the CRT and Windows do not expect it set.
  WriteFsBase(0);

  if (failures == 0) {
    std::printf("\nfsbase probe: all passed; %%fs base is usable for bionic TLS\n");
  } else {
    std::printf("\n%d check(s) failed; the loader must patch %%fs accesses instead\n", failures);
  }
  return failures == 0 ? 0 : 1;
}
