// gesher-base: the per-process ptrace state shared between processes
// (docs/decisions/0093-cross-process-ptrace.md).
//
// Every Gesher process publishes a small named section,
// `Local\gesher-ptrace-<pid>`, with what Linux keeps in the kernel for
// ptrace: the dumpable flag, the uid, Yama's PR_SET_PTRACER exception, the
// tracer and the stop the tracer asked for. A tracer
// (compat/bionic/ptrace_bionic.cc) opens the tracee's section to check
// permissions and claim it, and asks the tracee to stop or resume through the
// named event `Local\gesher-ptrace-request-<pid>`; the tracee's guard answers
// on `Local\gesher-ptrace-done-<pid>`. The guard thread (the executable's
// copy of gesher-base only) stops the process at a safe point and resumes
// it; it also watches the tracer and releases the process when the tracer
// exits (killing it first under PTRACE_O_EXITKILL).
//
// The section is not a security boundary: any code running as the same
// Windows user can open and write it, as it can open the process itself.
#pragma once

#include <windows.h>

#include <cstdint>

namespace gesher::ptrace {

// A range of memory a tracer wrote (PTRACE_POKE*), for the arm64 translator
// to drop what it translated from it. `epoch` is the write's sequence number.
struct CodeWrite {
  volatile LONG64 epoch;
  volatile LONG64 address;
  volatile LONG64 size;
};
constexpr int kCodeWrites = 64;  // a ring; older writes than that clear everything

// A thread parked in one of Gesher's blocking waits while it owns one of its
// cross-process mutexes (a pipe description's gate): ParkedScope.
struct ParkedSlot {
  volatile LONG tid;
  volatile LONG restart;      // the guard interrupted its wait for a stop
  volatile LONG restartable;  // a stop interrupts the wait (else it is left)
  volatile LONG reserved;     // the slot is taken
  volatile LONG64 gate;       // the mutex it owns (its handle value here)
  volatile LONG64 file;       // the handle and OVERLAPPED of its pending I/O
  volatile LONG64 overlapped;
};
constexpr int kParkedSlots = 256;

// The section's layout. Fields change only with Interlocked operations.
struct SharedState {
  volatile LONG version;       // kStateVersion once initialised
  volatile LONG pid;           // the owning process
  volatile LONG dumpable;      // PR_SET_DUMPABLE: 0 or 1 (SUID_DUMP_USER)
  volatile LONG ptracer;       // PR_SET_PTRACER: 0 none, -1 any, else a pid
  volatile LONG uid;           // the process's uid (getuid)
  volatile LONG want_stop;     // the tracer asks for a stop (1) or a resume (0)
  volatile LONG stopped;       // the guard has stopped the process
  volatile LONG guard_tid;     // the guard thread (0: no guard, cannot be traced)
  volatile LONG options;       // PTRACE_O_* the tracer set (EXITKILL acts here)
  volatile LONG reserved;
  // The tracer as one value, so that claiming and releasing it are single
  // compare-exchanges: TracerId(pid, creation time); 0 when not traced.
  volatile LONG64 tracer;
  volatile LONG64 start;       // the owning process's creation time (pid reuse)
  volatile LONG64 stop_epoch;  // incremented at every stop the guard completes
  volatile LONG64 code_epoch;  // the last CodeWrite's epoch
  CodeWrite code_writes[kCodeWrites];
  // Threads parked in one of Gesher's blocking waits while owning one of its
  // cross-process mutexes. A stop interrupts such a wait, as a Linux stop
  // interrupts a blocked read(): the thread gives the mutex back, waits for
  // the stop to end outside it, and restarts its call.
  ParkedSlot parked[kParkedSlots];
};
constexpr LONG kStateVersion = 7;
constexpr LONG kOptExitkill = 0x100000;  // PTRACE_O_EXITKILL

// A tracer's identity: its pid in the low half, the low 32 bits of its
// creation time (FILETIME) in the high half (0: any process with that pid,
// for init).
constexpr int64_t TracerId(DWORD pid, int64_t start) {
  return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(start)) << 32) | pid);
}
constexpr DWORD TracerIdPid(int64_t id) { return static_cast<DWORD>(static_cast<uint64_t>(id)); }
// This process's identity as a tracer.
int64_t SelfTracerId();

// This process's state, created on first use (also in a process cloned from
// another: the state then follows the new pid and keeps the dumpable flag, as
// a forked child keeps its mm's).
SharedState* Self();

