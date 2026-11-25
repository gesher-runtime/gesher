// gesher-base: the per-process ptrace state and the tracee's stop guard
// (include/gesher/ptrace_state.h, docs/decisions/0093-cross-process-ptrace.md).
//
// Linux keeps a process's dumpable flag, its uid, its Yama ptracer exception
// and its tracer in the kernel, where both the tracer and procfs see them.
// Here each process publishes them in a named section that any Gesher process
// can open by pid. The section is created by the first copy of gesher-base in
// the process that needs it; the executable's copy also starts the guard.
//
// The guard is how a tracee stops. Linux stops every thread of a process that
// a tracer attaches to (the attach SIGSTOP is a group stop), and a thread
// stops where it can take a signal: in user code, or in an interruptible
// wait such as a read() on an empty pipe, never inside the kernel holding a
// lock another process needs. Gesher's system calls run in user mode, and
// some hold one of Gesher's named (cross-process) Windows mutexes for a
// moment: record locks, the log, system properties, open file descriptions.
// A thread suspended there would block every other process that needs that
// mutex, the tracer included. So the guard:
//  1. suspends every other thread of its process (GetThreadContext waits
//     until each really is), repeating until a pass finds none running;
//  2. looks for a Gesher mutex (its name starts with "gesher", any case) that
//     one of them owns. It resumes them all; if the owner is parked in one of
//     Gesher's blocking waits (ParkedScope: a pipe transfer holds its
//     description's gate while it blocks), it interrupts that wait, as a
//     Linux stop interrupts a blocked read() or write(): the owner releases
//     the gate and waits for the stop to end outside it, then goes on. It
//     waits (20 ms at a time, redoing the pass) for the mutex, and once it
//     has it suspends them all again holding it (no thread can take it again
//     before all are suspended), releases it and checks again;
//  3. publishes `stopped` only when every other thread is suspended and none
//     owns a Gesher mutex. A step that cannot be
//     completed (a thread it cannot suspend, a handle list it cannot read)
//     never publishes a stop: it retries.
// While stopped it rescans every 20 ms; a thread the system started meanwhile
// is suspended, and checked as in step 2.
//
// While the others are suspended the guard only makes system calls: no heap,
// no loader lock, no thread pool, since a suspended thread may hold them.
#include <gesher/ptrace_state.h>

#include <errno.h>
#include <unistd.h>
#include <windows.h>
#include <winternl.h>

#include <climits>
#include <string>

