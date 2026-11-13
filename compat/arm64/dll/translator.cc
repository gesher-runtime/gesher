// gesher arm64 translator: gesher_arm64.dll. Runs guest AArch64 code with
// dynarmic (A64 frontend, x86-64 backend) in the host process, the guest
// sharing the host address space, and marshals AAPCS64 <-> the host ABI in
// both directions. docs/arm64.md, docs/decisions/0047-arm64-translation.md.
//
// gesher_arm64_init() exchanges the two interface tables
// (gesher/arm64_translator.h). Then the loader (compat/arm64, compat/elf):
//  - add_code() registers each mapped guest executable segment. dynarmic only
//    translates code inside registered regions; a guest branch anywhere else
//    is a call of a host (System V) function, or a crash if nothing is there.
//  - each guest import that resolves to a host function is bound to a guest
//    trampoline `SVC #n; RET`; CallSVC(n) calls the host function n through
//    gesher_call_host_sysv (AAPCS64 -> System V).
//  - host code calls guest functions through host_entry(fn) System V stubs,
//    or straight through the guest pointer: guest code pages are not
//    executable, so the call faults (DEP) and the vectored handler resumes it
//    at the function's host_entry stub.
//  - a fault, an undefined instruction or a BRK in guest code ends the process
//    with an Android-style "Fatal signal" report (never a hang).

#include "translator.h"
#include <gesher/arm64_va_list.h>

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cfenv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"

namespace A64 = Dynarmic::A64;

// A guest -> host call, marshalled to System V by gesher_call_host_sysv.
struct HostCall {
  void* fn;              // the host System V function
  const uint64_t* gpr;   // X0..X5 -> rdi,rsi,rdx,rcx,r8,r9
  const uint64_t* xmm;   // V0..V7 low 64 bits -> xmm0..xmm7
  const uint64_t* stack; // host stack arguments: X6, X7, then guest stack args
  uint64_t nstack;       // count of `stack`
  uint64_t out_xmm0;     // written by the thunk: the result's xmm0 low 64 bits
  uint64_t out_rdx;      // written by the thunk: the result's rdx
};

// What gesher_guest_entry_shim builds from a System V call (thunks.S).
struct EntryFrame {
  uint64_t gpr[8];
  uint64_t xmm[8];
  const uint64_t* stack;  // the caller's stack arguments 9.. (System V)
  uint64_t out_rdx;
  uint64_t out_xmm0;
};

// thunks.S (Microsoft x64 entry):
extern "C" uint64_t gesher_call_host_sysv(HostCall* call);
extern "C" void gesher_guest_entry_shim();  // host_entry stubs jump here

namespace gesher_arm64 {

const gesher_arm64_host* g_host = nullptr;

namespace {

// A guest's rounding mode must not escape into its native host caller. This
// also contains deliberate host fenv changes made by a nested callback.
class HostFenvScope {
 public:
  HostFenvScope() { std::fegetenv(&saved_); }
  ~HostFenvScope() { std::fesetenv(&saved_); }
 private:
  std::fenv_t saved_;
};

// Internal callbacks can run with live guest MXCSR. Native address/cache
// classification is not guest arithmetic (unordered_map may do FP math).
class GuestMxcsrScope {
 public:
  GuestMxcsrScope() : saved_(_mm_getcsr()) { _mm_setcsr(0x1f80); }
  ~GuestMxcsrScope() { _mm_setcsr(saved_); }
 private:
  unsigned saved_;
};

// --- guest code regions -------------------------------------------------------
struct CodeRegion {
  uintptr_t start;
  uintptr_t end;
};
std::shared_mutex g_regions_mutex;
std::vector<CodeRegion> g_regions;

void AddRegion(uintptr_t start, size_t size) {
  std::unique_lock<std::shared_mutex> lock(g_regions_mutex);
  g_regions.push_back({start, start + size});
}

// --- guest code pages for trampolines and guest-side helpers ------------------
// Readable, not executable: the host never runs guest code (a host call into
// one of these faults and is redirected like any other guest function).
std::mutex g_code_mutex;
uint8_t* g_code_page = nullptr;
size_t g_code_used = 0;
constexpr size_t kCodePage = 0x10000;
std::vector<uint8_t*> g_code_pages;  // every one so far

uint32_t* AllocGuestCode(size_t words) {
  std::lock_guard<std::mutex> lock(g_code_mutex);
  size_t bytes = words * 4;
  if (g_code_page == nullptr || g_code_used + bytes > kCodePage) {
    g_code_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kCodePage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_code_used = 0;
    if (g_code_page == nullptr) return nullptr;
    g_code_pages.push_back(g_code_page);
    AddRegion(reinterpret_cast<uintptr_t>(g_code_page), kCodePage);
  }
  auto* p = reinterpret_cast<uint32_t*>(g_code_page + g_code_used);
  g_code_used += (bytes + 15) & ~size_t{15};
  return p;
}

// --- guest trampolines for host imports --------------------------------------
// `SVC #n; RET` with n = index + 1 into g_tramps. SVC #0 is a raw Linux
// syscall made by guest code; SVC #0xFFFF is the return stub.
constexpr uint32_t kRet = 0xD65F03C0;
constexpr uint32_t Svc(uint32_t imm) { return 0xD4000001u | (imm << 5); }
constexpr uint32_t kReturnSvc = 0xFFFF;
constexpr size_t kMaxTramps = 0xFFFE;
// Each trampoline has 32 bytes to itself, the rest BRK: an in-place hook of a
// libc function (WhatsApp's libessential) overwrites its first instructions
// with a jump and a literal address, 20 bytes and more. In a 16-byte slot
// that address landed on the next function's trampoline (abort's), and the
// guest's next call there ran half an address as code.
constexpr size_t kTrampWords = 8;
constexpr uint32_t kBrk = 0xD4200000;

struct Trampoline {
  void* host_fn;
  int va_list_index;
};
Trampoline g_tramps[kMaxTramps];
std::atomic<size_t> g_tramp_count{0};
std::mutex g_tramp_mutex;
std::unordered_map<void*, void*> g_tramp_by_fn;

// --- va_list host functions --------------------------------------------------
std::mutex g_va_mutex;
std::unordered_map<const void*, int> g_va_functions;

int VaListIndexOf(const void* fn) {
  std::lock_guard<std::mutex> lock(g_va_mutex);
  auto it = g_va_functions.find(fn);
  return it == g_va_functions.end() ? -1 : it->second;
}

// --- guest TLS (variant I: TPIDR_EL0 -> TCB, then the static TLS modules) ----
// bionic's arm64 TCB is 8 slots (TLS_SLOT_STACK_GUARD, read by
// -fstack-protector code at [TPIDR_EL0 + 40], is slot 5), so module offsets
// start at 64 and never overlap them.
constexpr size_t kTcbSize = 64;
constexpr size_t kTlsBlockSize = 256 * 1024;  // per thread
std::mutex g_tls_mutex;
size_t g_tls_offset = kTcbSize;  // next free offset in a block
struct TlsImage {
  int64_t offset;
  const void* image;
  size_t image_size;
  size_t total_size;
};
std::vector<TlsImage> g_tls_images;
std::vector<uint8_t*> g_tls_blocks;  // every live thread's block (its TP)

int64_t ApiTlsRegister(size_t total_size, size_t align) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  if (align < 1) align = 1;
  size_t off = (g_tls_offset + align - 1) & ~(align - 1);
  if (off + total_size > kTlsBlockSize) return -1;
  g_tls_offset = off + total_size;
  return static_cast<int64_t>(off);
}

void ApiTlsPublish(int64_t offset, const void* image, size_t image_size, size_t total_size) {
  std::lock_guard<std::mutex> lock(g_tls_mutex);
  g_tls_images.push_back({offset, image, image_size, total_size});
  for (uint8_t* base : g_tls_blocks) {
    std::memset(base + offset, 0, total_size);
    if (image != nullptr && image_size > 0) std::memcpy(base + offset, image, image_size);
  }
}

// --- the exclusive monitor and its processor ids ------------------------------
// Every live Jit (one per guest thread and nesting depth) has its own processor
// id. They used to be a counter modulo 64: an app with more guest threads than
// that (WhatsApp has 100+) had Jits sharing an id, and dynarmic keeps ONE
// reservation and ONE saved LDXR value per id. Two threads sharing an id that
// LDXR the same word (a contended lock, a reference count) overwrite each
// other's saved value, and the store-exclusive then compares memory with the
// OTHER thread's value: it succeeds with a result computed from a stale read,
// an update is lost, and a lock word or counter is corrupt for good (a thread
// spinning on it forever). Ids now come from a free list (the lowest free one,
// so the monitor's store-exclusive scan, patched to cover only the ids in use,
// stays short) and go back when the Jit is destroyed.
constexpr size_t kMaxProcessors = 4096;

Dynarmic::ExclusiveMonitor& Monitor() {
  static Dynarmic::ExclusiveMonitor m{kMaxProcessors};
  return m;
}

std::mutex g_ids_mutex;
std::vector<bool> g_ids_used;
size_t g_ids_active = 0;  // the monitor scans [0, g_ids_active)

size_t AcquireProcessorId() {
  std::lock_guard<std::mutex> lock(g_ids_mutex);
  size_t id = 0;
  while (id < g_ids_used.size() && g_ids_used[id]) ++id;
  if (id == g_ids_used.size()) {
    if (id >= kMaxProcessors) {
      GuestCrash(kSigSys, 1, "SYS_SECCOMP", 0, "more than 4096 live guest threads/Jits");
    }
    g_ids_used.push_back(false);
  }
  g_ids_used[id] = true;
  if (id + 1 > g_ids_active) {
    g_ids_active = id + 1;
    Monitor().SetActiveProcessorCount(g_ids_active);
  }
  return id;
}

void ReleaseProcessorId(size_t id) {
  Monitor().ClearProcessor(id);  // no stale reservation for the id's next owner
  std::lock_guard<std::mutex> lock(g_ids_mutex);
  g_ids_used[id] = false;
  size_t active = g_ids_active;
  while (active > 0 && !g_ids_used[active - 1]) --active;
  if (active != g_ids_active) {
    g_ids_active = active;
    Monitor().SetActiveProcessorCount(active);
  }
}

size_t LiveProcessorIds() {
  std::lock_guard<std::mutex> lock(g_ids_mutex);
  size_t n = 0;
  for (bool used : g_ids_used) n += used ? 1 : 0;
  return n;
}

// --- WFE / SEV -----------------------------------------------------------------
// dynarmic raises the hint instructions to ExceptionRaised (our A64 build's
// translation options hook them). On hardware, WFE sleeps until an event: a
// SEV, the PE's exclusive monitor being cleared (another core's store to the
// reserved granule), or Linux's event stream (every 100 us); a WFE spin-wait
// costs no CPU. As a no-op it burned a whole core for as long as the wait
// lasted. Here (Env::WaitForEvent):
//  - an event is: the thread's event register (SEVL; a SEV since its last
//    WFE), or its Jit's reservation in the global monitor being clear (no
//    LDXR before the WFE, or another thread's successful store-exclusive to
//    the granule cleared it). Then WFE returns at once.
//  - otherwise, for the first ~1 ms of a wait (consecutive WFEs at one PC), it
//    polls for an event with pauses and yields; after that it blocks on the
//    reservation granule's bucket of a small hashed table, which a
//    successful store-exclusive to that granule and SEV wake. A plain-store
//    release (STLR) cannot wake it: that block ends on the WaitOnAddress
//    timeout, one timer tick (1-16 ms at the default timer resolution).
constexpr uint64_t kNoReservation = 0xDEAD'DEAD'DEAD'DEADull;  // dynarmic's INVALID_EXCLUSIVE_ADDRESS
struct WfeBucket {
  std::atomic<uint64_t> epoch{0};
  std::atomic<int> waiters{0};
  char pad[64 - sizeof(std::atomic<uint64_t>) - sizeof(std::atomic<int>)];
};
WfeBucket g_wfe_buckets[64];
std::atomic<int> g_wfe_waiters{0};    // in all buckets
std::atomic<uint64_t> g_sev_count{0};  // SEVs so far

WfeBucket& BucketFor(uint64_t address) { return g_wfe_buckets[(address >> 6) & 63]; }

void WakeBucket(WfeBucket& b) {
  b.epoch.fetch_add(1, std::memory_order_seq_cst);
  WakeByAddressAll(&b.epoch);
}

// A successful store-exclusive to `address` (under the monitor's lock, after
// it cleared the reservations on it): wake the WFE waiters of its granule.
// The fence pairs with the waiter's increment before it re-checks its
// reservation, so neither side misses the other. Waking happens only when
// someone waits on that granule's bucket, so the lock is rarely held longer.
void StoreExclusiveEvent(uint64_t address) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (g_wfe_waiters.load(std::memory_order_relaxed) == 0) return;
  WfeBucket& b = BucketFor(address);
  if (b.waiters.load(std::memory_order_relaxed) != 0) WakeBucket(b);
}