// prctl(PR_GET_DUMPABLE) / prctl(PR_SET_DUMPABLE, value): 0 or -errno.
int GetDumpable();
int SetDumpable(uint64_t value);
// prctl(PR_SET_PTRACER, value): 0 (clear), PR_SET_PTRACER_ANY (all ones, as
// the kernel's 64-bit unsigned long) or a pid that exists. 0 or -errno
// (EINVAL for a pid that does not exist).
int SetPtracer(uint64_t value);
// /proc/self/status TracerPid: this process's tracer, 0 when it is not traced
// (or its tracer has exited: Linux detaches it then).
int TracerPid();

// A process's creation time (FILETIME as an integer), 0 if it cannot be read.
int64_t ProcessStartTime(HANDLE process);
// Whether the tracer `id` still runs.
bool TracerAlive(int64_t id);
// Clears `state->tracer` if that process has exited; returns the live tracer.
int64_t LiveTracer(SharedState* state);

// What a tracer may do to a tracee (Linux __ptrace_may_access and Yama
// ptrace_scope 1, without CAP_SYS_PTRACE): the decision, apart from finding
// the processes. `tracer_is_ancestor`: the tracer is an ancestor of the
// tracee; `tracer_descends_from_ptracer`: the tracer is the process the
// tracee named with PR_SET_PTRACER, or one of its descendants.
struct AccessFacts {
  bool same_process;
  int tracer_uid;
  int tracee_uid;
  int tracee_dumpable;
  int tracee_ptracer;  // 0, -1 (any) or a pid
  bool tracer_is_ancestor;
  bool tracer_descends_from_ptracer;
};
// 0 if allowed, else -EPERM.
int MayAttach(const AccessFacts& facts);

// The writes a tracer made to this process, as the guard published them
// before the process ran again (the translator reads them on each dispatch;
// the address does not change). `epoch` is the last one's.
struct PublishedCodeWrites {
  volatile int64_t epoch;
  CodeWrite writes[kCodeWrites];
};
const PublishedCodeWrites* CodeWrites();
// Called by the guard, with every other thread of the process suspended,
// when a tracer wrote memory: the translator makes its running code return to
// its dispatcher. It must take no lock and allocate nothing.
// The translator's code is System V on x86-64 hosts; an ARM64 host has one ABI.
#if defined(__x86_64__)
using HaltHook = void (__attribute__((sysv_abi)) *)();
#else
using HaltHook = void (*)();
#endif
void SetHaltHook(HaltHook hook);

// Marks the calling thread as parked in a Gesher blocking wait while it owns
// the Gesher mutex `gate`, for the scope's lifetime. A stop interrupts a
// restartable wait (CancelSynchronousIo): the caller then sees
// Interrupted(), releases the mutex, calls WaitWhileStopping() and restarts
// its call, as Linux restarts a system call a stop interrupted. A wait that
// cannot be interrupted without losing track of its progress (a pipe write)
// is a safe point as it is: the stop leaves it pending, owning `gate`.
class ParkedScope {
 public:
  explicit ParkedScope(HANDLE gate = nullptr, bool restartable = true);
  ~ParkedScope();
  ParkedScope(const ParkedScope&) = delete;
  ParkedScope& operator=(const ParkedScope&) = delete;
  // The I/O the wait is for: a stop cancels exactly that request
  // (CancelIoEx), with the thread still suspended in it.
  void SetIo(HANDLE file, OVERLAPPED* overlapped);
  // Whether a stop interrupted the wait.
  bool Interrupted() const { return slot_ != nullptr && slot_->restart != 0; }

 private:
  ParkedSlot* slot_ = nullptr;
};
// Waits, holding no Gesher mutex, while this process's tracer asks for a stop
// (the guard suspends the caller there meanwhile).
void WaitWhileStopping();

// Another process's state, for a tracer.
struct Remote {
  HANDLE section = nullptr;
  SharedState* state = nullptr;
  HANDLE request = nullptr;  // the guard's request event
  HANDLE done = nullptr;     // the guard's answer
};
// False when `pid` publishes no state (not a Gesher process, or gone), or,
// with `start` (its creation time), a state an exited process with the same
// pid left (the new process has not published its own yet).
bool OpenRemote(DWORD pid, Remote* out, int64_t start = 0);
void CloseRemote(Remote* remote);
// Wakes the guard after changing `want_stop`, `tracer` or `options`.
void Notify(const Remote& remote);
// Records a write the tracer made to the tracee's memory.
void RecordCodeWrite(const Remote& remote, uint64_t address, uint64_t size);

}  // namespace gesher::ptrace