namespace gesher::ptrace {
namespace {

std::wstring SectionName(DWORD pid) { return L"Local\\gesher-ptrace-" + std::to_wstring(pid); }
std::wstring RequestName(DWORD pid) {
  return L"Local\\gesher-ptrace-request-" + std::to_wstring(pid);
}
std::wstring DoneName(DWORD pid) { return L"Local\\gesher-ptrace-done-" + std::to_wstring(pid); }

SRWLOCK g_lock = SRWLOCK_INIT;
DWORD g_pid = 0;  // the pid g_state belongs to (a cloned process has another)
SharedState* g_state = nullptr;
HANDLE g_request = nullptr;
HANDLE g_done = nullptr;
bool g_owner = false;  // this copy of gesher-base is the executable's
PublishedCodeWrites g_code_writes = {};
volatile HaltHook g_halt_hook = nullptr;

// --- ntdll ---------------------------------------------------------------------

using NtGetNextThreadFn = LONG(NTAPI*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);
using NtQueryInformationProcessFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using NtQueryMutantFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
using NtQueryObjectFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtGetNextThreadFn g_next_thread = nullptr;
NtQueryInformationProcessFn g_query_process = nullptr;
NtQueryMutantFn g_query_mutant = nullptr;
NtQueryObjectFn g_query_object = nullptr;
constexpr LONG kNoMoreEntries = static_cast<LONG>(0x8000001A);

// --- suspending the other threads ------------------------------------------------

// The threads the guard suspended. Static: nothing is allocated while the
// other threads are suspended.
constexpr int kMaxThreads = 8192;
HANDLE g_suspended[kMaxThreads];
DWORD g_suspended_ids[kMaxThreads];
int g_suspended_count = 0;

bool AlreadySuspended(DWORD id) {
  for (int i = 0; i < g_suspended_count; ++i) {
    if (g_suspended_ids[i] == id) return true;
  }
  return false;
}

enum class Walk { kComplete, kIncomplete };

// Suspends every thread of this process but the calling one, until a pass
// over the thread list finds none left running (a thread that was still
// running may have created another). kIncomplete when a thread could not be
// suspended or the list could not be read to its end; those suspended so
// far stay suspended. `*added` counts the threads newly suspended.
Walk SuspendOthers(int* added = nullptr) {
  const DWORD self = GetCurrentThreadId();
  constexpr ACCESS_MASK kAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                  THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
  Walk result = Walk::kComplete;
  for (bool changed = true; changed;) {
    changed = false;
    HANDLE current = nullptr;
    bool current_kept = false;
    for (;;) {
      HANDLE next = nullptr;
      const LONG status = g_next_thread(GetCurrentProcess(), current, kAccess, 0, 0, &next);
      if (current != nullptr && !current_kept) CloseHandle(current);
      current = nullptr;
      if (status < 0) {
        if (status != kNoMoreEntries) result = Walk::kIncomplete;
        break;
      }
      current = next;
      current_kept = false;
      const DWORD id = GetThreadId(next);
      if (id == 0 || id == self || AlreadySuspended(id)) continue;
      if (g_suspended_count == kMaxThreads) {
        result = Walk::kIncomplete;
        continue;
      }
      if (SuspendThread(next) == static_cast<DWORD>(-1)) {
        // An exiting thread runs no more user code; any other is a failure.
        if (WaitForSingleObject(next, 0) != WAIT_OBJECT_0) result = Walk::kIncomplete;
        continue;
      }
      // SuspendThread returns before the thread stops; reading its context
      // waits until it has.
      CONTEXT context;
      context.ContextFlags = CONTEXT_CONTROL;
      GetThreadContext(next, &context);
      g_suspended[g_suspended_count] = next;
      g_suspended_ids[g_suspended_count] = id;
      ++g_suspended_count;
      if (added != nullptr) ++*added;
      current_kept = true;
      changed = true;
    }
  }
  return result;
}

void ResumeOthers() {
  for (int i = 0; i < g_suspended_count; ++i) {
    ResumeThread(g_suspended[i]);
    CloseHandle(g_suspended[i]);
  }
  g_suspended_count = 0;
}

// --- Gesher mutexes the suspended threads own -------------------------------------

constexpr int kProcessHandleInformation = 51;
constexpr int kMutantOwnerInformation = 1;
constexpr int kObjectNameInformation = 1;
struct HandleEntry {
  HANDLE handle;
  ULONG_PTR handle_count;
  ULONG_PTR pointer_count;
  ACCESS_MASK access;
  ULONG type;
  ULONG attributes;
  ULONG reserved;
};
struct HandleSnapshot {
  ULONG_PTR count;
  ULONG_PTR reserved;
  HandleEntry entries[1];
};
struct ClientId {
  HANDLE process;
  HANDLE thread;
};

HANDLE g_probe_mutex = nullptr;  // never owned: tells which type index is a mutex
void* g_snapshot = nullptr;      // VirtualAlloc'd, not the heap
SIZE_T g_snapshot_size = 0;
alignas(8) unsigned char g_name_buffer[4096];

ParkedSlot* FindParked(SharedState* state, DWORD tid) {
  for (int i = 0; i < kParkedSlots; ++i) {
    if (static_cast<DWORD>(state->parked[i].tid) == tid) return &state->parked[i];
  }
  return nullptr;
}

// Whether a mutex is one of Gesher's: the last component of its name starts
// with "gesher" (Local\gesher-pipe-..., Global\Gesher.RecordLocks.v1, ...).
bool GesherMutex(HANDLE handle) {
  ULONG length = 0;
  if (g_query_object(handle, kObjectNameInformation, g_name_buffer, sizeof(g_name_buffer),
                     &length) < 0) {
    return false;
  }
  const auto* name = reinterpret_cast<const UNICODE_STRING*>(g_name_buffer);
  if (name->Buffer == nullptr || name->Length == 0) return false;  // unnamed: this process's own
  const size_t count = name->Length / sizeof(wchar_t);
  size_t start = count;
  while (start > 0 && name->Buffer[start - 1] != L'\\') --start;
  static const wchar_t kPrefix[] = L"gesher";
  for (size_t i = 0; i < 6; ++i) {
    if (start + i >= count) return false;
    wchar_t c = name->Buffer[start + i];
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (c != kPrefix[i]) return false;
  }
  return true;
}

enum class Owned { kNone, kFound, kUnknown };

// A duplicate of a Gesher mutex a suspended thread owns, and its owner.
// kUnknown when the handles cannot be listed or a Gesher mutex cannot be
// examined; other mutexes (a Windows DLL's, the app's) are not looked at.
Owned OwnedMutex(HANDLE* out, DWORD* owner_tid) {
  *out = nullptr;
  *owner_tid = 0;
  if (g_query_process == nullptr || g_query_mutant == nullptr || g_query_object == nullptr ||
      g_probe_mutex == nullptr) {
    return Owned::kUnknown;
  }
  for (;;) {
    if (g_snapshot == nullptr) {
      g_snapshot_size = g_snapshot_size == 0 ? 256 * 1024 : g_snapshot_size * 2;
      if (g_snapshot_size > (256u << 20)) return Owned::kUnknown;
      g_snapshot = VirtualAlloc(nullptr, g_snapshot_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (g_snapshot == nullptr) return Owned::kUnknown;
    }
    ULONG needed = 0;
    const LONG status = g_query_process(GetCurrentProcess(), kProcessHandleInformation,
                                        g_snapshot, static_cast<ULONG>(g_snapshot_size), &needed);
    if (status >= 0) break;
    VirtualFree(g_snapshot, 0, MEM_RELEASE);
    g_snapshot = nullptr;
  }
  const auto* snapshot = static_cast<const HandleSnapshot*>(g_snapshot);
  ULONG mutex_type = ULONG_MAX;
  for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
    if (snapshot->entries[i].handle == g_probe_mutex) mutex_type = snapshot->entries[i].type;
  }
  if (mutex_type == ULONG_MAX) return Owned::kUnknown;
  for (ULONG_PTR i = 0; i < snapshot->count; ++i) {
    const HandleEntry& entry = snapshot->entries[i];
    if (entry.type != mutex_type || entry.handle == g_probe_mutex) continue;
    // The name first: only Gesher's own mutexes matter, and nothing else is
    // duplicated or queried.
    if (!GesherMutex(entry.handle)) continue;
    HANDLE query = entry.handle;
    HANDLE copy = nullptr;
    // MUTANT_QUERY_STATE (1) is what CreateMutex's MUTEX_MODIFY_STATE grants.
    if ((entry.access & 1) == 0) {
      if (!DuplicateHandle(GetCurrentProcess(), entry.handle, GetCurrentProcess(), &copy,
                           SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, 0)) {
        return Owned::kUnknown;
      }
      query = copy;
    }
    ClientId owner = {};
    const LONG status =
        g_query_mutant(query, kMutantOwnerInformation, &owner, sizeof(owner), nullptr);
    const DWORD thread = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(owner.thread));
    const bool ours = status >= 0 && thread != 0 && AlreadySuspended(thread);
    if (status < 0 || !ours) {
      if (copy != nullptr) CloseHandle(copy);
      if (status < 0) return Owned::kUnknown;
      continue;
    }
    if (copy == nullptr &&
        !DuplicateHandle(GetCurrentProcess(), entry.handle, GetCurrentProcess(), &copy,
                         SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, 0)) {
      return Owned::kUnknown;
    }
    *out = copy;
    *owner_tid = thread;
    return Owned::kFound;
  }
  return Owned::kNone;
}

// --- the guard -----------------------------------------------------------------

// The tracer this guard watches, and a handle to it (null for init, or one
// this process may not open: then it looks now and then instead).
int64_t g_watched = 0;
HANDLE g_watched_process = nullptr;

void Watch(int64_t tracer) {
  if (tracer == g_watched) return;
  if (g_watched_process != nullptr) CloseHandle(g_watched_process);
  g_watched_process = nullptr;
  g_watched = tracer;
  const DWORD pid = TracerIdPid(tracer);
  if (tracer != 0 && pid != 1) {
    g_watched_process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (g_watched_process != nullptr &&
        static_cast<uint32_t>(ProcessStartTime(g_watched_process)) !=
            static_cast<uint32_t>(static_cast<uint64_t>(tracer) >> 32)) {
      // Not the tracer: the pid names another process now.
      CloseHandle(g_watched_process);
      g_watched_process = nullptr;
    }
  }
}

// The tracer exited without detaching: Linux detaches its tracees, and kills
// them first if it asked for PTRACE_O_EXITKILL.
void TracerGone(SharedState* state, int64_t tracer) {
  if ((state->options & kOptExitkill) != 0) TerminateProcess(GetCurrentProcess(), 128 + 9);
  if (InterlockedCompareExchange64(&state->tracer, 0, tracer) == tracer) {
    InterlockedExchange(&state->options, 0);
    InterlockedExchange(&state->want_stop, 0);
  }
}

// Waits on the request event and the watched tracer. Returns 0 for a request
// (or the timeout), 1 when the tracer exited.
int WaitGuard(DWORD timeout_ms) {
  HANDLE handles[2] = {g_request, g_watched_process};
  const DWORD n = g_watched_process != nullptr ? 2 : 1;
  // Without a handle to the tracer, look whether it still runs now and then.
  if (g_watched != 0 && g_watched_process == nullptr && TracerIdPid(g_watched) != 1 &&
      timeout_ms > 250) {
    timeout_ms = 250;
  }
  const DWORD w = WaitForMultipleObjects(n, handles, FALSE, timeout_ms);
  if (w == WAIT_OBJECT_0 + 1) return 1;
  if (w == WAIT_TIMEOUT && g_watched != 0 && g_watched_process == nullptr &&
      TracerIdPid(g_watched) != 1 && !TracerAlive(g_watched)) {
    return 1;
  }
  return 0;
}

bool StillWanted(SharedState* state, int64_t tracer) {
  return state->want_stop != 0 && state->tracer == tracer;
}


// Brings the suspended set to a safe point: returns true with every other
// thread suspended and no Gesher mutex owned by any of them, false if
// the stop was withdrawn meanwhile (the others then run).
bool ReachSafePoint(SharedState* state, int64_t tracer) {
  for (;;) {
    if (!StillWanted(state, tracer)) {
      ResumeOthers();
      return false;
    }
    if (SuspendOthers() != Walk::kComplete) {
      // A thread it could not suspend: no stop it could stand by. Retry.
      ResumeOthers();
      if (WaitGuard(20) == 1) TracerGone(state, tracer);
      continue;
    }
    HANDLE mutex = nullptr;
    DWORD owner = 0;
    const Owned owned = OwnedMutex(&mutex, &owner);
    if (owned == Owned::kNone) return true;
    // A suspended thread owns a Gesher mutex. Parked in one of Gesher's
    // blocking waits, it is interrupted: it gives the mutex back and waits
    // for the stop outside it. A wait in I/O is identified while the thread
    // is suspended (its handle and OVERLAPPED) and cancelled once it runs
    // again: cancelling a thread's synchronous I/O waits for that thread, so
    // it cannot be done while it is suspended. CancelIoEx with that
    // OVERLAPPED reaches no other request than that one, or a later one of
    // the same thread with the same OVERLAPPED on the same handle, which is
    // then interrupted as harmlessly (a transfer that moved nothing restarts).
    HANDLE cancel_file = nullptr;
    OVERLAPPED* cancel_overlapped = nullptr;
    if (owned == Owned::kFound) {
      ParkedSlot* slot = FindParked(state, owner);
      if (slot != nullptr) {
        InterlockedExchange(&slot->restart, 1);
        cancel_file = reinterpret_cast<HANDLE>(static_cast<intptr_t>(slot->file));
        cancel_overlapped = reinterpret_cast<OVERLAPPED*>(static_cast<intptr_t>(slot->overlapped));
      }
    }
    ResumeOthers();
    // A thread that has not issued its request yet is found again by the
    // next pass.
    if (cancel_file != nullptr && cancel_overlapped != nullptr) CancelIoEx(cancel_file, cancel_overlapped);
    if (owned == Owned::kUnknown) {
      if (WaitGuard(20) == 1) TracerGone(state, tracer);
      continue;
    }
    // Let the owner run until it has released the mutex, then take it, so
    // that no thread takes it again before they are all suspended. 20 ms at
    // a time: a thread that took it again meanwhile is found by the next
    // pass.
    HANDLE handles[3] = {mutex, g_request, g_watched_process};
    const DWORD n = g_watched_process != nullptr ? 3 : 2;
    const DWORD w = WaitForMultipleObjects(n, handles, FALSE, 20);
    if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED_0) {
      // An abandoned mutex (its owner died) is taken and released here: its
      // next owner sees an ordinary acquisition. Gesher's code treats both
      // the same, and only Gesher's mutexes are taken.
      SuspendOthers();
      ReleaseMutex(mutex);
    } else if (w == WAIT_OBJECT_0 + 2) {
      CloseHandle(mutex);
      TracerGone(state, tracer);
      continue;
    }
    CloseHandle(mutex);
  }
}