void SignalEvent() {  // SEV: every PE's event register
  g_sev_count.fetch_add(1, std::memory_order_seq_cst);
  if (g_wfe_waiters.load(std::memory_order_seq_cst) == 0) return;
  for (WfeBucket& b : g_wfe_buckets) {
    if (b.waiters.load(std::memory_order_relaxed) != 0) WakeBucket(b);
  }
}

// The QPC counter as CNTPCT_EL0 (CNTFRQ_EL0 is set to its frequency).
uint64_t QpcFrequency() {
  static const uint64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return static_cast<uint64_t>(v.QuadPart);
  }();
  return f;
}
uint64_t QpcNow() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return static_cast<uint64_t>(v.QuadPart);
}

// --- diagnostics: GESHER_ARM64_PROFILE ----------------------------------------
// GESHER_ARM64_PROFILE=<ms> (e.g. 5000) starts a sampler: every 10 ms it
// samples each guest thread that is using CPU (its guest PC, or the host
// function it is in), and every <ms> it reports each thread that used more
// than a quarter of a core: its counters (guest -> host calls, raw syscalls,
// exclusive loads/stores and failed store CASes, hint instructions, Run()
// re-entries) and its hottest guest PCs and host functions.
// GESHER_ARM64_PROFILE_PC=1 also turns off block linking, so the sampled guest
// PC is the current block's (slower; the default PC is the last dispatch).
int ProfilePeriodMs() {
  static const int period = [] {
    char v[16] = {};
    DWORD n = GetEnvironmentVariableA("GESHER_ARM64_PROFILE", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return 0;
    int ms = std::atoi(v);
    return ms <= 0 ? 0 : (ms < 500 ? 500 : ms);
  }();
  return period;
}
bool ProfilePreciseEnabled() {
  static const bool on = [] {
    char v[4] = {};
    return ProfilePeriodMs() > 0 &&
           GetEnvironmentVariableA("GESHER_ARM64_PROFILE_PC", v, sizeof(v)) > 0 && v[0] == '1';
  }();
  return on;
}
void StartProfiler();

// The return stub: guest code `SVC #0xFFFF` the host sets as X30 before
// entering the guest; when the guest returns there, Run() stops.
uintptr_t g_return_stub = 0;

// Non-zero while this thread executes guest code (not while it is in a host
// call made by the guest): a fault then is the guest's.
thread_local int t_guest_running = 0;

class Env;
thread_local Env* t_env_raw = nullptr;

// Host list shape, with private ARM bank tags (see arm64_va_list.h).
struct SysvVaListTag {
  uint32_t gp_offset;
  uint32_t fp_offset;
  void* overflow_arg_area;
  void* reg_save_area;
};
struct Aapcs64VaList {
  uint8_t* stack;
  uint8_t* gr_top;
  uint8_t* vr_top;
  int32_t gr_offs;
  int32_t vr_offs;
};
struct VaListStorage {
  SysvVaListTag tag;
  gesher::abi::Arm64VaRegisters registers;
};
static_assert(sizeof(SysvVaListTag) == 24);

// Keep all eight GP slots separate from the eight FP slots. A plain SysV
// list cannot express that distinction: prefixing its overflow area with the
// seventh/eighth GP slot reorders earlier FP overflow arguments. The shim
// reads only the bank or actual stack slot requested by the consumer; even a
// literal-only format must never speculatively read beyond the guest stack.
void ConvertVaList(const Aapcs64VaList* a, VaListStorage* s) {
  int n_gp = a->gr_offs < 0 ? -a->gr_offs / 8 : 0;
  int n_fp = a->vr_offs < 0 ? -a->vr_offs / 16 : 0;
  if (n_fp > 8) n_fp = 8;
  if (n_gp > 8) n_gp = 8;
  s->registers = {a->gr_top, a->vr_top};
  s->tag.gp_offset = gesher::abi::kArm64VaTag | static_cast<uint32_t>((8 - n_gp) * 8);
  s->tag.fp_offset = gesher::abi::kArm64VaTag | static_cast<uint32_t>((8 - n_fp) * 16);
  s->tag.reg_save_area = &s->registers;
  s->tag.overflow_arg_area = a->stack;
}

// Pages already found to hold host code: a guest that calls a host pointer
// (a JNIEnv table entry, dlsym's result) does so again and again.
std::shared_mutex g_host_pages_mutex;
std::unordered_map<uintptr_t, bool> g_host_pages;

bool HostExecutableUncached(uintptr_t addr);

bool FastmemEnabled() {
  static const bool enabled = [] {
    char v[8] = {};
    DWORD n = GetEnvironmentVariableA("GESHER_ARM64_FASTMEM", v, sizeof(v));
    return !(n > 0 && v[0] == '0');
  }();
  return enabled;
}

bool HostExecutable(uintptr_t addr) {
  uintptr_t page = addr & ~uintptr_t{0xFFF};
  {
    std::shared_lock<std::shared_mutex> lock(g_host_pages_mutex);
    auto it = g_host_pages.find(page);
    if (it != g_host_pages.end()) return it->second;
  }
  bool exec = HostExecutableUncached(addr);
  if (exec) {  // only positive answers: a page may be mapped later
    std::unique_lock<std::shared_mutex> lock(g_host_pages_mutex);
    g_host_pages[page] = true;
  }
  return exec;
}

bool HostExecutableUncached(uintptr_t addr) {
  MEMORY_BASIC_INFORMATION mbi;
  if (addr < 0x10000 || VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) return false;
  DWORD p = mbi.Protect & 0xFF;
  return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}

// Per-thread counters: written only by the owning thread (a plain increment),
// read by the profiler.
struct Counter {
  std::atomic<uint64_t> v{0};
  void Bump() { v.store(v.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
  uint64_t Get() const { return v.load(std::memory_order_relaxed); }
};
struct ThreadCounters {
  Counter host_calls, syscalls, excl_reads, excl_writes, excl_cas_failed, hints, waits, runs;
};

class Env;
std::mutex g_envs_mutex;
std::vector<Env*> g_envs;  // every Env with a live thread (for the profiler)

class Env final : public A64::UserCallbacks {
 public:
  Env() {
    // This thread's guest TLS block (its thread pointer); Attach() fills it.
    tls_block_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTlsBlockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    tls_pointer_ = reinterpret_cast<uint64_t>(tls_block_);
    // A private guest stack for this thread, with a no-access guard page at
    // its low end (a guest stack overflow faults there and is reported).
    stack_base_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStackSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    DWORD old;
    VirtualProtect(stack_base_, kGuardSize, PAGE_NOACCESS, &old);
    stack_top_ = reinterpret_cast<uint64_t>(stack_base_ + kStackSize) & ~uint64_t{15};
    stack_cur_ = stack_top_;
  }

  ~Env() override {
    jits_.clear();
    for (size_t id : ids_) ReleaseProcessorId(id);
    VirtualFree(tls_block_, 0, MEM_RELEASE);
    VirtualFree(stack_base_, 0, MEM_RELEASE);
  }

  // The calling thread takes this Env (new, or one an exited thread left in
  // the pool): a fresh guest TLS block with the images of the modules loaded
  // so far, an empty guest stack, the default FP state.
  void Attach() {
    {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      // A pooled block holds its last thread's TLS: clear the TCB and every
      // module's area (all of [0, g_tls_offset)); a new block is zero anyway.
      if (used_before_) std::memset(tls_block_, 0, g_tls_offset);
      g_tls_blocks.push_back(tls_block_);
      for (const TlsImage& ti : g_tls_images) {
        std::memset(tls_block_ + ti.offset, 0, ti.total_size);
        if (ti.image) std::memcpy(tls_block_ + ti.offset, ti.image, ti.image_size);
      }
    }
    used_before_ = true;
    tls_pointer_ = reinterpret_cast<uint64_t>(tls_block_);  // a guest MSR TPIDR_EL0 may have moved it
    stack_cur_ = stack_top_;
    depth_ = 0;
    fpcr_ = fpsr_ = 0;
    event_ = false;
    wait_pc_ = 0;
    tid_ = GetCurrentThreadId();
    if (ProfilePeriodMs() > 0) {
      DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread_, 0,
                      FALSE, DUPLICATE_SAME_ACCESS);
      std::lock_guard<std::mutex> lock(g_envs_mutex);
      g_envs.push_back(this);
    }
  }

  // The thread is exiting. Afterwards the Env may be pooled for another thread.
  void Detach() {
    if (thread_ != nullptr) {
      {
        std::lock_guard<std::mutex> lock(g_envs_mutex);  // not while being sampled
        std::erase(g_envs, this);
      }
      CloseHandle(thread_);
      thread_ = nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      std::erase(g_tls_blocks, tls_block_);
    }
    if (t_env_raw == this) t_env_raw = nullptr;
    for (auto& jit : jits_) jit->ClearExclusiveState();
    for (size_t id : ids_) Monitor().ClearProcessor(id);
  }

  // Whether another thread may reuse this Env: not if its thread left in the
  // middle of guest code (a Jit still marked running).
  bool Reusable() const { return depth_ == 0; }

  // Going into the pool: the stack's contents are dead, so its pages are
  // decommitted (out of the working set and the commit charge; the no-access
  // guard pages at its low end stay as they are). Unpark() commits them again
  // for the next thread; false if the system cannot (the Env is then freed).
  void Park() {
    VirtualFree(stack_base_ + kGuardSize, kStackSize - kGuardSize, MEM_DECOMMIT);
  }
  bool Unpark() {
    return VirtualAlloc(stack_base_ + kGuardSize, kStackSize - kGuardSize, MEM_COMMIT,
                        PAGE_READWRITE) != nullptr;
  }

  // One Jit per nesting depth: dynarmic's Run() is not reentrant, so a
  // guest->host->guest chain (a host callback that is guest code — a qsort
  // comparator, a JNI call back into a guest native) runs the inner guest on
  // the next Jit. They share the thread's TLS pointer and guest stack.
  A64::Jit* JitAt(size_t depth) {
    while (jits_.size() <= depth) {
      A64::UserConfig conf;
      conf.callbacks = this;
      conf.tpidr_el0 = &tls_pointer_;
      conf.tpidrro_el0 = &tls_pointer_;
      const size_t id = AcquireProcessorId();
      ids_.push_back(id);
      conf.processor_id = id;
      conf.global_monitor = &Monitor();
      // Exclusives stay callbacks (the default): the monitor's inline form
      // would scan all kMaxProcessors slots, not the ids in use.
      conf.fastmem_exclusive_access = false;
      conf.enable_cycle_counting = false;
      conf.optimizations = Dynarmic::all_safe_optimizations;
      if (ProfilePreciseEnabled()) {
        conf.optimizations &= ~(Dynarmic::OptimizationFlag::BlockLinking |
                                Dynarmic::OptimizationFlag::ReturnStackBuffer |
                                Dynarmic::OptimizationFlag::FastDispatch);
      }
      conf.unsafe_optimizations = false;
      conf.wall_clock_cntpct = true;
      if (QpcFrequency() <= 0xFFFFFFFFu) conf.cntfrq_el0 = static_cast<uint32_t>(QpcFrequency());
      // Fastmem: the guest shares the host address space, so a guest access
      // is generated inline as a host access at [0 + vaddr] (all 64 bits),
      // not a callback. A fault there is a guest fault (the vectored handler
      // reports it). Exclusive accesses still go through the callbacks (the
      // CAS above). GESHER_ARM64_FASTMEM=0 turns it off.
      if (FastmemEnabled()) {
        conf.fastmem_pointer = uintptr_t{0};
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = false;
      }
      // The outermost Jit runs almost all guest code; nested ones are short.
      conf.code_cache_size = (jits_.empty() ? 32 : 8) * 1024 * 1024;
      jits_.push_back(std::make_unique<A64::Jit>(conf));
    }
    return jits_[depth].get();
  }
  A64::Jit* CurrentJit() { return depth_ > 0 ? jits_[depth_ - 1].get() : nullptr; }
  uint32_t Fpcr() {
    A64::Jit* jit = CurrentJit();
    return jit != nullptr ? jit->GetFpcr() : fpcr_;
  }
  void SetFpcr(uint32_t value) {
    A64::Jit* jit = CurrentJit();
    if (jit != nullptr) jit->SetFpcr(value);
    else fpcr_ = value;
  }
  uint32_t Fpsr() {
    A64::Jit* jit = CurrentJit();
    return jit != nullptr ? jit->GetFpsr() : fpsr_;
  }
  void SetFpsr(uint32_t value) {
    A64::Jit* jit = CurrentJit();
    if (jit != nullptr) jit->SetFpsr(value);
    else fpsr_ = value;
  }

  std::optional<std::uint32_t> MemoryReadCode(uint64_t a) override {
    if (!InGuestCode(a)) return std::nullopt;
    uint32_t v;
    std::memcpy(&v, reinterpret_cast<void*>(a), 4);
    return v;
  }
  // With fastmem, only exclusive loads (LDXR/LDAXR/LDXP) come here.
  std::uint8_t MemoryRead8(uint64_t a) override { counters_.excl_reads.Bump(); return *reinterpret_cast<volatile uint8_t*>(a); }
  std::uint16_t MemoryRead16(uint64_t a) override { counters_.excl_reads.Bump(); uint16_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 2); return v; }
  std::uint32_t MemoryRead32(uint64_t a) override { counters_.excl_reads.Bump(); uint32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; }
  std::uint64_t MemoryRead64(uint64_t a) override { counters_.excl_reads.Bump(); uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 8); return v; }
  A64::Vector MemoryRead128(uint64_t a) override { counters_.excl_reads.Bump(); A64::Vector v; std::memcpy(&v, reinterpret_cast<void*>(a), 16); return v; }
  void MemoryWrite8(uint64_t a, std::uint8_t v) override { *reinterpret_cast<volatile uint8_t*>(a) = v; }
  void MemoryWrite16(uint64_t a, std::uint16_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 2); }
  void MemoryWrite32(uint64_t a, std::uint32_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
  void MemoryWrite64(uint64_t a, std::uint64_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
  void MemoryWrite128(uint64_t a, A64::Vector v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 16); }

  // Store-exclusive (STXR/STLXR/STXP): dynarmic's global monitor checks the
  // reservation and hands the store here with the value LDXR read; a host
  // compare-and-swap makes it atomic against every other thread. (The
  // defaults return false, which made every guest LL/SC loop spin forever.)
  // (These run under the monitor's lock; a store-exclusive the monitor refuses
  // never gets here: excl_reads - excl_writes counts those.)
  template <typename T>
  bool Cas(uint64_t a, T value, T expected) {
    counters_.excl_writes.Bump();
    bool ok = __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, value, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    StoreExclusiveDone(ok, a);
    return ok;
  }
  void StoreExclusiveDone(bool ok, uint64_t a) {
    if (!ok) {
      counters_.excl_cas_failed.Bump();
    } else {
      StoreExclusiveEvent(a);  // it cleared other PEs' monitors: an event for WFE
    }
  }
  bool MemoryWriteExclusive8(uint64_t a, std::uint8_t v, std::uint8_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive16(uint64_t a, std::uint16_t v, std::uint16_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive32(uint64_t a, std::uint32_t v, std::uint32_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive64(uint64_t a, std::uint64_t v, std::uint64_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive128(uint64_t a, A64::Vector v, A64::Vector e) override {
    using U128 = unsigned __int128;
    counters_.excl_writes.Bump();
    U128 expected = (static_cast<U128>(e[1]) << 64) | e[0];
    U128 value = (static_cast<U128>(v[1]) << 64) | v[0];
    bool ok = __atomic_compare_exchange_n(reinterpret_cast<U128*>(a), &expected, value, false,
                                          __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);  // cmpxchg16b
    StoreExclusiveDone(ok, a);
    return ok;
  }

  void InterpreterFallback(uint64_t pc, size_t) override {
    uint32_t insn = 0;
    if (InGuestCode(pc)) std::memcpy(&insn, reinterpret_cast<void*>(pc), 4);
    char detail[112];
    std::snprintf(detail, sizeof(detail),
                  "instruction 0x%08x at 0x%llx is undefined or not supported by the translator",
                  insn, static_cast<unsigned long long>(pc));
    GuestCrash(kSigIll, 1, "ILL_ILLOPC", pc, detail);
  }

  void CallSVC(std::uint32_t swi) override {
    A64::Jit* jit = CurrentJit();
    if (swi == kReturnSvc) {  // the guest returned to the host
      jit->HaltExecution();
      return;
    }
    if (swi == 0) {  // a raw Linux syscall in guest code: x8 = number
      pending_call_ = {nullptr, -1, true};
      jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
      return;
    }
    if (swi - 1 >= g_tramp_count.load(std::memory_order_acquire)) {
      GuestCrash(kSigSys, 1, "SYS_SECCOMP", jit->GetPC() - 4, "SVC with an unknown number");
    }
    const Trampoline& t = g_tramps[swi - 1];
    QueueHostCall(jit, t.host_fn, t.va_list_index);
  }

  void ExceptionRaised(uint64_t pc, A64::Exception ex) override {
    A64::Jit* jit = CurrentJit();
    switch (ex) {
      case A64::Exception::NoExecuteFault: {
        GuestMxcsrScope callback_fenv;
        // A guest branch to an address outside every guest code region. If
        // host code is there, it is a host function the guest got as a plain
        // pointer (dlsym, eglGetProcAddress, a table): call it as System V
        // and return to the guest's X30, as a bound trampoline would.
        if (HostExecutable(pc)) {
          QueueHostCall(jit, reinterpret_cast<void*>(pc), VaListIndexOf(reinterpret_cast<void*>(pc)));
          jit->SetPC(jit->GetRegister(30));
          return;
        }
        GuestCrash(kSigSegv, pc < 0x10000 ? 1 : 2, pc < 0x10000 ? "SEGV_MAPERR" : "SEGV_ACCERR", pc,
                   "the guest jumped to an address with no code");
      }
      case A64::Exception::Breakpoint:
        GuestCrash(kSigTrap, 1, "TRAP_BRKPT", pc, "BRK (__builtin_trap)");
      // Hints: carry on at pc + 4 (see WFE / SEV above).
      case A64::Exception::WaitForInterrupt:
      case A64::Exception::WaitForEvent:
        counters_.hints.Bump();
        WaitForEvent(pc);
        return;
      case A64::Exception::SendEvent:
        counters_.hints.Bump();
        event_ = true;  // SEV sets every PE's event register, this one's too
        SignalEvent();
        return;
      case A64::Exception::SendEventLocal:
        counters_.hints.Bump();
        event_ = true;
        return;
      case A64::Exception::Yield:
        counters_.hints.Bump();
        YieldProcessor();
        return;
      default: {
        uint32_t insn = 0;
        if (InGuestCode(pc)) std::memcpy(&insn, reinterpret_cast<void*>(pc), 4);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "undefined instruction 0x%08x (exception %d)", insn,
                      static_cast<int>(ex));
        GuestCrash(kSigIll, 1, "ILL_ILLOPC", pc, detail);
      }
    }
  }

  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return static_cast<uint64_t>(-1) / 2; }
  // MRS CNTPCT_EL0: a real, monotonic count at CNTFRQ_EL0 (it was always 0, so
  // a guest waiting for it to advance spun forever).
  std::uint64_t GetCNTPCT() override { return QpcNow(); }

  // WFE (see WFE / SEV above). Returns when there is an event, or after a
  // bounded poll or block: the guest re-checks its condition after every WFE,
  // as it must on hardware.
  void WaitForEvent(uint64_t pc) {
    const volatile uint64_t* slot = Dynarmic::GetExclusiveMonitorAddressPointer(
        &Monitor(), ids_[depth_ > 0 ? depth_ - 1 : 0]);
    auto event = [&](uint64_t reserved, uint64_t sevs) {
      return *slot != reserved || g_sev_count.load(std::memory_order_seq_cst) != sevs;
    };
    const uint64_t sevs = g_sev_count.load(std::memory_order_seq_cst);
    const uint64_t reserved = *slot;
    if (event_ || sevs != seen_sevs_ || reserved == kNoReservation) {
      event_ = false;
      seen_sevs_ = sevs;
      wait_pc_ = 0;  // the wait (if any) is over
      return;
    }
    const uint64_t now = QpcNow();
    // A new wait: another WFE, or this one after a gap (the guest did more).
    if (pc != wait_pc_ || now - last_wfe_ > QpcFrequency() / 500) {
      wait_pc_ = pc;
      wait_start_ = now;
    }
    last_wfe_ = now;
    if (now - wait_start_ < QpcFrequency() / 1000) {  // the first ~1 ms: poll
      for (int i = 0; i < 64; ++i) {
        if (event(reserved, sevs)) return;
        YieldProcessor();
      }
      SwitchToThread();
      return;
    }
    counters_.waits.Bump();
    WfeBucket& b = BucketFor(reserved);
    g_wfe_waiters.fetch_add(1, std::memory_order_seq_cst);
    b.waiters.fetch_add(1, std::memory_order_seq_cst);
    uint64_t compare = b.epoch.load(std::memory_order_seq_cst);
    if (!event(reserved, sevs)) WaitOnAddress(&b.epoch, &compare, sizeof(compare), 1);
    b.waiters.fetch_sub(1, std::memory_order_seq_cst);
    g_wfe_waiters.fetch_sub(1, std::memory_order_seq_cst);
    last_wfe_ = QpcNow();
  }

  // Runs guest `fn` on this thread with X0..X7 = gpr, V0..V7 = xmm and
  // `nstack` words of stack arguments, on the Jit for the current nesting
  // depth, below whatever guest frames are live. Returns X0; X1 and V0 are
  // kept for the caller.
  uint64_t Enter(const void* fn, const uint64_t gpr[8], const uint64_t xmm[8],
                 const uint64_t* stack_args, size_t nstack) {
    HostFenvScope host_fenv;
    A64::Jit* parent = CurrentJit();
    uint32_t fpcr = parent != nullptr ? parent->GetFpcr() : fpcr_;
    uint32_t fpsr = parent != nullptr ? parent->GetFpsr() : fpsr_;
    A64::Jit* jit = JitAt(depth_);
    jit->SetFpcr(fpcr);
    jit->SetFpsr(fpsr);
    uint64_t saved_stack_cur = stack_cur_;
    uint64_t sp = (stack_cur_ - 256 - nstack * 8) & ~uint64_t{15};
    if (sp < reinterpret_cast<uint64_t>(stack_base_) + 0x20000) {
      GuestCrash(kSigSegv, 2, "SEGV_ACCERR", sp, "guest stack exhausted (nested callbacks)");
    }
    if (nstack != 0) std::memcpy(reinterpret_cast<void*>(sp), stack_args, nstack * 8);
    ++depth_;
    for (int i = 0; i < 8; ++i) {
      jit->SetRegister(i, gpr[i]);
      jit->SetVector(i, A64::Vector{xmm[i], 0});
    }
    jit->SetRegister(29, 0);
    jit->SetRegister(30, g_return_stub);
    jit->SetSP(sp);
    jit->SetPC(reinterpret_cast<uint64_t>(fn));
    RunGuest(jit);
    uint64_t result = jit->GetRegister(0);
    last_x1_ = jit->GetRegister(1);
    last_fp_result_ = jit->GetVector(0)[0];
    fpcr_ = jit->GetFpcr();
    fpsr_ = jit->GetFpsr();
    --depth_;
    if (parent != nullptr) {
      parent->SetFpcr(fpcr_);
      parent->SetFpsr(fpsr_);
    }
    stack_cur_ = saved_stack_cur;
    return result;
  }

  // A guest program's _start (ApiRunProgram): SP = the Linux initial stack
  // block, copied to the top of this thread's guest stack.
  uint64_t EnterProgram(const void* entry, const uint64_t* block, size_t words) {
    HostFenvScope host_fenv;
    uint64_t sp = (stack_top_ - 64 - words * 8) & ~uint64_t{15};
    std::memcpy(reinterpret_cast<void*>(sp), block, words * 8);
    stack_cur_ = sp;
    uint64_t gpr[8] = {0}, xmm[8] = {0};
    // Enter() places SP below stack_cur_; _start wants SP == the block.
    A64::Jit* jit = JitAt(depth_);
    jit->SetFpcr(fpcr_);
    jit->SetFpsr(fpsr_);
    ++depth_;
    for (int i = 0; i < 8; ++i) {
      jit->SetRegister(i, gpr[i]);
      jit->SetVector(i, A64::Vector{xmm[i], 0});
    }
    jit->SetRegister(29, 0);
    jit->SetRegister(30, g_return_stub);
    jit->SetSP(sp);
    jit->SetPC(reinterpret_cast<uint64_t>(entry));
    RunGuest(jit);
    fpcr_ = jit->GetFpcr();
    fpsr_ = jit->GetFpsr();
    --depth_;
    return jit->GetRegister(0);
  }

  uint64_t last_fp_result() const { return last_fp_result_; }
  uint64_t last_x1() const { return last_x1_; }
  bool OnGuestStack(uint64_t a) const {
    return a >= reinterpret_cast<uint64_t>(stack_base_) && a < stack_top_;
  }
  bool InStackGuard(uint64_t a) const {
    return a >= reinterpret_cast<uint64_t>(stack_base_) &&
           a < reinterpret_cast<uint64_t>(stack_base_) + kGuardSize;
  }
  void* host_fn() const { return host_fn_; }
  uint64_t host_caller() const { return host_caller_; }

 private:
  struct PendingHostCall {
    void* fn = nullptr;
    int va_index = -1;
    bool syscall = false;
  };

  void QueueHostCall(A64::Jit* jit, void* fn, int va_index) {
    pending_call_ = {fn, va_index, false};
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
  }

  void RunGuest(A64::Jit* jit) {
    for (;;) {
      pending_call_ = {};
      jit->ClearHalt(Dynarmic::HaltReason::UserDefined1 | Dynarmic::HaltReason::UserDefined2);
      int saved_running = t_guest_running;
      t_guest_running = 1;
      counters_.runs.Bump();
      A64::Jit* saved_jit = running_jit_.exchange(jit, std::memory_order_relaxed);
      jit->Run();
      running_jit_.store(saved_jit, std::memory_order_relaxed);
      t_guest_running = saved_running;
      // SVC saves its following PC; NoExecuteFault saves X30. Returning from
      // Run restores host MXCSR before executing arbitrary native host code.
      // A fresh dispatch then uses the actual (possibly changed) guest FPCR.
      PendingHostCall pending = pending_call_;
      if (pending.syscall) {
        HostFenvScope host_fenv;
        uint64_t args[6];
        for (int i = 0; i < 6; ++i) args[i] = jit->GetRegister(i);
        counters_.syscalls.Bump();
        last_syscall_.store(jit->GetRegister(8), std::memory_order_relaxed);
        int64_t result = GuestRawSyscall(jit->GetRegister(8), args);
        jit->SetRegister(0, static_cast<uint64_t>(result));
      } else if (pending.fn != nullptr) {
        CallHost(jit, pending.fn, pending.va_index);
      } else {
        return;
      }
    }
  }

  static constexpr size_t kStackSize = 8 << 20;
  static constexpr size_t kGuardSize = 0x10000;

  void CallHost(A64::Jit* jit, void* fn, int va_index) {
    HostFenvScope host_fenv;
    // AAPCS64 -> System V. X0..X5 are the first six System V integer
    // arguments; X6, X7 are System V arguments 7 and 8, which go on the stack;
    // any further arguments (a variadic call with more than eight) are on the
    // guest stack at SP and follow on the host stack. Copying a fixed window
    // of guest stack words covers them without knowing the real count (the
    // callee reads only what its signature or format string names).
    uint64_t gpr[6], xmm[8];
    for (int i = 0; i < 6; ++i) gpr[i] = jit->GetRegister(i);
    for (int i = 0; i < 8; ++i) xmm[i] = jit->GetVector(i)[0];
    uint64_t stack[2 + 14];
    stack[0] = jit->GetRegister(6);
    stack[1] = jit->GetRegister(7);
    uint64_t gsp = jit->GetSP();
    std::memcpy(&stack[2], reinterpret_cast<void*>(gsp), 14 * 8);
    VaListStorage va;
    if (va_index >= 0 && va_index < 8) {
      uint64_t& slot = va_index < 6 ? gpr[va_index] : stack[va_index - 6];
      if (slot != 0) {
        char trace[2] = {};
        if (GetEnvironmentVariableA("GESHER_ARM64_VARARGS_TRACE", trace, sizeof(trace)) != 0 &&
            trace[0] == '1') {
          const auto* a = reinterpret_cast<const Aapcs64VaList*>(slot);
          const int remaining_gp = a->gr_offs < 0 ? -a->gr_offs / 8 : 0;
          const int remaining_fp = a->vr_offs < 0 ? -a->vr_offs / 16 : 0;
          const auto source = reinterpret_cast<uint64_t>(a->stack);
          const uint64_t bytes = 0;  // Conversion now reads metadata only.
          std::fprintf(stderr,
                       "ARM_VARARGS_BOUNDS sp=%llx stack=%llx top=%llx base=%llx "
                       "gp=%d fp=%d eager_bytes=%llu eager_end=%llx\n",
                       static_cast<unsigned long long>(gsp),
                       static_cast<unsigned long long>(source),
                       static_cast<unsigned long long>(stack_top_),
                       static_cast<unsigned long long>(reinterpret_cast<uint64_t>(stack_base_)),
                       remaining_gp, remaining_fp, static_cast<unsigned long long>(bytes),
                       static_cast<unsigned long long>(source + bytes));
          std::fflush(stderr);
        }
        ConvertVaList(reinterpret_cast<const Aapcs64VaList*>(slot), &va);
        slot = reinterpret_cast<uint64_t>(&va.tag);
      }
    }
    HostCall call{fn, gpr, xmm, stack, 16, 0, 0};
    // A host->guest call made by this host function runs below the guest's
    // live frames.
    uint64_t saved_stack_cur = stack_cur_;
    stack_cur_ = gsp;
    // For fault reports: which host function the guest is in, called from where.
    void* saved_host_fn = host_fn_;
    uint64_t saved_host_caller = host_caller_;
    host_fn_ = fn;
    host_caller_ = jit->GetRegister(30);
    counters_.host_calls.Bump();
    void* saved_sampled_fn = sampled_host_fn_.exchange(fn, std::memory_order_relaxed);
    int saved_running = t_guest_running;
    t_guest_running = 0;
    uint64_t rax = gesher_call_host_sysv(&call);
    t_guest_running = saved_running;
    sampled_host_fn_.store(saved_sampled_fn, std::memory_order_relaxed);
    host_fn_ = saved_host_fn;
    host_caller_ = saved_host_caller;
    stack_cur_ = saved_stack_cur;
    jit->SetRegister(0, rax);
    jit->SetRegister(1, call.out_rdx);
    jit->SetVector(0, A64::Vector{call.out_xmm0, 0});
  }

  std::vector<std::unique_ptr<A64::Jit>> jits_;
  PendingHostCall pending_call_;
  uint32_t fpcr_ = 0;
  uint32_t fpsr_ = 0;
  size_t depth_ = 0;
  uint8_t* tls_block_ = nullptr;
  uint64_t tls_pointer_ = 0;
  uint8_t* stack_base_ = nullptr;
  uint64_t stack_top_ = 0;
  uint64_t stack_cur_ = 0;
  uint64_t last_fp_result_ = 0;
  uint64_t last_x1_ = 0;
  void* host_fn_ = nullptr;    // the host function of the innermost guest -> host call
  uint64_t host_caller_ = 0;   // its guest return address (X30)
  std::vector<size_t> ids_;    // jits_[i]'s exclusive monitor processor id
  bool used_before_ = false;   // a thread has had it (its TLS block is not fresh)
  bool event_ = false;         // the WFE event register (SEVL / SEV)
  uint64_t seen_sevs_ = 0;     // the SEV count this thread's last WFE consumed
  uint64_t wait_pc_ = 0;       // the WFE of the current wait, 0 if none
  uint64_t wait_start_ = 0;    // when that wait began (QPC)
  uint64_t last_wfe_ = 0;

 public:
  // For the profiler (another thread reads these while this one runs).
  ThreadCounters counters_;
  std::atomic<A64::Jit*> running_jit_{nullptr};  // the Jit in Run(), if any
  std::atomic<void*> sampled_host_fn_{nullptr};  // the host function called from guest code
  std::atomic<uint64_t> last_syscall_{~uint64_t{0}};
  HANDLE thread_ = nullptr;  // only while profiling
  DWORD tid_ = 0;
};

// Envs of exited threads, for new threads to reuse: their Jits keep the code
// they translated (thread pools run the same guest code thread after thread),
// their stacks and TLS blocks are allocated already. A few only: each holds
// a code cache's resident pages. GESHER_ARM64_ENV_POOL=<n> (0 disables).
std::mutex g_env_pool_mutex;
std::vector<Env*> g_env_pool;
std::atomic<uint64_t> g_envs_created{0}, g_envs_reused{0};  // for the profiler

size_t EnvPoolLimit() {
  static const size_t limit = [] {
    char v[8] = {};
    DWORD n = GetEnvironmentVariableA("GESHER_ARM64_ENV_POOL", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return size_t{8};
    const int limit = std::atoi(v);
    return static_cast<size_t>(limit < 0 ? 0 : limit > 64 ? 64 : limit);
  }();
  return limit;
}

struct EnvRelease {
  void operator()(Env* env) const {
    env->Detach();
    if (env->Reusable()) {
      std::lock_guard<std::mutex> lock(g_env_pool_mutex);
      if (g_env_pool.size() < EnvPoolLimit()) {
        env->Park();
        g_env_pool.push_back(env);
        return;
      }
    }
    delete env;
  }
};
thread_local std::unique_ptr<Env, EnvRelease> t_env;

Env* ThreadEnv() {
  if (!t_env) {
    Env* env = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_env_pool_mutex);
      if (!g_env_pool.empty()) {
        env = g_env_pool.back();
        g_env_pool.pop_back();
      }
    }
    if (env != nullptr && !env->Unpark()) {
      delete env;
      env = nullptr;
    }
    if (env == nullptr) {
      env = new Env();
      g_envs_created.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_envs_reused.fetch_add(1, std::memory_order_relaxed);
    }
    env->Attach();
    t_env.reset(env);
    t_env_raw = env;
    if (ProfilePeriodMs() > 0) StartProfiler();
  }
  return t_env.get();
}

uint64_t EnterGuest(const void* fn, const uint64_t gpr[8], const uint64_t xmm[8],
                    const uint64_t* stack_args, size_t nstack) {
  return ThreadEnv()->Enter(fn, gpr, xmm, stack_args, nstack);
}

// --- host_entry stubs --------------------------------------------------------
std::mutex g_entry_mutex;
std::unordered_map<const void*, void*> g_entries;
uint8_t* g_entry_page = nullptr;
size_t g_entry_used = 0;
size_t g_entry_cap = 0;

void* MakeHostEntry(const void* fn) {
  std::lock_guard<std::mutex> lock(g_entry_mutex);
  auto it = g_entries.find(fn);
  if (it != g_entries.end()) return it->second;
  constexpr size_t kStub = 24;  // mov r10, imm64; mov rax, imm64; jmp rax
  if (g_entry_page == nullptr || g_entry_used + kStub > g_entry_cap) {
    g_entry_cap = 0x10000;
    g_entry_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, g_entry_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_entry_used = 0;
    if (g_entry_page == nullptr) return nullptr;
  }
  uint8_t* p = g_entry_page + g_entry_used;
  g_entry_used += kStub;
  uint64_t f = reinterpret_cast<uint64_t>(fn);
  uint64_t shim = reinterpret_cast<uint64_t>(&gesher_guest_entry_shim);
  p[0] = 0x49; p[1] = 0xBA;              // mov r10, imm64(fn)
  std::memcpy(p + 2, &f, 8);
  p[10] = 0x48; p[11] = 0xB8;            // mov rax, imm64(shim)
  std::memcpy(p + 12, &shim, 8);
  p[20] = 0xFF; p[21] = 0xE0;            // jmp rax
  p[22] = 0x90; p[23] = 0x90;
  FlushInstructionCache(GetCurrentProcess(), p, kStub);
  g_entries[fn] = p;
  return p;
}

// --- acquire loads on read-only memory ---------------------------------------
// dynarmic emits a fastmem acquire load (LDAR, LDARB, LDARH, LDAPR*, and the
// ordered 128-bit read) as a locked read-modify-write that leaves memory as it
// was: `lock xadd [m], r` with r zeroed, or `lock cmpxchg16b [m]` with
// rdx:rax = rcx:rbx = 0 (src/dynarmic/backend/x64/emit_x64_memory.h,
// EmitReadMemoryMov). A locked instruction always performs its write cycle, so
// on a page the guest may only read (Dart's write-protected VM isolate heap,
// .rodata, any PROT_READ mapping) the guest's load faults as a write, where
// the AArch64 load succeeds. CompleteOrderedLoad finishes such a load in the
// handler: it reads the value, puts it where the instruction would, and
// resumes after the instruction. Ordered stores are `xchg`, so the plain read
// done here keeps the acquire/RCsc ordering the guest asked for.
enum class OrderedLoad {
  kNone,        // not one of dynarmic's ordered-load idioms
  kCompleted,   // done; resume after the instruction
  kUnreadable,  // an ordered load of memory the guest may not read either
};

DWORD64* GprOf(CONTEXT* c, unsigned index) {
  switch (index) {
    case 0: return &c->Rax;
    case 1: return &c->Rcx;
    case 2: return &c->Rdx;
    case 3: return &c->Rbx;
    case 4: return &c->Rsp;
    case 5: return &c->Rbp;
    case 6: return &c->Rsi;
    case 7: return &c->Rdi;
    case 8: return &c->R8;
    case 9: return &c->R9;
    case 10: return &c->R10;
    case 11: return &c->R11;
    case 12: return &c->R12;
    case 13: return &c->R13;
    case 14: return &c->R14;
    case 15: return &c->R15;
    default: return nullptr;
  }
}

// [addr, addr + size) is committed and readable (whatever its write access).
bool ReadableRange(uintptr_t addr, size_t size) {
  const uintptr_t end = addr + size;
  if (end < addr) return false;
  for (uintptr_t at = addr; at < end;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;
    const DWORD p = mbi.Protect & 0xFF;
    if (p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_WRITECOPY &&
        p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE && p != PAGE_EXECUTE_WRITECOPY) {
      return false;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= at) return false;
    at = next;
  }
  return true;
}

template <typename T>
T LoadAt(uintptr_t a) {
  if (a % sizeof(T) == 0) return __atomic_load_n(reinterpret_cast<const T*>(a), __ATOMIC_SEQ_CST);
  T v;
  std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
  return v;
}

// A write fault at `fault` (ExceptionInformation[1]) with context `c`.
OrderedLoad CompleteOrderedLoad(CONTEXT* c, uintptr_t fault) {
  // Only dynarmic's generated code (private executable memory): never an
  // instruction of this DLL or of any other image.
  MEMORY_BASIC_INFORMATION jit_code;
  if (VirtualQuery(reinterpret_cast<void*>(c->Rip), &jit_code, sizeof(jit_code)) == 0 ||
      jit_code.State != MEM_COMMIT || jit_code.Type != MEM_PRIVATE) {
    return OrderedLoad::kNone;
  }
  // Each byte is read only once the bytes before it say the instruction goes
  // on, so nothing past the faulting instruction is touched.
  const auto* ip = reinterpret_cast<const uint8_t*>(c->Rip);
  size_t n = 0;
  bool lock = false;
  bool opsize = false;
  for (; n < 4; ++n) {
    if (ip[n] == 0xF0) {
      lock = true;
    } else if (ip[n] == 0x66) {
      opsize = true;
    } else {
      break;
    }
  }
  if (!lock) return OrderedLoad::kNone;
  unsigned rex = 0;
  if ((ip[n] & 0xF0) == 0x40) rex = ip[n++];
  if (ip[n] != 0x0F) return OrderedLoad::kNone;
  const uint8_t opcode = ip[n + 1];
  if (opcode != 0xC0 && opcode != 0xC1 && opcode != 0xC7) return OrderedLoad::kNone;
  n += 2;
  const uint8_t modrm = ip[n++];
  const unsigned mod = modrm >> 6;
  const unsigned reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
  const unsigned rm = modrm & 7;
  if (mod == 3) return OrderedLoad::kNone;  // a register operand

  // The memory operand: [base + index * scale + disp] or [rip + disp32].
  uint64_t ea = 0;
  bool rip_relative = false;
  bool disp32 = mod == 2;
  if (rm == 4) {
    const uint8_t sib = ip[n++];
    const unsigned index = ((sib >> 3) & 7) | ((rex & 2) << 2);
    if (index != 4) ea += *GprOf(c, index) << (sib >> 6);
    if ((sib & 7) == 5 && mod == 0) {
      disp32 = true;  // no base
    } else {
      ea += *GprOf(c, (sib & 7) | ((rex & 1) << 3));
    }
  } else if (rm == 5 && mod == 0) {
    rip_relative = true;
    disp32 = true;
  } else {
    ea += *GprOf(c, rm | ((rex & 1) << 3));
  }
  if (mod == 1) {
    ea += static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(ip[n])));
    n += 1;
  } else if (disp32) {
    int32_t d;
    std::memcpy(&d, ip + n, 4);
    ea += static_cast<uint64_t>(static_cast<int64_t>(d));
    n += 4;
  }
  if (rip_relative) ea += c->Rip + n;

  // The instruction, its size, and that it would leave memory unchanged.
  size_t size;
  if (opcode == 0xC7) {  // cmpxchg16b m128: only /1 with REX.W
    if ((reg & 7) != 1 || (rex & 8) == 0 || opsize) return OrderedLoad::kNone;
    if (c->Rax != 0 || c->Rdx != 0 || c->Rbx != 0 || c->Rcx != 0) return OrderedLoad::kNone;
    size = 16;
  } else if (opcode == 0xC0) {  // xadd m8, r8
    size = 1;
  } else {  // xadd m16/32/64, r
    size = (rex & 8) != 0 ? 8 : opsize ? 2 : 4;
  }
  // xadd's register (an 8-bit one without REX: 4..7 are ah, ch, dh, bh).
  DWORD64* source = nullptr;
  unsigned shift = 0;
  if (opcode != 0xC7) {
    if (size == 1 && rex == 0 && reg >= 4) {
      source = GprOf(c, reg - 4);
      shift = 8;
    } else {
      source = GprOf(c, reg);
    }
    if (source == nullptr) return OrderedLoad::kNone;
    const uint64_t mask = size == 8 ? ~uint64_t{0} : ((uint64_t{1} << (size * 8)) - 1);
    if (((*source >> shift) & mask) != 0) return OrderedLoad::kNone;  // a real add
  }
  if (fault < ea || fault - ea >= size) return OrderedLoad::kNone;
  if (!ReadableRange(static_cast<uintptr_t>(ea), size)) return OrderedLoad::kUnreadable;

  const uintptr_t a = static_cast<uintptr_t>(ea);
  constexpr DWORD kCf = 0x1, kPf = 0x4, kAf = 0x10, kZf = 0x40, kSf = 0x80, kOf = 0x800;
  if (size == 16) {
    // cmpxchg16b: equal to rdx:rax (zero) leaves it and sets ZF; otherwise
    // rdx:rax gets the value and ZF is clear. Either way rdx:rax = memory.
    const uint64_t lo = LoadAt<uint64_t>(a);
    const uint64_t hi = LoadAt<uint64_t>(a + 8);
    c->Rax = lo;
    c->Rdx = hi;
    c->EFlags = (lo == 0 && hi == 0) ? (c->EFlags | kZf) : (c->EFlags & ~kZf);
  } else {
    // xadd with a zero register: the register gets the value; memory and
    // the flags are those of value + 0.
    uint64_t v;
    uint64_t sign;
    switch (size) {
      case 1: v = LoadAt<uint8_t>(a); sign = 0x80; break;
      case 2: v = LoadAt<uint16_t>(a); sign = 0x8000; break;
      case 4: v = LoadAt<uint32_t>(a); sign = 0x80000000u; break;
      default: v = LoadAt<uint64_t>(a); sign = uint64_t{1} << 63; break;
    }
    switch (size) {
      case 1:
        *source = (*source & ~(uint64_t{0xFF} << shift)) | (v << shift);
        break;
      case 2:
        *source = (*source & ~uint64_t{0xFFFF}) | v;
        break;
      default:  // a 32-bit result zero-extends, as on hardware
        *source = v;
        break;
    }
    DWORD flags = c->EFlags & ~(kCf | kPf | kAf | kZf | kSf | kOf);
    if (v == 0) flags |= kZf;
    if ((v & sign) != 0) flags |= kSf;
    if ((__builtin_popcount(static_cast<unsigned>(v & 0xFF)) & 1) == 0) flags |= kPf;
    c->EFlags = flags;
  }
  c->Rip += n;
  return OrderedLoad::kCompleted;
}

