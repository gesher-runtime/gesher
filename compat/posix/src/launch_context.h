// Private CreateProcess startup snapshot. Not a registration credential: only
// a broker-owned launch record can authorize UID. No argv/environment lookup.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <cstring>

namespace gesher { namespace launch {
// lpReserved2 begins with the CRT descriptor count (zero). The trailing record
// is copied by CreateProcess into this child's startup information. Never put
// secrets here: guest code can inspect its own snapshot.
struct Context {
  uint32_t crt_descriptors = 0;
  uint32_t magic = 0x474c4358;
  uint32_t version = 1;
  uint32_t uid = 0;
};
static_assert(sizeof(Context) == 16, "startup context ABI");
struct Snapshot { bool present; uint32_t uid; };
inline Snapshot ReadStartup() {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  GetStartupInfoW(&startup);
  Context context;
  if (startup.cbReserved2 != sizeof(context) || !startup.lpReserved2) return {false, 0};
  std::memcpy(&context, startup.lpReserved2, sizeof(context));
  if (context.crt_descriptors || context.magic != 0x474c4358 || context.version != 1 ||
      context.uid > 19999) return {false, 0};
  return {true, context.uid};
}
// Initialized when this native module loads, before guest setenv can run.
// Source-only: exact CRT startup ABI/build and real child fixture remain owed.
inline const Snapshot kStartup = ReadStartup();
} }  // namespace gesher::launch
