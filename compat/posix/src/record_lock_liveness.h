// Conservative owner identity probes. A permission/query error is never death.
#pragma once
#include "record_lock_registry.h"

namespace gesher::record_locks {

enum class Liveness : uint8_t { kAlive, kDead, kUnknown };
int CurrentProcessIdentity(ProcessIdentity* identity);
int CurrentThreadIdentity(ThreadIdentity* identity);
Liveness ProbeProcess(const ProcessIdentity& identity);
Liveness ProbeThread(const ThreadIdentity& identity);
int FileIdentity(HANDLE file, File* identity);
int HandleAccess(HANDLE handle, uint32_t* access, uint32_t* handle_count = nullptr);

struct Probes {
  Liveness (*process)(const ProcessIdentity&) = ProbeProcess;
  Liveness (*thread)(const ThreadIdentity&) = ProbeThread;
};
struct Pruned {
  size_t references = 0, process_locks = 0, description_locks = 0, waits = 0, unknown = 0;
};
// Changes only the caller's staged snapshot. Real process/thread probes are
// default; deterministic failure/reuse probes exist only as an explicit test
// dependency. Unknown owners/references/waits remain intact.
void PruneDeadOwners(RegistryState* state, Pruned* result, const Probes& probes = {});

}  // namespace gesher::record_locks