// The vectored exception handler, first in the chain.
//  - A host function called a guest function pointer directly (a qsort
//    comparator, a pthread start routine, a key destructor, any callback the
//    guest handed to a host API). Guest code pages are not executable, so the
//    call faults with DEP at the guest address, with the System V arguments
//    in place and the return address on the stack: resuming at the function's
//    host_entry stub makes it exactly a call of the stub.
//  - Any other fault while this thread runs guest code is the guest's: it is
//    reported Android-style and the process ends.
// `addr` for a report: guest object+offset (symbol+n), host module+offset, or raw.
void PrintWhere(const char* label, uint64_t addr) {
  const char* object = nullptr;
  const void* base = nullptr;
  const char* symbol = nullptr;
  const void* symbol_addr = nullptr;
  if (g_host != nullptr && g_host->describe != nullptr &&
      g_host->describe(reinterpret_cast<const void*>(addr), &object, &base, &symbol,
                       &symbol_addr)) {
    std::fprintf(stderr, "%s%s+0x%llx", label, object ? object : "?",
                 static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(base)));
    if (symbol != nullptr) {
      std::fprintf(stderr, " (%s+%llu)", symbol,
                   static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(symbol_addr)));
    }
    std::fprintf(stderr, "\n");
    return;
  }
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &module) &&
      GetModuleFileNameA(module, name, sizeof(name)) != 0) {
    const char* file = std::strrchr(name, '\\');
    std::fprintf(stderr, "%s%s+0x%llx\n", label, file != nullptr ? file + 1 : name,
                 static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(module)));
  } else {
    std::fprintf(stderr, "%s0x%llx\n", label, static_cast<unsigned long long>(addr));
  }
}