void Publish(SharedState* state) {
  InterlockedExchange(&state->stopped, 1);
  InterlockedIncrement64(&state->stop_epoch);
  SetEvent(g_done);
}

void Resume(SharedState* state) {
  // Memory a tracer wrote reaches the translator before any thread runs
  // again: the ranges, then their epoch, then running code is halted so that
  // it returns to the dispatcher and drops what it translated from them.
  const int64_t epoch = state->code_epoch;
  if (epoch != g_code_writes.epoch) {
    for (int i = 0; i < kCodeWrites; ++i) {
      g_code_writes.writes[i].address = state->code_writes[i].address;
      g_code_writes.writes[i].size = state->code_writes[i].size;
      InterlockedExchange64(&g_code_writes.writes[i].epoch, state->code_writes[i].epoch);
    }
    InterlockedExchange64(const_cast<LONG64*>(&g_code_writes.epoch), epoch);
    if (HaltHook hook = g_halt_hook) hook();
  }
  ResumeOthers();
  InterlockedExchange(&state->stopped, 0);
  SetEvent(g_done);
}

DWORD WINAPI GuardMain(void* parameter) {
  auto* state = static_cast<SharedState*>(parameter);
  for (;;) {
    int64_t tracer = state->tracer;
    if (tracer != 0 && !TracerAlive(tracer)) {
      TracerGone(state, tracer);
      continue;
    }
    Watch(tracer);
    if (tracer == 0) {
      InterlockedExchange(&state->want_stop, 0);
      WaitGuard(INFINITE);
      continue;
    }
    if (state->want_stop == 0) {
      if (WaitGuard(INFINITE) == 1) TracerGone(state, tracer);
      continue;
    }
    if (!ReachSafePoint(state, tracer)) continue;
    Publish(state);
    // Stopped until the tracer resumes or releases it, or exits.
    for (;;) {
      if (WaitGuard(20) == 1) {
        TracerGone(state, tracer);
        break;
      }
      if (!StillWanted(state, tracer)) break;
      // A thread the system started meanwhile (a thread pool worker):
      // suspend it, and make sure it holds no Gesher mutex.
      int added = 0;
      const Walk walk = SuspendOthers(&added);
      if (added == 0 && walk == Walk::kComplete) continue;
      HANDLE mutex = nullptr;
      DWORD owner = 0;
      const Owned owned =
          walk == Walk::kComplete ? OwnedMutex(&mutex, &owner) : Owned::kUnknown;
      if (owned == Owned::kNone) continue;
      if (mutex != nullptr) CloseHandle(mutex);
      // Not at a safe point any more: withdraw the stop until it is again.
      InterlockedExchange(&state->stopped, 0);
      if (!ReachSafePoint(state, tracer)) break;
      Publish(state);
    }
    Resume(state);
  }
}

