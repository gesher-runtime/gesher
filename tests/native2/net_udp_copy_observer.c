// Actual Windows API observation only; not a Gesher ABI success fixture.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  SYSTEM_INFO info; GetSystemInfo(&info);
  SIZE_T page = info.dwPageSize;
  unsigned char* memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!memory) return 2;
  const char source[] = "abcdefgh";
  for (int fault = 0; fault < 7; ++fault) {
    DWORD old = 0;
    if (!VirtualProtect(memory, page * 2, PAGE_READWRITE, &old)) return 3;
    memset(memory, 0xa5, page * 2);
    void* destination = memory;
    if (fault == 1) destination = (void*)(uintptr_t)1;
    if (fault == 2) destination = NULL;
    if (fault == 3 && !VirtualProtect(memory, page, PAGE_READONLY, &old)) return 4;
    if (fault == 4 && !VirtualProtect(memory, page, PAGE_NOACCESS, &old)) return 5;
    if (fault == 5) {
      destination = memory + page - 4;
      if (!VirtualProtect(memory + page, page, PAGE_NOACCESS, &old)) return 6;
    }
    if (fault == 6 && !VirtualProtect(memory, page, PAGE_READWRITE | PAGE_GUARD, &old)) return 9;
    SIZE_T copied = (SIZE_T)-1; SetLastError(0);
    BOOL okay = ReadProcessMemory(GetCurrentProcess(), source, destination, 8, &copied);
    DWORD error = GetLastError();
    MEMORY_BASIC_INFORMATION observed = {0};
    if (!VirtualQuery(memory, &observed, sizeof(observed))) return 10;
    if (!VirtualProtect(memory, page * 2, PAGE_READWRITE, &old)) return 7;
    unsigned char* prefix = fault == 5 ? memory + page - 4 : memory;
    printf("UDPCOPY fault=%d success=%d error=%lu copied=%zu protection=%lu prefix=%02x%02x%02x%02x next=%02x\n",
           fault, okay, error, copied, observed.Protect, prefix[0], prefix[1], prefix[2], prefix[3], prefix[4]);
  }
  return VirtualFree(memory, 0, MEM_RELEASE) ? 0 : 8;
}