// --- the profiler (GESHER_ARM64_PROFILE) ----------------------------------------
struct ProfileSlot {
  uint64_t key = 0;
  uint32_t count = 0;
};
template <size_t N>
void CountSample(std::array<ProfileSlot, N>& slots, uint64_t key) {
  ProfileSlot* least = &slots[0];
  for (ProfileSlot& s : slots) {
    if (s.count != 0 && s.key == key) {
      ++s.count;
      return;
    }
    if (s.count < least->count) least = &s;
  }
  // Space-saving: the least counted slot takes the new key.
  least->key = key;
  ++least->count;
}
template <size_t N>
void PrintTop(const char* what, std::array<ProfileSlot, N>& slots, uint32_t samples, size_t top) {
  std::array<ProfileSlot, N> sorted = slots;
  std::sort(sorted.begin(), sorted.end(),
            [](const ProfileSlot& a, const ProfileSlot& b) { return a.count > b.count; });
  for (size_t i = 0; i < top && i < N && sorted[i].count != 0; ++i) {
    char label[64];
    std::snprintf(label, sizeof(label), "      %3u%% %s ", sorted[i].count * 100 / samples, what);
    PrintWhere(label, sorted[i].key);
  }
}

struct ProfileState {
  DWORD tid = 0;               // the thread these numbers are about
  uint64_t user = 0;           // the thread's user time at the last tick (100 ns)
  uint64_t period_user = 0;    // ... at the start of the report period
  ULONGLONG busy_until = 0;    // sample on ticks until then (user time moved)
  uint64_t base[8] = {};       // the counters at the start of the period
  uint32_t samples = 0, in_guest = 0, in_host = 0;
  std::array<ProfileSlot, 16> pcs{};
  std::array<ProfileSlot, 8> host_fns{};
  std::array<ProfileSlot, 8> rips{};
};

