// Executes the exact production copy helper, not a separately mirrored copy.
#define WIN32_LEAN_AND_MEAN
#include "../../compat/posix/src/socket_user_copy.h"
#include <stdio.h>
#include <string.h>
#include <atomic>

struct Race {
  void* page;
  SIZE_T size;
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::atomic<unsigned> changes{0};
};
static DWORD WINAPI ChangeProtection(void* raw) {
  auto& race = *static_cast<Race*>(raw);
  const DWORD modes[] = {PAGE_READONLY, PAGE_READWRITE | PAGE_GUARD, PAGE_READWRITE};
  ULONGLONG deadline = GetTickCount64() + 2000;
  unsigned i = 0;
  while (!race.stop.load() && GetTickCount64() < deadline) {
    DWORD old = 0;
    if (!VirtualProtect(race.page, race.size, modes[i++ % 3], &old)) { race.failed.store(true); break; }
    race.changes.fetch_add(1);
  }
  return 0;
}

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
  Race race{memory, page};
  HANDLE changer = CreateThread(nullptr, 0, ChangeProtection, &race, 0, nullptr);
  if (changer == nullptr) return 13;
  ULONGLONG ready_deadline = GetTickCount64() + 5000;
  while (race.changes.load() == 0 && GetTickCount64() < ready_deadline) Sleep(1);
  unsigned before = race.changes.load();
  for (unsigned i = 0; i < 20000; ++i) (void)gesher::ws::usercopy::To(memory, source, 8);
  unsigned after = race.changes.load();
  race.stop.store(true);
  if (WaitForSingleObject(changer, 5000) != WAIT_OBJECT_0) {
    // Never free/unwind storage while the owned target might access it.
    TerminateProcess(GetCurrentProcess(), 14); return 14;
  }
  CloseHandle(changer);
  DWORD old = 0;
  if (race.failed.load() || before == 0 || after <= before ||
      !VirtualProtect(memory, page * 2, PAGE_READWRITE, &old)) return 15;
  printf("UDPGUARDED protection-race calls=20000 changes=%u target-joined=1 no-process-fault=1\n", after - before);
  // Guard state during concurrent changes has no stable snapshot guarantee.
  if (!VirtualFree(memory, 0, MEM_RELEASE)) return 8;
  puts("UDPGUARDED all actual shared-helper controls passed");
  return 0;
}