// Creates (or re-creates, in a process cloned from another) this process's
// state. Called with g_lock held exclusively.
void CreateLocked(DWORD pid) {
  LONG dumpable = 1;
  if (g_state != nullptr) {
    // A clone of another process: its view still maps the parent's section,
    // and the parent's handle values mean nothing here. Keep the dumpable
    // flag (a forked child keeps it); everything else (tracer, ptracer
    // exception) belongs to the parent.
    dumpable = g_state->dumpable;
    g_state = nullptr;
    g_request = nullptr;
    g_done = nullptr;
    g_probe_mutex = nullptr;
    g_watched = 0;
    g_watched_process = nullptr;
    g_suspended_count = 0;
  }
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(SharedState), SectionName(pid).c_str());
  if (section == nullptr) return;
  const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
  auto* state = static_cast<SharedState*>(
      MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState)));
  HANDLE request = CreateEventW(nullptr, FALSE, FALSE, RequestName(pid).c_str());
  HANDLE done = CreateEventW(nullptr, FALSE, FALSE, DoneName(pid).c_str());
  if (state == nullptr || request == nullptr || done == nullptr) {
    if (state != nullptr) UnmapViewOfFile(state);
    if (request != nullptr) CloseHandle(request);
    if (done != nullptr) CloseHandle(done);
    CloseHandle(section);
    return;
  }
  // Initialise a new section, or one left by an exited process with the same
  // pid that a tracer still had open (the executable's copy, which runs first
  // in a new process, always does). Another module's copy of gesher-base in
  // this process finds it initialised (same pid, same version).
  if (!existed || g_owner || state->version != kStateVersion ||
      state->pid != static_cast<LONG>(pid)) {
    InterlockedExchange(&state->version, 0);
    InterlockedExchange(&state->dumpable, dumpable);
    InterlockedExchange(&state->ptracer, 0);
    InterlockedExchange(&state->uid, static_cast<LONG>(getuid()));
    InterlockedExchange(&state->want_stop, 0);
    InterlockedExchange(&state->stopped, 0);
    InterlockedExchange(&state->guard_tid, 0);
    InterlockedExchange(&state->options, 0);
    InterlockedExchange64(&state->tracer, 0);
    InterlockedExchange64(&state->start, ProcessStartTime(GetCurrentProcess()));
    InterlockedExchange64(&state->stop_epoch, 0);
    InterlockedExchange64(&state->code_epoch, g_code_writes.epoch);
    for (int i = 0; i < kParkedSlots; ++i) {
      InterlockedExchange(&state->parked[i].tid, 0);
      InterlockedExchange(&state->parked[i].restart, 0);
      InterlockedExchange(&state->parked[i].taken, 0);
    }
    InterlockedExchange(&state->pid, static_cast<LONG>(pid));
  }
  // The section and events stay open for the life of the process.
  g_state = state;
  g_request = request;
  g_done = done;
  g_pid = pid;
  if (g_owner && g_next_thread != nullptr) {
    if (g_probe_mutex == nullptr) g_probe_mutex = CreateMutexW(nullptr, FALSE, nullptr);
    // A small stack: the guard only makes system calls. Its id is published
    // before the state is, so that a tracer never finds a state without it.
    DWORD guard_tid = 0;
    HANDLE guard = CreateThread(nullptr, 64 * 1024, GuardMain, state,
                                STACK_SIZE_PARAM_IS_A_RESERVATION, &guard_tid);
    if (guard != nullptr) {
      InterlockedExchange(&state->guard_tid, static_cast<LONG>(guard_tid));
      CloseHandle(guard);
    }
  }
  if (state->version != kStateVersion) InterlockedExchange(&state->version, kStateVersion);
}