void ReadCounters(Env* env, uint64_t out[8]) {
  const ThreadCounters& c = env->counters_;
  const Counter* all[8] = {&c.host_calls, &c.excl_reads, &c.excl_writes, &c.excl_cas_failed,
                           &c.syscalls,   &c.hints,      &c.waits,       &c.runs};
  for (int i = 0; i < 8; ++i) out[i] = all[i]->Get();
}

uint64_t UserTime(HANDLE thread) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(thread, &c, &e, &k, &u)) return 0;
  return (static_cast<uint64_t>(u.dwHighDateTime) << 32) | u.dwLowDateTime;
}

DWORD WINAPI ProfilerThread(void*) {
  std::unordered_map<Env*, ProfileState> states;
  const ULONGLONG period = static_cast<ULONGLONG>(ProfilePeriodMs());
  ULONGLONG period_start = GetTickCount64();
  for (;;) {
    Sleep(10);
    const ULONGLONG now = GetTickCount64();
    const bool report = now - period_start >= period;
    std::unique_lock<std::mutex> lock(g_envs_mutex);  // an exiting thread waits in Detach
    for (auto it = states.begin(); it != states.end();) {
      if (std::find(g_envs.begin(), g_envs.end(), it->first) == g_envs.end()) {
        it = states.erase(it);
      } else {
        ++it;
      }
    }
    for (Env* env : g_envs) {
      if (env->thread_ == nullptr) continue;
      bool fresh = states.find(env) == states.end();
      ProfileState& st = states[env];  // allocate before suspending anything
      if (!fresh && st.tid != env->tid_) {  // a pooled Env, now another thread's
        st = ProfileState{};
        fresh = true;
      }
      st.tid = env->tid_;
      const uint64_t user = UserTime(env->thread_);
      if (fresh) {
        st.user = st.period_user = user;
        ReadCounters(env, st.base);
        continue;
      }
      if (user != st.user) st.busy_until = now + 50;  // times move in ~15 ms steps
      st.user = user;
      if (now > st.busy_until) continue;
      // No allocation, lock or I/O while the thread is suspended.
      if (SuspendThread(env->thread_) == static_cast<DWORD>(-1)) continue;
      CONTEXT ctx;
      ctx.ContextFlags = CONTEXT_CONTROL;
      const bool have_ctx = GetThreadContext(env->thread_, &ctx) != 0;
      A64::Jit* jit = env->running_jit_.load(std::memory_order_relaxed);
      const uint64_t pc = jit != nullptr ? jit->GetPC() : 0;
      void* host_fn = env->sampled_host_fn_.load(std::memory_order_relaxed);
      ResumeThread(env->thread_);
      ++st.samples;
      if (jit != nullptr) {
        ++st.in_guest;
        CountSample(st.pcs, pc);
      } else if (host_fn != nullptr) {
        ++st.in_host;
        CountSample(st.host_fns, reinterpret_cast<uint64_t>(host_fn));
      }
      if (have_ctx) CountSample(st.rips, ctx.Rip);
    }
    if (!report) continue;
    // Taken under the lock, printed after it: describing an address may wait
    // for the loader, whose holder may be a thread waiting for g_envs_mutex.
    struct Hot {
      DWORD tid;
      double cpu;
      uint64_t delta[8];
      uint64_t last_syscall;
      ProfileState st;
    };
    std::vector<Hot> hot;
    const double seconds = static_cast<double>(now - period_start) / 1000.0;
    period_start = now;
    const size_t threads = g_envs.size();
    for (Env* env : g_envs) {
      auto it = states.find(env);
      if (it == states.end()) continue;
      ProfileState& st = it->second;
      uint64_t c[8];
      ReadCounters(env, c);
      const double cpu = static_cast<double>(st.user - st.period_user) / 1e5 / seconds;  // %
      if (cpu >= 25.0 && st.samples != 0) {
        Hot h{env->tid_, cpu, {}, env->last_syscall_.load(std::memory_order_relaxed), st};
        for (int i = 0; i < 8; ++i) h.delta[i] = c[i] - st.base[i];
        hot.push_back(h);
      }
      st.period_user = st.user;
      std::memcpy(st.base, c, sizeof(c));
      st.samples = st.in_guest = st.in_host = 0;
      st.pcs = {};
      st.host_fns = {};
      st.rips = {};
    }
    lock.unlock();
    size_t pooled;
    {
      std::lock_guard<std::mutex> pool_lock(g_env_pool_mutex);
      pooled = g_env_pool.size();
    }
    std::fprintf(stderr,
                 "gesher arm64 profile: %zu guest threads, %zu Jits; Envs created %llu, reused "
                 "%llu, pooled %zu\n",
                 threads, LiveProcessorIds(),
                 static_cast<unsigned long long>(g_envs_created.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_envs_reused.load(std::memory_order_relaxed)),
                 pooled);
    for (Hot& h : hot) {
      auto rate = [&](uint64_t n) {
        return static_cast<unsigned long long>(static_cast<double>(n) / seconds);
      };
      const uint64_t refused = h.delta[1] > h.delta[2] ? h.delta[1] - h.delta[2] : 0;
      std::fprintf(stderr,
                   "  tid %lu: cpu %.0f%%; per second: host calls %llu, raw syscalls %llu (last "
                   "%lld), exclusive loads %llu, stores %llu (CAS failed %llu, monitor refused "
                   "%llu), hints %llu, WFE waits %llu, Run() %llu; samples %u (guest %u, host "
                   "call %u)\n",
                   h.tid, h.cpu, rate(h.delta[0]), rate(h.delta[4]),
                   h.last_syscall == ~uint64_t{0} ? -1LL : static_cast<long long>(h.last_syscall),
                   rate(h.delta[1]), rate(h.delta[2]), rate(h.delta[3]), rate(refused),
                   rate(h.delta[5]), rate(h.delta[6]), rate(h.delta[7]), h.st.samples,
                   h.st.in_guest, h.st.in_host);
      PrintTop("guest pc  ", h.st.pcs, h.st.samples, 6);
      PrintTop("host call ", h.st.host_fns, h.st.samples, 4);
      PrintTop("host rip  ", h.st.rips, h.st.samples, 4);
    }
    std::fflush(stderr);
  }
}

