// This executable never calls syscall directly. The module import library must
// precede gesher_posix, reproducing the link order that masked the missing owner.
#include <windows.h>
#include <linux/futex.h>
#include <time.h>
#include <cstdio>
extern "C" __declspec(dllimport) long futex_module_call(
    volatile int*, int, int, const timespec*, volatile int*, int);
int main() {
  volatile int word = 0;
  bool exported = GetProcAddress(GetModuleHandleW(nullptr), "gesher_futex_owner_v1") != nullptr;
  long result = futex_module_call(&word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
  if (!exported || result != 0) {
    std::printf("FAIL futex owner: exported=%d DLL wake=%ld\n", exported, result);
    return 1;
  }
  std::puts("futex_owner_link_test all passed");
  return 0;
}