// The executable's copy owns the guard; it starts with the process, so a
// tracer can stop a process that never called anything ptrace-related.
const bool g_started = [] {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&g_lock), &self) ||
      self != GetModuleHandleW(nullptr)) {
    return false;
  }
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  g_next_thread = reinterpret_cast<NtGetNextThreadFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtGetNextThread")));
  g_query_process = reinterpret_cast<NtQueryInformationProcessFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationProcess")));
  g_query_mutant = reinterpret_cast<NtQueryMutantFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryMutant")));
  g_query_object = reinterpret_cast<NtQueryObjectFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryObject")));
  g_owner = true;
  AcquireSRWLockExclusive(&g_lock);
  CreateLocked(GetCurrentProcessId());
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}();

}  // namespace

SharedState* Self() {
  const DWORD pid = GetCurrentProcessId();
  AcquireSRWLockShared(&g_lock);
  SharedState* state = g_pid == pid ? g_state : nullptr;
  ReleaseSRWLockShared(&g_lock);
  if (state != nullptr) return state;
  AcquireSRWLockExclusive(&g_lock);
  if (g_pid != pid || g_state == nullptr) CreateLocked(pid);
  state = g_state;
  ReleaseSRWLockExclusive(&g_lock);
  return state;
}

int64_t SelfTracerId() {
  // Cached per process: a process cloned from this one (fork) has another
  // pid and creation time, and computes its own.
  static volatile LONG64 cached_id = 0;
  const DWORD pid = GetCurrentProcessId();
  const int64_t cached = cached_id;
  if (cached != 0 && TracerIdPid(cached) == pid) return cached;
  const int64_t id = TracerId(pid, ProcessStartTime(GetCurrentProcess()));
  InterlockedExchange64(&cached_id, id);
  return id;
}