void StartProfiler() {
  static std::once_flag once;
  std::call_once(once, [] {
    HANDLE h = CreateThread(nullptr, 0, ProfilerThread, nullptr, 0, nullptr);
    if (h != nullptr) CloseHandle(h);
    std::fprintf(stderr, "gesher arm64: profiling guest threads every %d ms\n", ProfilePeriodMs());
    std::fflush(stderr);
  });
}

// A fault in host code during a guest -> host call: ART's fatal-signal report
// that may follow has no registers or backtrace on Windows, so say where the
// guest was. A note only (ART may still handle the fault); the first few.
void NoteHostCallFault(const EXCEPTION_RECORD* r, const CONTEXT* c) {
  static std::atomic<int> notes{0};
  Env* env = t_env_raw;
  if (env == nullptr || env->host_fn() == nullptr || notes.fetch_add(1) >= 8) return;
  const char* op = r->ExceptionInformation[0] == 0   ? "reading"
                   : r->ExceptionInformation[0] == 1 ? "writing"
                                                     : "executing";
  std::fprintf(stderr, "gesher arm64: tid %lu: host fault %s 0x%llx in a call from guest code\n",
               GetCurrentThreadId(), op,
               static_cast<unsigned long long>(r->ExceptionInformation[1]));
  PrintWhere("    host pc        ", c->Rip);
  PrintWhere("    host function  ", reinterpret_cast<uint64_t>(env->host_fn()));
  PrintWhere("    called from    ", env->host_caller());
  std::fflush(stderr);
}

LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  CONTEXT* c = info->ContextRecord;
  // The host itself running guest (AArch64) bytes: a guest page became
  // host-executable, so a host call of a guest pointer did not fault into its
  // host_entry stub. Whatever x86 those bytes decode to ends in a fault here.
  auto host_ran_guest_code = [&]() {
    if (t_guest_running || !InGuestCode(c->Rip)) return;
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "the host executed guest code at 0x%llx (exception 0x%08lx): a guest page "
                  "is host-executable (guest mprotect/mmap with PROT_EXEC?)",
                  static_cast<unsigned long long>(c->Rip), r->ExceptionCode);
    PrintWhere("gesher arm64: host executed guest code at ", c->Rip);
    GuestCrash(kSigIll, 1, "ILL_ILLOPC", c->Rip, detail);
  };
  if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
    auto kind = r->ExceptionInformation[0];
    auto addr = static_cast<uintptr_t>(r->ExceptionInformation[1]);
    if (kind == 8 && addr == c->Rip && InGuestCode(addr)) {
      void* stub = MakeHostEntry(reinterpret_cast<void*>(addr));
      if (stub != nullptr) {
        c->Rip = reinterpret_cast<DWORD64>(stub);
        return EXCEPTION_CONTINUE_EXECUTION;
      }
    }
    if (!(kind == 8 && addr == c->Rip)) host_ran_guest_code();
    if (!t_guest_running && addr < 0x10000) NoteHostCallFault(r, c);
    if (t_guest_running) {
      bool mapped = false;
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != 0) {
        mapped = mbi.State == MEM_COMMIT;
      }
      const char* detail = kind == 1 ? "guest write" : kind == 8 ? "guest execute" : "guest read";
      if (kind == 1) {
        // An acquire load that faulted as a write (see CompleteOrderedLoad).
        switch (CompleteOrderedLoad(c, addr)) {
          case OrderedLoad::kCompleted:
            return EXCEPTION_CONTINUE_EXECUTION;
          case OrderedLoad::kUnreadable:
            detail = "guest read";
            break;
          case OrderedLoad::kNone:
            break;
        }
      }
      if (t_env_raw != nullptr && t_env_raw->InStackGuard(addr)) detail = "guest stack overflow";
      GuestCrash(kSigSegv, mapped ? 2 : 1, mapped ? "SEGV_ACCERR" : "SEGV_MAPERR", addr, detail);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }
  if (r->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
      r->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
    host_ran_guest_code();
  }
  if (t_guest_running) {
    switch (r->ExceptionCode) {
      case EXCEPTION_STACK_OVERFLOW:
        GuestCrash(kSigSegv, 2, "SEGV_ACCERR", 0, "host stack overflow in guest code");
      case EXCEPTION_ILLEGAL_INSTRUCTION:
      case EXCEPTION_PRIV_INSTRUCTION:
        GuestCrash(kSigIll, 1, "ILL_ILLOPC", 0, "illegal instruction in translated code");
      default:
        break;
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// --- the API table -----------------------------------------------------------
void ApiAddCode(const void* start, size_t size, const char* /*name*/) {
  AddRegion(reinterpret_cast<uintptr_t>(start), size);
}

uint64_t ApiCall(const void* fn, const uint64_t* args, size_t nargs) {
  uint64_t gpr[8] = {0}, xmm[8] = {0};
  for (size_t i = 0; i < nargs && i < 8; ++i) gpr[i] = args[i];
  return EnterGuest(fn, gpr, xmm, nullptr, 0);
}

void* ApiHostEntry(const void* fn) { return MakeHostEntry(fn); }

void* ApiGuestSymbol(const char* name) { return GuestSymbol(name); }

void ApiAddVaListFunction(const void* fn, int index) { AddVaListFunction(fn, index); }

// The static TLSDESC resolver (guest code): the descriptor's second word is
// the offset from the thread pointer. `ldr x0, [x0, #8]; ret` — it preserves
// every other register, as the TLSDESC calling convention requires.
void* g_tlsdesc_static = nullptr;
void* ApiTlsDescStatic(void) { return g_tlsdesc_static; }

int ApiIsGuestCode(const void* addr) {
  return InGuestCode(reinterpret_cast<uintptr_t>(addr)) ? 1 : 0;
}

uint64_t ApiRunProgram(const void* entry, const uint64_t* block, size_t words) {
  return ThreadEnv()->EnterProgram(entry, block, words);
}

gesher_arm64_api g_api = {
    GESHER_ARM64_API_VERSION,
    ApiAddCode,
    ApiCall,
    ApiHostEntry,
    ApiGuestSymbol,
    ApiAddVaListFunction,
    ApiTlsRegister,
    ApiTlsPublish,
    ApiTlsDescStatic,
    ApiIsGuestCode,
    ApiRunProgram,
};

}  // namespace

bool InGuestCode(uintptr_t addr) {
  static thread_local CodeRegion last{0, 0};
  if (addr >= last.start && addr < last.end) return true;
  std::shared_lock<std::shared_mutex> lock(g_regions_mutex);
  for (const CodeRegion& r : g_regions) {
    if (addr >= r.start && addr < r.end) {
      last = r;
      return true;
    }
  }
  return false;
}

uint32_t GuestFpcr() { return ThreadEnv()->Fpcr(); }
void SetGuestFpcr(uint32_t value) { ThreadEnv()->SetFpcr(value); }
uint32_t GuestFpsr() { return ThreadEnv()->Fpsr(); }
void SetGuestFpsr(uint32_t value) { ThreadEnv()->SetFpsr(value); }

void AddVaListFunction(const void* fn, int index) {
  if (fn == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_va_mutex);
    g_va_functions[fn] = index;
  }
  // A trampoline already bound to it picks the index up.
  std::lock_guard<std::mutex> lock(g_tramp_mutex);
  auto it = g_tramp_by_fn.find(const_cast<void*>(fn));
  if (it != g_tramp_by_fn.end()) {
    uint32_t svc;
    std::memcpy(&svc, it->second, 4);
    g_tramps[((svc >> 5) & 0xFFFF) - 1].va_list_index = index;
  }
}

