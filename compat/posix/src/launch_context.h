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
  uint32_t version = 3;
  uint32_t uid = 0;
  uint64_t control = 0; // inherited endpoint, ONLY broker or dedicated writer
  uint64_t broker = 0;  // inherited actual broker HANDLE, ONLY dedicated writer
  uint32_t kind = 0;    // 0: ordinary self snapshot, 1: broker, 2: writer
  uint32_t server_pid = 0; // public expected broker identity, never a capability
};
static_assert(sizeof(Context) == 40, "startup context ABI");
struct Snapshot { bool present; uint32_t uid; uint64_t control = 0, broker = 0; uint32_t kind = 0, server_pid = 0; };
inline Snapshot ReadStartup() {
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  GetStartupInfoW(&startup);
  Context context;
  if (startup.cbReserved2 != sizeof(context) || !startup.lpReserved2) return {false, 0};
  std::memcpy(&context, startup.lpReserved2, sizeof(context));
  if (context.crt_descriptors || context.magic != 0x474c4358 || context.version != 3 ||
      context.uid > 19999 || context.kind > 2 || context.server_pid > INT32_MAX) return {false, 0};
  return {true, context.uid, context.control, context.broker, context.kind, context.server_pid};
}
// Initialized when this native module loads, before guest setenv can run.
// Source-only: exact CRT startup ABI/build and real child fixture remain owed.
inline const Snapshot kStartup = ReadStartup();
} }  // namespace gesher::launch