int GetDumpable() {
  SharedState* state = Self();
  return state != nullptr ? static_cast<int>(state->dumpable) : 1;
}

int SetDumpable(uint64_t value) {
  if (value != 0 && value != 1) return -EINVAL;  // SUID_DUMP_DISABLE, SUID_DUMP_USER
  SharedState* state = Self();
  if (state == nullptr) return -ENOMEM;
  InterlockedExchange(&state->dumpable, static_cast<LONG>(value));
  return 0;
}

int SetPtracer(uint64_t value) {
  SharedState* state = Self();
  if (state == nullptr) return -ENOMEM;
  LONG ptracer;
  if (value == 0) {
    ptracer = 0;
  } else if (value == ~uint64_t{0}) {  // PR_SET_PTRACER_ANY
    ptracer = -1;
  } else {
    if (value > static_cast<uint64_t>(INT_MAX)) return -EINVAL;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(value));
    if (h == nullptr) {
      if (GetLastError() != ERROR_ACCESS_DENIED) return -EINVAL;  // no such process
    } else {
      DWORD code = 0;
      bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
      CloseHandle(h);
      if (!alive) return -EINVAL;
    }
    ptracer = static_cast<LONG>(value);
  }
  InterlockedExchange(&state->ptracer, ptracer);
  return 0;
}