void* BindHost(void* fn, int va_list_index) {
  if (fn == nullptr) return nullptr;
  if (va_list_index == -2) va_list_index = VaListIndexOf(fn);
  std::lock_guard<std::mutex> lock(g_tramp_mutex);
  auto it = g_tramp_by_fn.find(fn);
  if (it != g_tramp_by_fn.end()) return it->second;
  size_t index = g_tramp_count.load(std::memory_order_relaxed);
  if (index >= kMaxTramps) return nullptr;
  uint32_t* p = AllocGuestCode(kTrampWords);
  if (p == nullptr) return nullptr;
  g_tramps[index] = {fn, va_list_index};
  p[0] = Svc(static_cast<uint32_t>(index + 1));
  p[1] = kRet;
  for (size_t i = 2; i < kTrampWords; ++i) p[i] = kBrk;
  g_tramp_count.store(index + 1, std::memory_order_release);
  g_tramp_by_fn[fn] = p;
  return p;
}

bool InTranslatorCode(const void* start, size_t size) {
  auto first = static_cast<const uint8_t*>(start);
  std::lock_guard<std::mutex> lock(g_code_mutex);
  for (uint8_t* page : g_code_pages) {
    if (first < page + kCodePage && first + size > page) return true;
  }
  return false;
}

void AddGuestCode(const void* start, size_t size) {
  if (size == 0) return;
  uintptr_t first = reinterpret_cast<uintptr_t>(start) & ~uintptr_t{0xFFF};
  uintptr_t end = (reinterpret_cast<uintptr_t>(start) + size + 0xFFF) & ~uintptr_t{0xFFF};
  {
    std::shared_lock<std::shared_mutex> lock(g_regions_mutex);
    for (const CodeRegion& r : g_regions) {
      if (first >= r.start && end <= r.end) return;  // already guest code
    }
  }
  AddRegion(first, end - first);
}

void* EmitGuestCode(const uint32_t* insns, size_t count) {
  uint32_t* p = AllocGuestCode(count);
  if (p != nullptr) std::memcpy(p, insns, count * 4);
  return p;
}

[[noreturn]] void GuestCrash(int signo, int code, const char* code_name, uint64_t fault_addr,
                             const char* detail) {
  static std::atomic<int> crashing{0};
  if (crashing.fetch_add(1) != 0) {
    // Another thread is already reporting; let it finish.
    Sleep(INFINITE);
  }
  const char* name = signo == kSigSegv ? "SIGSEGV"
                     : signo == kSigIll ? "SIGILL"
                     : signo == kSigTrap ? "SIGTRAP"
                     : signo == kSigSys ? "SIGSYS"
                                       : "SIGABRT";
  std::fprintf(stderr,
               "Fatal signal %d (%s), code %d (%s), fault addr 0x%llx in tid %lu (arm64 guest)\n",
               signo, name, code, code_name, static_cast<unsigned long long>(fault_addr),
               GetCurrentThreadId());
  std::fprintf(stderr, "    Cause: %s\n", detail);
  if (signo == kSigSegv) {
    // What is at the fault address: which allocation, and how it is protected.
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(fault_addr), &mbi, sizeof(mbi)) != 0) {
      std::fprintf(stderr,
                   "    memory: allocation %p (protect 0x%lx), region %p+0x%llx, state 0x%lx, "
                   "protect 0x%lx, type 0x%lx\n",
                   mbi.AllocationBase, mbi.AllocationProtect, mbi.BaseAddress,
                   static_cast<unsigned long long>(mbi.RegionSize), mbi.State, mbi.Protect,
                   mbi.Type);
    }
  }
  std::fflush(stderr);
  Env* env = t_env_raw;
  if (env != nullptr && env->host_fn() != nullptr && !t_guest_running) {
    PrintWhere("    in host function ", reinterpret_cast<uint64_t>(env->host_fn()));
    PrintWhere("    called from guest ", env->host_caller());
  }
  A64::Jit* jit = env != nullptr ? env->CurrentJit() : nullptr;
  if (jit != nullptr) {
    // dynarmic keeps the PC per block, so `pc` is the start of the faulting
    // block (or the instruction itself for an undefined one).
    uint64_t frames[16];
    int n = 0;
    // The registers, as debuggerd prints them (as of the block's last sync).
    for (int r = 0; r < 30; r += 2) {
      std::fprintf(stderr, "    x%-2d %016llx  x%-2d %016llx\n", r,
                   static_cast<unsigned long long>(jit->GetRegister(r)), r + 1,
                   static_cast<unsigned long long>(jit->GetRegister(r + 1)));
    }
    std::fprintf(stderr, "    lr  %016llx  sp  %016llx  pc  %016llx\n",
                 static_cast<unsigned long long>(jit->GetRegister(30)),
                 static_cast<unsigned long long>(jit->GetSP()),
                 static_cast<unsigned long long>(jit->GetPC()));
    frames[n++] = jit->GetPC();
    frames[n++] = jit->GetRegister(30);
    uint64_t fp = jit->GetRegister(29);
    while (n < 16 && fp != 0 && (fp & 7) == 0 && env->OnGuestStack(fp)) {
      uint64_t next, lr;
      std::memcpy(&next, reinterpret_cast<void*>(fp), 8);
      std::memcpy(&lr, reinterpret_cast<void*>(fp + 8), 8);
      if (lr == 0) break;
      frames[n++] = lr;
      if (next <= fp) break;
      fp = next;
    }
    std::fprintf(stderr, "backtrace (guest, approximate):\n");
    for (int i = 0; i < n; ++i) {
      const char* object = nullptr;
      const void* base = nullptr;
      const char* symbol = nullptr;
      const void* symbol_addr = nullptr;
      if (g_host != nullptr && g_host->describe != nullptr &&
          g_host->describe(reinterpret_cast<const void*>(frames[i]), &object, &base, &symbol,
                           &symbol_addr)) {
        std::fprintf(stderr, "      #%02d pc %016llx  %s", i,
                     static_cast<unsigned long long>(frames[i] - reinterpret_cast<uint64_t>(base)),
                     object ? object : "?");
        if (symbol != nullptr) {
          std::fprintf(stderr, " (%s+%llu)", symbol,
                       static_cast<unsigned long long>(frames[i] -
                                                       reinterpret_cast<uint64_t>(symbol_addr)));
        }
        std::fprintf(stderr, "\n");
      } else {
        std::fprintf(stderr, "      #%02d pc %016llx  <unknown>\n", i,
                     static_cast<unsigned long long>(frames[i]));
      }
    }
  }
  std::fflush(stderr);
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 128 + signo);
  for (;;) Sleep(INFINITE);
}

}  // namespace gesher_arm64

using namespace gesher_arm64;

namespace {

// How many System V stack words (after the two that become X6, X7) a
// host_entry call copies to the guest stack. The stub does not know the
// callee's signature, so it copies a window large enough for any JNI native:
// a Java method has at most 255 parameter slots, so with JNIEnv* and the
// jclass/jobject at most 249 arguments are left after X0..X7 (and V0..V7).
// Flutter's FlutterJNI.nativeSetViewportMetrics(long, float, 15 x int,
// 3 x int[], 4 x int) alone has 17 AAPCS64 stack arguments: a window of 8
// gave the guest garbage (null) for its int[] arguments.
constexpr size_t kEntryStackWords = 256;

// The words of the System V caller's stack arguments that can be read at
// `stack`: kEntryStackWords, or fewer near the top of the thread's stack (the
// window must not run past it). The words beyond the real arguments are the
// caller's frames; the guest callee never reads them.
size_t EntryStackWords(const uint64_t* stack) {
  const auto s = reinterpret_cast<uintptr_t>(stack);
  const auto* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  uintptr_t end = 0;
  if (s >= reinterpret_cast<uintptr_t>(tib->StackLimit) &&
      s < reinterpret_cast<uintptr_t>(tib->StackBase)) {
    end = reinterpret_cast<uintptr_t>(tib->StackBase);
  } else {
    // Not the thread's Windows stack (a fiber, a private stack): the
    // committed readable region holding the arguments.
    MEMORY_BASIC_INFORMATION mbi;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
    if (VirtualQuery(stack, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
        (mbi.Protect & kReadable) != 0 && (mbi.Protect & PAGE_GUARD) == 0) {
      end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
  }
  if (end <= s) return 0;
  size_t words = (end - s) / 8;
  return words < kEntryStackWords ? words : kEntryStackWords;
}

}  // namespace

// Called (Microsoft ABI) by gesher_guest_entry_shim with the guest function
// (from the host_entry stub's r10) and the System V call's arguments.
//
// System V passes integer arguments 1-6 and FP arguments 1-8 in registers and
// every further one on the stack in argument order, one 8-byte slot each;
// AAPCS64 on Linux/Android passes integer arguments 1-8 and FP arguments 1-8
// in registers and the rest the same way (8-byte slots, argument order; not
// Apple's packed layout). So System V stack words 0 and 1 are X6 and X7 (the
// shim's gpr[6..7]) and words 2.. are the guest's stack arguments 0.., as
// long as no FP argument overflows V7 before the 8th integer argument.
extern "C" uint64_t gesher_guest_entry_trampoline(const void* fn, EntryFrame* frame) {
  uint64_t r =
      EnterGuest(fn, frame->gpr, frame->xmm, frame->stack, EntryStackWords(frame->stack));
  frame->out_rdx = t_env->last_x1();
  frame->out_xmm0 = t_env->last_fp_result();
  return r;
}

// A guest trampoline (guest code) that calls host function `fn`; the loader
// binds a guest import's GOT entry to this address.
extern "C" __declspec(dllexport) void* gesher_arm64_bind(void* fn) { return BindHost(fn); }

extern "C" __declspec(dllexport) const gesher_arm64_api* gesher_arm64_init(
    const gesher_arm64_host* host) {
  // API3 changes the private va_list representation consumed by the host
  // shim. Reject old hosts before resolving any shim symbol or running code.
  if (host == nullptr || host->version != GESHER_ARM64_API_VERSION) {
    return nullptr;
  }
  g_host = host;
  g_api.version = host->version;
  {
    uint32_t ret_stub[] = {Svc(kReturnSvc), kRet};
    g_return_stub = reinterpret_cast<uintptr_t>(EmitGuestCode(ret_stub, 2));
    uint32_t tlsdesc[] = {0xF9400400u /* ldr x0, [x0, #8] */, kRet};
    g_tlsdesc_static = EmitGuestCode(tlsdesc, 2);
  }
  InitGuestLibc();
  // First in the chain, so an execute fault in guest code (a host call to a
  // guest callback) is redirected before gesher-base turns it into a signal.
  AddVectoredExceptionHandler(1, VectoredHandler);
  return &g_api;
}
