// Exercise a real protected-page read and verify the signal frame and recovery.
// On Windows a continuable exception also exercises the stack probe branch
// without requiring the test process to exhaust its own stack.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ucontext.h>

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

constexpr int kRecoveredValue = 42;
constexpr int kRegisterValue = 17;
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
std::atomic<uintptr_t> expected_address;
volatile sig_atomic_t stack_probe = 0;
volatile sig_atomic_t calls = 0;
volatile sig_atomic_t frame_matches = 0;

void Handle(int signo, siginfo_t* info, void* raw_context) {
  if (signo != SIGSEGV || info->si_signo != SIGSEGV || calls != 0) _Exit(2);
  calls = 1;
  auto& machine = static_cast<ucontext_t*>(raw_context)->uc_mcontext;
  const uintptr_t address = expected_address.load(std::memory_order_relaxed);
#if defined(__x86_64__)
  const bool address_matches = machine.gregs[REG_TRAPNO] == 14 &&
      static_cast<uintptr_t>(machine.gregs[REG_CR2]) == address;
  const bool registers_match = machine.gregs[REG_RSP] != 0 &&
      machine.gregs[REG_RIP] != 0 &&
      (stack_probe || machine.gregs[REG_RAX] == kRegisterValue);
  if (!stack_probe) {
    machine.gregs[REG_RIP] += 2;  // movl (%rdi), %eax in ReadProtectedPage.
    machine.gregs[REG_RAX] = kRecoveredValue;
  }
#elif defined(__aarch64__)
  const bool address_matches = machine.fault_address == address;
  const bool registers_match = machine.sp != 0 && machine.pc != 0 &&
      (stack_probe || machine.regs[0] == kRegisterValue);
  if (!stack_probe) {
    machine.pc += 4;  // ldr w0, [...] in ReadProtectedPage.
    machine.regs[0] = kRecoveredValue;
  }
#else
#error Unsupported fault context architecture
#endif
  frame_matches = info->si_code == SEGV_ACCERR &&
      reinterpret_cast<uintptr_t>(info->si_addr) == address && address_matches && registers_match;
}

int ReadProtectedPage(const void* page) {
#if defined(__x86_64__)
  int value = kRegisterValue;
  asm volatile("movl (%%rdi), %%eax" : "+a"(value) : "D"(page) : "memory");
#elif defined(__aarch64__)
  register int value asm("w0") = kRegisterValue;
  asm volatile("ldr %w0, [%1]" : "+&r"(value) : "r"(page) : "memory");
#endif
  return value;
}

}  // namespace

int main() {
  struct sigaction action {};
  action.sa_sigaction = Handle;
  action.sa_flags = SA_SIGINFO;
  if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGSEGV, &action, nullptr) != 0) return 3;
#ifdef _WIN32
  void* page = VirtualAlloc(nullptr, 1, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
  if (page == nullptr) return 4;
#else
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return 4;
  void* page = mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) return 4;
#endif
  expected_address.store(reinterpret_cast<uintptr_t>(page), std::memory_order_relaxed);
  const int result = ReadProtectedPage(page);
#ifdef _WIN32
  if (!VirtualFree(page, 0, MEM_RELEASE)) return 5;
#else
  if (munmap(page, page_size) != 0) return 5;
#endif
  if (result != kRecoveredValue || calls != 1 || !frame_matches) return 6;
  puts("fault context: protected read delivered and recovered");

#ifdef _WIN32
  ULONG_PTR low;
  ULONG_PTR high;
  GetCurrentThreadStackLimits(&low, &high);
  constexpr ULONG_PTR kProbeDistance = 64 * 1024;
  ULONG_PTR parameters[] = {0, reinterpret_cast<ULONG_PTR>(&low) - kProbeDistance};
  if (parameters[1] <= low || parameters[1] >= high) return 7;
  stack_probe = 1;
  calls = 0;
  frame_matches = 0;
  expected_address.store(parameters[1], std::memory_order_relaxed);
  RaiseException(EXCEPTION_STACK_OVERFLOW, 0, 2, parameters);
  if (calls != 1 || !frame_matches) return 8;
  puts("fault context: stack probe address retained in signal and machine frames");
#endif
  return 0;
}