int TracerPid() {
  SharedState* state = Self();
  return state != nullptr ? static_cast<int>(TracerIdPid(LiveTracer(state))) : 0;
}

int64_t ProcessStartTime(HANDLE process) {
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
  return static_cast<int64_t>((static_cast<uint64_t>(created.dwHighDateTime) << 32) |
                              created.dwLowDateTime);
}

bool TracerAlive(int64_t id) {
  const DWORD pid = TracerIdPid(id);
  if (pid == 1) return true;  // init outlives everything
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
  DWORD code = 0;
  const uint32_t start = static_cast<uint32_t>(static_cast<uint64_t>(id) >> 32);
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE &&
               (start == 0 || static_cast<uint32_t>(ProcessStartTime(h)) == start);
  CloseHandle(h);
  return alive;
}

int64_t LiveTracer(SharedState* state) {
  const int64_t tracer = state->tracer;
  if (tracer == 0 || TracerAlive(tracer)) return tracer;
  // Under PTRACE_O_EXITKILL the tracee's guard kills it first.
  if ((state->options & kOptExitkill) != 0) return 0;
  if (InterlockedCompareExchange64(&state->tracer, 0, tracer) == tracer) {
    InterlockedExchange(&state->options, 0);
  }
  return state->tracer;
}

int MayAttach(const AccessFacts& facts) {
  // kernel/ptrace.c: a thread of the same process cannot be attached to.
  if (facts.same_process) return -EPERM;
  // __ptrace_may_access: another uid needs CAP_SYS_PTRACE, which no app has;
  // so does a process that is not dumpable.
  if (facts.tracer_uid != facts.tracee_uid) return -EPERM;
  if (facts.tracee_dumpable == 0) return -EPERM;
  // Yama ptrace_scope 1 (yama_ptrace_access_check).
  if (facts.tracer_is_ancestor) return 0;
  if (facts.tracee_ptracer == -1) return 0;
  if (facts.tracee_ptracer > 0 && facts.tracer_descends_from_ptracer) return 0;
  return -EPERM;
}

