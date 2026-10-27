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

#include <array>
#include <atomic>
#include <cstdint>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

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

uint32_t* AllocGuestCode(size_t words) {
  std::lock_guard<std::mutex> lock(g_code_mutex);
  size_t bytes = words * 4;
  if (g_code_page == nullptr || g_code_used + bytes > kCodePage) {
    g_code_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kCodePage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_code_used = 0;
    if (g_code_page == nullptr) return nullptr;
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

Dynarmic::ExclusiveMonitor& Monitor() {
  static Dynarmic::ExclusiveMonitor m{64};
  return m;
}

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

class Env final : public A64::UserCallbacks {
 public:
  Env() {
    // This thread's guest TLS block (its thread pointer), with the images of
    // the modules loaded so far.
    tls_block_ = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kTlsBlockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    tls_pointer_ = reinterpret_cast<uint64_t>(tls_block_);
    {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      g_tls_blocks.push_back(tls_block_);
      for (const TlsImage& ti : g_tls_images) {
        std::memset(tls_block_ + ti.offset, 0, ti.total_size);
        if (ti.image) std::memcpy(tls_block_ + ti.offset, ti.image, ti.image_size);
      }
    }
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
    {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      for (size_t i = 0; i < g_tls_blocks.size(); ++i) {
        if (g_tls_blocks[i] == tls_block_) {
          g_tls_blocks.erase(g_tls_blocks.begin() + i);
          break;
        }
      }
    }
    if (t_env_raw == this) t_env_raw = nullptr;
    jits_.clear();
    VirtualFree(tls_block_, 0, MEM_RELEASE);
    VirtualFree(stack_base_, 0, MEM_RELEASE);
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
      conf.processor_id = next_id_.fetch_add(1) & 63;
      conf.global_monitor = &Monitor();
      conf.enable_cycle_counting = false;
      conf.optimizations = Dynarmic::all_safe_optimizations;
      conf.unsafe_optimizations = false;
      conf.wall_clock_cntpct = true;
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
  std::uint8_t MemoryRead8(uint64_t a) override { return *reinterpret_cast<volatile uint8_t*>(a); }
  std::uint16_t MemoryRead16(uint64_t a) override { uint16_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 2); return v; }
  std::uint32_t MemoryRead32(uint64_t a) override { uint32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; }
  std::uint64_t MemoryRead64(uint64_t a) override { uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 8); return v; }
  A64::Vector MemoryRead128(uint64_t a) override { A64::Vector v; std::memcpy(&v, reinterpret_cast<void*>(a), 16); return v; }
  void MemoryWrite8(uint64_t a, std::uint8_t v) override { *reinterpret_cast<volatile uint8_t*>(a) = v; }
  void MemoryWrite16(uint64_t a, std::uint16_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 2); }
  void MemoryWrite32(uint64_t a, std::uint32_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
  void MemoryWrite64(uint64_t a, std::uint64_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
  void MemoryWrite128(uint64_t a, A64::Vector v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 16); }

  // Store-exclusive (STXR/STLXR/STXP): dynarmic's global monitor checks the
  // reservation and hands the store here with the value LDXR read; a host
  // compare-and-swap makes it atomic against every other thread. (The
  // defaults return false, which made every guest LL/SC loop spin forever.)
  template <typename T>
  static bool Cas(uint64_t a, T value, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, value, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  }
  bool MemoryWriteExclusive8(uint64_t a, std::uint8_t v, std::uint8_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive16(uint64_t a, std::uint16_t v, std::uint16_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive32(uint64_t a, std::uint32_t v, std::uint32_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive64(uint64_t a, std::uint64_t v, std::uint64_t e) override { return Cas(a, v, e); }
  bool MemoryWriteExclusive128(uint64_t a, A64::Vector v, A64::Vector e) override {
    using U128 = unsigned __int128;
    U128 expected = (static_cast<U128>(e[1]) << 64) | e[0];
    U128 value = (static_cast<U128>(v[1]) << 64) | v[0];
    return __atomic_compare_exchange_n(reinterpret_cast<U128*>(a), &expected, value, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);  // cmpxchg16b
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
      case A64::Exception::WaitForInterrupt:
      case A64::Exception::WaitForEvent:
      case A64::Exception::SendEvent:
      case A64::Exception::SendEventLocal:
      case A64::Exception::Yield:
        return;  // hints: carry on at pc + 4
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
  std::uint64_t GetCNTPCT() override { return 0; }

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
    for (size_t i = 0; i < nstack; ++i) {
      std::memcpy(reinterpret_cast<void*>(sp + i * 8), &stack_args[i], 8);
    }
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
      jit->Run();
      t_guest_running = saved_running;
      // SVC saves its following PC; NoExecuteFault saves X30. Returning from
      // Run restores host MXCSR before executing arbitrary native host code.
      // A fresh dispatch then uses the actual (possibly changed) guest FPCR.
      PendingHostCall pending = pending_call_;
      if (pending.syscall) {
        HostFenvScope host_fenv;
        uint64_t args[6];
        for (int i = 0; i < 6; ++i) args[i] = jit->GetRegister(i);
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
    int saved_running = t_guest_running;
    t_guest_running = 0;
    uint64_t rax = gesher_call_host_sysv(&call);
    t_guest_running = saved_running;
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
  static std::atomic<size_t> next_id_;
};

std::atomic<size_t> Env::next_id_{0};
thread_local std::unique_ptr<Env> t_env;

Env* ThreadEnv() {
  if (!t_env) {
    t_env = std::make_unique<Env>();
    t_env_raw = t_env.get();
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

// The vectored exception handler, first in the chain.
//  - A host function called a guest function pointer directly (a qsort
//    comparator, a pthread start routine, a key destructor, any callback the
//    guest handed to a host API). Guest code pages are not executable, so the
//    call faults with DEP at the guest address, with the System V arguments
//    in place and the return address on the stack: resuming at the function's
//    host_entry stub makes it exactly a call of the stub.
//  - Any other fault while this thread runs guest code is the guest's: it is
//    reported Android-style and the process ends.
LONG CALLBACK VectoredHandler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  CONTEXT* c = info->ContextRecord;
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
    if (t_guest_running) {
      bool mapped = false;
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) != 0) {
        mapped = mbi.State == MEM_COMMIT;
      }
      const char* detail = kind == 1 ? "guest write" : kind == 8 ? "guest execute" : "guest read";
      if (t_env_raw != nullptr && t_env_raw->InStackGuard(addr)) detail = "guest stack overflow";
      GuestCrash(kSigSegv, mapped ? 2 : 1, mapped ? "SEGV_ACCERR" : "SEGV_MAPERR", addr, detail);
    }
    return EXCEPTION_CONTINUE_SEARCH;
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
  uint32_t* p = AllocGuestCode(2);
  if (p == nullptr) return nullptr;
  g_tramps[index] = {fn, va_list_index};
  p[0] = Svc(static_cast<uint32_t>(index + 1));
  p[1] = kRet;
  g_tramp_count.store(index + 1, std::memory_order_release);
  g_tramp_by_fn[fn] = p;
  return p;
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
  std::fflush(stderr);
  Env* env = t_env_raw;
  A64::Jit* jit = env != nullptr ? env->CurrentJit() : nullptr;
  if (jit != nullptr) {
    // dynarmic keeps the PC per block, so `pc` is the start of the faulting
    // block (or the instruction itself for an undefined one).
    uint64_t frames[16];
    int n = 0;
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

// Called (Microsoft ABI) by gesher_guest_entry_shim with the guest function
// (from the host_entry stub's r10) and the System V call's arguments.
extern "C" uint64_t gesher_guest_entry_trampoline(const void* fn, EntryFrame* frame) {
  uint64_t r = EnterGuest(fn, frame->gpr, frame->xmm, frame->stack, 8);
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
