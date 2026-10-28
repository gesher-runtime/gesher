// CI-only actual kernel transition witness. See native3-kernel-witness.md.
#pragma once
#include "../../compat/posix/src/record_lock_witness_hook.h"
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstddef>
#include <cwchar>

namespace kernel_witness {
inline void Require(bool ok, const char* label) {
  if (!ok) {
    std::fprintf(stderr, "KERNEL_WITNESS_FAILURE %s winerror=%lu\n", label,
                 static_cast<unsigned long>(GetLastError()));
    // The outer controller always stops the owned named trace on failure.
    std::_Exit(111);
  }
}
inline int64_t Qpc() {
  LARGE_INTEGER value{};
  Require(QueryPerformanceCounter(&value), "QPC");
  return value.QuadPart;
}
struct Phase { int64_t begin = 0, end = 0; };
inline std::array<Phase, 128> phases;
inline unsigned phase_count = 0;
inline std::atomic<unsigned> entered{0};
inline void PhaseHook(bool entering) {
  Require(phase_count < phases.size(), "bounded phase storage");
  if (entering) {
    phases[phase_count].begin = Qpc();
    entered.fetch_add(1, std::memory_order_release);
  } else {
    phases[phase_count].end = Qpc();
    ++phase_count;
  }
}
struct Identity { DWORD tid = 0; uint64_t created = 0; };
inline Identity GetIdentity(HANDLE thread) {
  FILETIME born{}, exit{}, kernel{}, user{};
  Require(GetThreadTimes(thread, &born, &exit, &kernel, &user), "retained handle birth identity");
  return {GetThreadId(thread), (uint64_t(born.dwHighDateTime) << 32) | born.dwLowDateTime};
}
struct Birth { DWORD pid, tid; int64_t qpc; };
struct Switch { DWORD old_tid, new_tid; uint8_t state, reason, mode; int64_t qpc; };
struct Evidence {
  std::vector<Birth> births;
  std::vector<Switch> switches;
  bool supported = true;
};
inline const GUID thread_provider{0x3d6fa8d1, 0xfe05, 0x11d0,
                                  {0x9d, 0xda, 0x00, 0xc0, 0x4f, 0xd7, 0xba, 0x7c}};
template<class T> inline bool Property(EVENT_RECORD* record, const wchar_t* name, T* value) {
  PROPERTY_DATA_DESCRIPTOR property{};
  property.PropertyName = reinterpret_cast<ULONGLONG>(name);
  property.ArrayIndex = ULONG_MAX;
  ULONG size = 0;
  return TdhGetPropertySize(record, 0, nullptr, 1, &property, &size) == ERROR_SUCCESS &&
    size == sizeof(T) && TdhGetProperty(record, 0, nullptr, 1, &property, size,
                                      reinterpret_cast<PBYTE>(value)) == ERROR_SUCCESS;
}
inline void WINAPI Consume(EVENT_RECORD* record) {
  auto& evidence = *static_cast<Evidence*>(record->UserContext);
  if (!IsEqualGUID(record->EventHeader.ProviderId, thread_provider)) return;
  const auto opcode = record->EventHeader.EventDescriptor.Opcode;
  if (opcode == EVENT_TRACE_TYPE_START) {
    Birth row{}; row.qpc = record->EventHeader.TimeStamp.QuadPart;
    if (!Property(record, L"ProcessId", &row.pid) || !Property(record, L"TThreadId", &row.tid)) {
      evidence.supported = false; return;
    }
    if (row.pid == GetCurrentProcessId()) evidence.births.push_back(row);
  } else if (opcode == 36) {
    Switch row{}; row.qpc = record->EventHeader.TimeStamp.QuadPart;
    if (!Property(record, L"OldThreadId", &row.old_tid) ||
        !Property(record, L"NewThreadId", &row.new_tid) ||
        !Property(record, L"OldThreadState", &row.state) ||
        !Property(record, L"OldThreadWaitReason", &row.reason) ||
        !Property(record, L"OldThreadWaitMode", &row.mode)) {
      evidence.supported = false; return;
    }
    if (evidence.switches.size() >= 1000000) { evidence.supported = false; return; }
    evidence.switches.push_back(row);
  }
}
// Birth is an actual Start, never a rundown/DCStart or guessed TID. The retained
// handle prevents recycling while captured; its FILETIME must match exactly.
inline bool Witness(const Evidence& evidence, Identity expected, Identity retained,
                    Phase phase, int64_t release, bool require_wake) {
  if (!evidence.supported || !phase.begin || phase.end <= phase.begin ||
      expected.tid != retained.tid || expected.created != retained.created) return false;
  unsigned births = 0;
  for (const auto& birth : evidence.births)
    if (birth.pid == GetCurrentProcessId() && birth.tid == expected.tid && birth.qpc <= phase.begin) ++births;
  if (births != 1) return false;
  for (const auto& change : evidence.switches) {
    if (change.old_tid != expected.tid || change.state != 5 || change.mode != 1 ||
        (change.reason != 6 && change.reason != 13) ||
        change.qpc < phase.begin || change.qpc > phase.end) continue;
    if (!require_wake) return true;
    if (release <= change.qpc || release >= phase.end) continue;
    for (const auto& wake : evidence.switches)
      if (wake.new_tid == expected.tid && wake.qpc >= release && wake.qpc <= phase.end) return true;
  }
  return false;
}
struct Properties {
  EVENT_TRACE_PROPERTIES value{};
  wchar_t name[128]{};
  wchar_t file[MAX_PATH]{};
};
inline int Run(const locks::PreparedOperation& prepared, HANDLE cancel) {
  LARGE_INTEGER frequency{}; Require(QueryPerformanceFrequency(&frequency), "QPC frequency");
  HANDLE token = nullptr;
  Require(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token), "open trace token");
  TOKEN_PRIVILEGES privileges{}; privileges.PrivilegeCount = 1;
  Require(LookupPrivilegeValueW(nullptr, L"SeSystemProfilePrivilege", &privileges.Privileges[0].Luid), "lookup profile privilege");
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  SetLastError(ERROR_SUCCESS);
  Require(AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
          GetLastError() == ERROR_SUCCESS, "enable profile privilege (no fallback)");
  Require(CloseHandle(token), "close token");
  Properties properties;
  std::swprintf(properties.name, 128, L"Gesher.Native3.KernelWitness.%lu", static_cast<unsigned long>(GetCurrentProcessId()));
  // A caller-owned empty working directory makes this a bounded CI-local path.
  DWORD path_size = GetFullPathNameW(L"kernel-witness.etl", MAX_PATH, properties.file, nullptr);
  Require(path_size > 0 && path_size < MAX_PATH, "trace file path");
  auto& p = properties.value;
  p.Wnode.BufferSize = sizeof(properties); p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  p.Wnode.ClientContext = 1;  // QueryPerformanceCounter, preserved by RAW_TIMESTAMP.
  p.LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_SYSTEM_LOGGER_MODE;
  p.MaximumFileSize = 64; p.BufferSize = 64; p.MinimumBuffers = 16; p.MaximumBuffers = 128;
  p.EnableFlags = EVENT_TRACE_FLAG_THREAD | EVENT_TRACE_FLAG_CSWITCH | EVENT_TRACE_FLAG_NO_SYSCONFIG;
  p.LoggerNameOffset = offsetof(Properties, name); p.LogFileNameOffset = offsetof(Properties, file);
  TRACEHANDLE session = 0;
  const ULONG started = StartTraceW(&session, properties.name, &p);
  SetLastError(started); Require(started == ERROR_SUCCESS, "StartTrace owned system logger");
  std::printf("KERNEL_TRACE session=%ls pid=%lu\n", properties.name, static_cast<unsigned long>(GetCurrentProcessId()));
  std::fflush(stdout);
  // Fresh native handles are retained until after trace analysis.
  std::atomic<bool> spin_stop{false};
  std::atomic<DWORD> owner_tid{0}, spin_tid{0}, other_tid{0};
  int64_t spin_begin = 0, spin_end = 0, other_begin = 0, other_end = 0;
  HANDLE other_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Require(other_release != nullptr, "other-thread event");
  std::thread other([&] {
    other_tid.store(GetCurrentThreadId(), std::memory_order_release);
    other_begin = Qpc();
    Require(WaitForSingleObject(other_release, 5000) == WAIT_OBJECT_0, "other thread finite wake");
    other_end = Qpc();
  });
  std::thread spinning([&] {
    spin_tid.store(GetCurrentThreadId(), std::memory_order_release);
    spin_begin = Qpc();
    while (!spin_stop.load(std::memory_order_acquire)) YieldProcessor();
    spin_end = Qpc();
  });
  int outcome = -1;
  std::thread owner([&] {
    owner_tid.store(GetCurrentThreadId(), std::memory_order_release);
    locks::record_wait_phase = PhaseHook;
    locks::Registry registry;
    outcome = registry.Open();
    if (!outcome) outcome = locks::WaitProcessLock(registry, prepared, cancel, 4000);
    locks::record_wait_phase = nullptr;
  });
  const int64_t identities_bound = Qpc() + frequency.QuadPart;
  while ((!owner_tid.load(std::memory_order_acquire) || !spin_tid.load(std::memory_order_acquire) ||
          !other_tid.load(std::memory_order_acquire)) && Qpc() < identities_bound) YieldProcessor();
  Require(owner_tid.load() && spin_tid.load() && other_tid.load(), "three live thread IDs published");
  HANDLE owner_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, owner_tid.load());
  HANDLE spin_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, spin_tid.load());
  HANDLE other_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, other_tid.load());
  Require(owner_handle && spin_handle && other_handle, "retain three thread handles");
  const Identity owner_id = GetIdentity(owner_handle), spin_id = GetIdentity(spin_handle), other_id = GetIdentity(other_handle);
  const int64_t bound = Qpc() + frequency.QuadPart * 2;
  while (!entered.load(std::memory_order_acquire) && Qpc() < bound) YieldProcessor();
  Require(entered.load(std::memory_order_acquire) > 0, "actual internal wait phase entered");
  // This bounded observation interval is collection only. No duration or
  // registration is accepted as proof; only the captured CSwitch passes below.
  const int64_t collect_until = Qpc() + frequency.QuadPart / 2;
  while (Qpc() < collect_until) YieldProcessor();
  const int64_t release = Qpc();
  Require(SetEvent(cancel), "cancel actual internal wait");
  Require(WaitForSingleObject(owner_handle, 5000) == WAIT_OBJECT_0, "actual internal return bound");
  owner.join();
  Require(outcome == ECANCELED && phase_count > 0, "actual internal cancellation result");
  const int64_t other_signal = Qpc();
  Require(SetEvent(other_release), "wake other thread");
  spin_stop.store(true, std::memory_order_release);
  Require(WaitForSingleObject(other_handle, 5000) == WAIT_OBJECT_0 &&
          WaitForSingleObject(spin_handle, 5000) == WAIT_OBJECT_0, "control threads finite join");
  other.join(); spinning.join();
  const int64_t wrong_phase = Qpc();
  ULONG stopped = ControlTraceW(session, properties.name, &p, EVENT_TRACE_CONTROL_STOP);
  SetLastError(stopped); Require(stopped == ERROR_SUCCESS, "stop owned trace");
  Require(p.EventsLost == 0 && p.LogBuffersLost == 0 && p.RealTimeBuffersLost == 0, "lossless kernel collection");
  Evidence evidence;
  EVENT_TRACE_LOGFILEW log{}; log.LogFileName = properties.file;
  log.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  log.EventRecordCallback = Consume; log.Context = &evidence;
  TRACEHANDLE reader = OpenTraceW(&log);
  Require(reader != INVALID_PROCESSTRACE_HANDLE, "open actual ETL");
  ULONG processed = ProcessTrace(&reader, 1, nullptr, nullptr);
  ULONG closed = CloseTrace(reader);
  Require(processed == ERROR_SUCCESS && closed == ERROR_SUCCESS && log.EventsLost == 0, "complete actual ETL decode");
  bool actual = false;
  for (unsigned i = 0; i < phase_count; ++i) {
    const auto phase = phases[i];
    if (Witness(evidence, owner_id, GetIdentity(owner_handle), phase, release, true)) {
      actual = true;
      std::printf("KERNEL_WAIT pid=%lu tid=%lu created=%llu phase_begin=%lld phase_end=%lld release=%lld qpc_hz=%lld\n",
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long>(owner_id.tid),
        static_cast<unsigned long long>(owner_id.created), static_cast<long long>(phase.begin),
        static_cast<long long>(phase.end), static_cast<long long>(release), static_cast<long long>(frequency.QuadPart));
    }
  }
  Require(actual, "actual internal phase Waiting/UserRequest and post-release schedule-in");
  Require(Witness(evidence, other_id, GetIdentity(other_handle), {other_begin, other_end}, other_signal, true), "independent other-thread calibration");
  Require(!Witness(evidence, spin_id, GetIdentity(spin_handle), {spin_begin, spin_end}, 0, false), "runnable thread rejected");
  bool scheduled = false;
  for (const auto& change : evidence.switches)
    if (change.old_tid == spin_id.tid && change.qpc >= spin_begin && change.qpc <= spin_end) scheduled = true;
  Require(scheduled, "runnable negative actually observed by kernel");
  Require(!Witness(evidence, owner_id, GetIdentity(other_handle), {other_begin, other_end}, other_signal, true), "other retained thread rejected");
  Identity stale = owner_id; ++stale.created;
  Require(!Witness(evidence, stale, GetIdentity(owner_handle), phases[0], 0, false), "stale creation rejected");
  Require(!Witness(evidence, owner_id, GetIdentity(owner_handle), {wrong_phase, wrong_phase + 1}, 0, false), "wrong phase rejected");
  evidence.supported = false;
  Require(!Witness(evidence, owner_id, GetIdentity(owner_handle), phases[0], 0, false), "unsupported observation rejected");
  // A signaled cancellation must never enter the internal kernel-wait phase.
  const unsigned old_count = phase_count;
  locks::record_wait_phase = PhaseHook;
  locks::Registry signaled_registry;
  Require(signaled_registry.Open() == 0 &&
          locks::WaitProcessLock(signaled_registry, prepared, cancel, 1000) == ECANCELED &&
          phase_count == old_count, "pre-signaled cancellation has no wait phase");
  locks::record_wait_phase = nullptr;
  Require(CloseHandle(owner_handle) && CloseHandle(other_handle) && CloseHandle(spin_handle) &&
          CloseHandle(other_release), "all retained handles closed");
  Require(DeleteFileW(properties.file), "owned ETL removed");
  // Stop succeeded; querying this exact owned session must report absence.
  Require(ControlTraceW(0, properties.name, &p, EVENT_TRACE_CONTROL_QUERY) == ERROR_WMI_INSTANCE_NOT_FOUND,
          "owned trace session absent after cleanup");
  std::puts("KERNEL_WITNESS_SUMMARY actual_internal=1 controls=7 failures=0 public_blocking_closed=false");
  return 0;
}
}  // namespace kernel_witness