const PublishedCodeWrites* CodeWrites() { return &g_code_writes; }

void SetHaltHook(HaltHook hook) { g_halt_hook = hook; }

ParkedScope::ParkedScope() {
  SharedState* state = Self();
  if (state == nullptr) return;
  const LONG tid = static_cast<LONG>(GetCurrentThreadId());
  // The slot is filled before its thread id is, so that the guard never sees
  // a thread with another's request.
  for (int i = 0; i < kParkedSlots; ++i) {
    ParkedSlot& slot = state->parked[i];
    if (InterlockedCompareExchange(&slot.taken, 1, 0) != 0) continue;
    InterlockedExchange(&slot.restart, 0);
    InterlockedExchange64(&slot.file, 0);
    InterlockedExchange64(&slot.overlapped, 0);
    InterlockedExchange(&slot.tid, tid);
    slot_ = &slot;
    return;
  }
  // All slots taken: the thread is not marked, and a stop waits until its
  // wait ends.
}

void ParkedScope::SetIo(HANDLE file, OVERLAPPED* overlapped) {
  if (slot_ == nullptr) return;
  InterlockedExchange64(&slot_->overlapped, static_cast<LONG64>(reinterpret_cast<intptr_t>(overlapped)));
  InterlockedExchange64(&slot_->file, static_cast<LONG64>(reinterpret_cast<intptr_t>(file)));
}

ParkedScope::~ParkedScope() {
  if (slot_ == nullptr) return;
  InterlockedExchange(&slot_->tid, 0);
  InterlockedExchange64(&slot_->file, 0);
  InterlockedExchange64(&slot_->overlapped, 0);
  InterlockedExchange(&slot_->restart, 0);
  InterlockedExchange(&slot_->taken, 0);
}

void WaitWhileStopping() {
  SharedState* state = Self();
  if (state == nullptr) return;
  while (state->want_stop != 0 && state->tracer != 0) Sleep(1);
}

bool OpenRemote(DWORD pid, Remote* out, int64_t start) {
  *out = Remote{};
  HANDLE section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, SectionName(pid).c_str());
  if (section == nullptr) return false;
  auto* state = static_cast<SharedState*>(
      MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedState)));
  if (state == nullptr || state->version != kStateVersion ||
      state->pid != static_cast<LONG>(pid) || (start != 0 && state->start != start)) {
    if (state != nullptr) UnmapViewOfFile(state);
    CloseHandle(section);
    return false;
  }
  out->section = section;
  out->state = state;
  out->request = OpenEventW(EVENT_MODIFY_STATE, FALSE, RequestName(pid).c_str());
  out->done = OpenEventW(SYNCHRONIZE, FALSE, DoneName(pid).c_str());
  return true;
}

void CloseRemote(Remote* remote) {
  if (remote->state != nullptr) UnmapViewOfFile(remote->state);
  if (remote->section != nullptr) CloseHandle(remote->section);
  if (remote->request != nullptr) CloseHandle(remote->request);
  if (remote->done != nullptr) CloseHandle(remote->done);
  *remote = Remote{};
}

void Notify(const Remote& remote) {
  if (remote.request != nullptr) SetEvent(remote.request);
}

void RecordCodeWrite(const Remote& remote, uint64_t address, uint64_t size) {
  SharedState* state = remote.state;
  // One tracer per tracee, and the tracee is stopped: no other writer.
  const int64_t epoch = state->code_epoch + 1;
  CodeWrite& slot = state->code_writes[epoch % kCodeWrites];
  InterlockedExchange64(&slot.epoch, 0);
  InterlockedExchange64(&slot.address, static_cast<LONG64>(address));
  InterlockedExchange64(&slot.size, static_cast<LONG64>(size));
  InterlockedExchange64(&slot.epoch, epoch);
  InterlockedExchange64(&state->code_epoch, epoch);
}

}  // namespace gesher::ptrace

// Anchors this file (-u in posix.cmake): every process starts its guard.
extern "C" int gesher_ptrace_state_anchor_v1 = 0;
