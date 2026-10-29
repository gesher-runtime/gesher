// Executes the exact production copy helper, not a separately mirrored copy.
#define WIN32_LEAN_AND_MEAN
#include "../../compat/posix/src/socket_user_copy.h"
#include <stdio.h>
#include <string.h>

int main() {
  SYSTEM_INFO info; GetSystemInfo(&info);
  SIZE_T page = info.dwPageSize;
  auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!memory) return 2;
  const char source[] = "abcdefgh";
  for (int fault = 0; fault < 7; ++fault) {
    DWORD old = 0;
    if (!VirtualProtect(memory, page * 2, PAGE_READWRITE, &old)) return 3;
    memset(memory, 0xa5, page * 2);
    void* destination = memory;
    if (fault == 1) destination = reinterpret_cast<void*>(uintptr_t{1});
    if (fault == 2) destination = nullptr;
    if (fault == 3 && !VirtualProtect(memory, page, PAGE_READONLY, &old)) return 4;
    if (fault == 4 && !VirtualProtect(memory, page, PAGE_NOACCESS, &old)) return 5;
    if (fault == 5) {
      destination = memory + page - 4;
      if (!VirtualProtect(memory + page, page, PAGE_NOACCESS, &old)) return 6;
    }
    if (fault == 6 && !VirtualProtect(memory, page, PAGE_READWRITE | PAGE_GUARD, &old)) return 9;
    bool okay = gesher::ws::usercopy::To(destination, source, 8);
    MEMORY_BASIC_INFORMATION observed = {};
    if (!VirtualQuery(memory, &observed, sizeof(observed))) return 10;
    bool guard = (observed.Protect & PAGE_GUARD) != 0;
    if (!VirtualProtect(memory, page * 2, PAGE_READWRITE, &old)) return 7;
    unsigned char* prefix = fault == 5 ? memory + page - 4 : memory;
    bool expected = (fault == 0 ? okay && memcmp(prefix, source, 8) == 0 :
                     fault == 5 ? !okay && memcmp(prefix, source, 4) == 0 && prefix[4] == 0xa5 :
                                  !okay && prefix[0] == 0xa5 && (fault != 6 || guard));
    printf("UDPGUARDED fault=%d success=%d guard=%d prefix=%02x%02x%02x%02x next=%02x verdict=%d\n",
           fault, okay, guard, prefix[0], prefix[1], prefix[2], prefix[3], prefix[4], expected);
    if (!expected) return 11;
  }
  char copy[8] = {};
  if (gesher::ws::usercopy::From(copy, reinterpret_cast<void*>(uintptr_t{1}), sizeof(copy)) ||
      !gesher::ws::usercopy::From(copy, source, sizeof(copy)) || memcmp(copy, source, 8) != 0) return 12;
  if (!VirtualFree(memory, 0, MEM_RELEASE)) return 8;
  puts("UDPGUARDED all actual shared-helper controls passed");
  return 0;
}
