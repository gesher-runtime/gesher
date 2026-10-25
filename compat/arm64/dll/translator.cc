// gesher arm64 translator: gesher_arm64.dll. Runs guest AArch64 code with
// dynarmic (A64 frontend, x86-64 backend) in the host process, the guest
// sharing the host address space, and marshals AAPCS64 <-> the host ABI in
// both directions. docs/arm64.md, docs/decisions/0047-arm64-translation.md.
//
// gesher_arm64_init() exchanges the two interface tables
// (gesher/arm64_translator.h). Then the loader (compat/arm64, compat/elf):
//  - add_code() registers each mapped guest executable segment; branches into
//    a registered region run as guest code, branches elsewhere are host calls.
//  - host_entry(fn) gives a System V x86-64 stub that enters the guest, for a
//    guest function host code calls (init/fini arrays, IFUNC resolvers, the
//    JNI natives and callbacks).
//  - each guest import is bound to a guest trampoline `SVC #0; RET`; the
//    guest's call lands there, CallSVC turns it into a System V call of the
//    host function (gesher_call_host_sysv marshals AAPCS64 -> System V).

#include <gesher/arm64_translator.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
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
  uint64_t nstack;       // count of `stack` (even, for 16-byte alignment)
  uint64_t out_xmm0;     // written by the thunk: the result's xmm0 low 64 bits
};

// thunks.S (Microsoft x64 entry):
extern "C" uint64_t gesher_call_host_sysv(HostCall* call);
extern "C" void gesher_guest_entry_shim();  // host_entry stubs jump here

namespace {

const gesher_arm64_host* g_host = nullptr;

// --- guest code regions -------------------------------------------------------
struct CodeRegion {
  uintptr_t start;
  uintptr_t end;
};
std::mutex g_regions_mutex;
std::vector<CodeRegion> g_regions;

bool InGuestCode(uintptr_t addr) {
  std::lock_guard<std::mutex> lock(g_regions_mutex);
  for (const CodeRegion& r : g_regions) {
    if (addr >= r.start && addr < r.end) return true;
  }
  return false;
}

void AddRegion(uintptr_t start, size_t size) {
  std::lock_guard<std::mutex> lock(g_regions_mutex);
  g_regions.push_back({start, start + size});
}

// --- guest trampolines for host imports --------------------------------------
constexpr uint32_t kSvc0 = 0xD4000001;  // SVC #0
constexpr uint32_t kRet = 0xD65F03C0;   // RET

struct Trampoline {
  void* host_fn;
  int va_list_index;
};
std::mutex g_tramp_mutex;
uint8_t* g_tramp_page = nullptr;
size_t g_tramp_used = 0;
size_t g_tramp_cap = 0;
std::vector<std::pair<uintptr_t, Trampoline>> g_tramps;

const Trampoline* FindTrampoline(uintptr_t addr) {
  std::lock_guard<std::mutex> lock(g_tramp_mutex);
  for (auto& [a, t] : g_tramps) {
    if (a == addr) return &t;
  }
  return nullptr;
}

int VaListIndexOf(void* fn);

void* MakeTrampoline(void* fn) {
  std::lock_guard<std::mutex> lock(g_tramp_mutex);
  if (g_tramp_page == nullptr || g_tramp_used + 8 > g_tramp_cap) {
    g_tramp_cap = 0x10000;
    g_tramp_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, g_tramp_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    g_tramp_used = 0;
    if (g_tramp_page == nullptr) return nullptr;
    AddRegion(reinterpret_cast<uintptr_t>(g_tramp_page), g_tramp_cap);
  }
  uint8_t* p = g_tramp_page + g_tramp_used;
  g_tramp_used += 8;
  std::memcpy(p, &kSvc0, 4);
  std::memcpy(p + 4, &kRet, 4);
  FlushInstructionCache(GetCurrentProcess(), p, 8);
  g_tramps.push_back({reinterpret_cast<uintptr_t>(p), {fn, VaListIndexOf(fn)}});
  return p;
}

// --- va_list host functions --------------------------------------------------
std::mutex g_va_mutex;
std::vector<std::pair<void*, int>> g_va_functions;

int VaListIndexOf(void* fn) {
  std::lock_guard<std::mutex> lock(g_va_mutex);
  for (auto& [f, i] : g_va_functions) {
    if (f == fn) return i;
  }
  return -1;
}

// --- guest TLS (variant I: TPIDR_EL0 -> thread pointer; TCB then modules) -----
// A simple static-TLS allocator: modules get offsets above the 16-byte TCB in
// each thread's block. Enough for local-exec / initial-exec and static TLSDESC.
constexpr size_t kTcbSize = 16;
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
std::vector<uint64_t*> g_tls_blocks;  // every live thread's block base (as TP)

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
  for (uint64_t* tp : g_tls_blocks) {
    auto* base = reinterpret_cast<uint8_t*>(tp);
    std::memset(base + offset, 0, total_size);
    if (image != nullptr && image_size > 0) std::memcpy(base + offset, image, image_size);
  }
}

// --- the per-thread Jit ------------------------------------------------------
Dynarmic::ExclusiveMonitor& Monitor() {
  static Dynarmic::ExclusiveMonitor m{64};
  return m;
}

// The return stub: a guest page the host sets as X30 before entering the guest.
// When the guest returns to it, its `SVC #0` is recognised by its address and
// halts the Jit, so Run() returns and the host call unwinds. Created once.
std::mutex g_return_mutex;
uintptr_t g_return_stub = 0;

uintptr_t ReturnStub() {
  std::lock_guard<std::mutex> lock(g_return_mutex);
  if (g_return_stub == 0) {
    auto* p = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint32_t svc = kSvc0;
    std::memcpy(p, &svc, 4);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
    AddRegion(reinterpret_cast<uintptr_t>(p), 0x1000);
    g_return_stub = reinterpret_cast<uintptr_t>(p);
  }
  return g_return_stub;
}

class Env final : public A64::UserCallbacks {
 public:
  Env() {
    // This thread's guest TLS block, and its initial-exec images.
    tls_block_.assign(kTlsBlockSize / 8, 0);
    tls_pointer_ = reinterpret_cast<uint64_t>(tls_block_.data());
    *reinterpret_cast<uint64_t*>(tls_pointer_) = tls_pointer_;  // TCB self
    {
      std::lock_guard<std::mutex> lock(g_tls_mutex);
      g_tls_blocks.push_back(reinterpret_cast<uint64_t*>(tls_pointer_));
      for (const TlsImage& ti : g_tls_images) {
        auto* base = reinterpret_cast<uint8_t*>(tls_pointer_);
        std::memset(base + ti.offset, 0, ti.total_size);
        if (ti.image) std::memcpy(base + ti.offset, ti.image, ti.image_size);
      }
    }
    // A private guest stack for this thread (8 MiB), descending; nested
    // host->guest calls take successive sub-ranges.
    guest_stack_.assign(8 << 20, 0);
    stack_top_ = reinterpret_cast<uint64_t>(guest_stack_.data() + guest_stack_.size()) & ~uint64_t{15};
    stack_cur_ = stack_top_;

    A64::UserConfig conf;
    conf.callbacks = this;
    conf.tpidr_el0 = &tls_pointer_;
    conf.tpidrro_el0 = &tls_pointer_;
    conf.processor_id = next_id_.fetch_add(1) & 63;
    conf.global_monitor = &Monitor();
    conf.enable_cycle_counting = false;
    conf.wall_clock_cntpct = true;
    conf.code_cache_size = 32 * 1024 * 1024;
    jit_ = std::make_unique<A64::Jit>(conf);
  }

  A64::Jit* jit() { return jit_.get(); }

  std::uint8_t MemoryRead8(uint64_t a) override { return *reinterpret_cast<uint8_t*>(a); }
  std::uint16_t MemoryRead16(uint64_t a) override { uint16_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 2); return v; }
  std::uint32_t MemoryRead32(uint64_t a) override { uint32_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 4); return v; }
  std::uint64_t MemoryRead64(uint64_t a) override { uint64_t v; std::memcpy(&v, reinterpret_cast<void*>(a), 8); return v; }
  A64::Vector MemoryRead128(uint64_t a) override { A64::Vector v; std::memcpy(&v, reinterpret_cast<void*>(a), 16); return v; }
  void MemoryWrite8(uint64_t a, std::uint8_t v) override { *reinterpret_cast<uint8_t*>(a) = v; }
  void MemoryWrite16(uint64_t a, std::uint16_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 2); }
  void MemoryWrite32(uint64_t a, std::uint32_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
  void MemoryWrite64(uint64_t a, std::uint64_t v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
  void MemoryWrite128(uint64_t a, A64::Vector v) override { std::memcpy(reinterpret_cast<void*>(a), &v, 16); }

  void InterpreterFallback(uint64_t pc, size_t) override {
    std::fprintf(stderr, "gesher arm64: interpreter fallback at guest pc 0x%llx\n",
                 static_cast<unsigned long long>(pc));
    jit_->HaltExecution();
  }

  void CallSVC(std::uint32_t) override {
    uint64_t pc = jit_->GetPC();  // points after the SVC
    if (pc - 4 == ReturnStub()) {  // the guest returned to the host
      returned_ = true;
      jit_->HaltExecution();
      return;
    }
    const Trampoline* t = FindTrampoline(pc - 4);
    if (t == nullptr) {
      std::fprintf(stderr, "gesher arm64: SVC at guest pc 0x%llx is not a host trampoline\n",
                   static_cast<unsigned long long>(pc - 4));
      jit_->HaltExecution();
      return;
    }
    CallHost(t);
  }

  void ExceptionRaised(uint64_t pc, A64::Exception ex) override {
    std::fprintf(stderr, "gesher arm64: guest exception %d at pc 0x%llx\n", static_cast<int>(ex),
                 static_cast<unsigned long long>(pc));
    jit_->HaltExecution();
  }

  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return static_cast<uint64_t>(-1) / 2; }
  std::uint64_t GetCNTPCT() override { return 0; }

  uint64_t Enter(const void* fn, const uint64_t gpr[8], const uint64_t xmm[8]) {
    // Save the whole register file: a host->guest call may happen while a
    // guest->host->guest chain is unwinding on this thread.
    std::array<uint64_t, 31> saved_regs = jit_->GetRegisters();
    uint64_t saved_sp = jit_->GetSP();
    uint64_t saved_pc = jit_->GetPC();
    uint64_t saved_stack_cur = stack_cur_;

    uint64_t sp = (stack_cur_ - 4096) & ~uint64_t{15};  // leave a red zone
    stack_cur_ = sp - (1 << 20);                        // reserve 1 MiB for this call
    for (int i = 0; i < 8; ++i) {
      jit_->SetRegister(i, gpr[i]);
      jit_->SetVector(i, A64::Vector{xmm[i], 0});
    }
    jit_->SetRegister(30, ReturnStub());
    jit_->SetSP(sp);
    jit_->SetPC(reinterpret_cast<uint64_t>(fn));

    uint64_t result = RunUntilReturn();
    uint64_t rfp = jit_->GetVector(0)[0];

    stack_cur_ = saved_stack_cur;
    jit_->SetRegisters(saved_regs);
    jit_->SetSP(saved_sp);
    jit_->SetPC(saved_pc);
    last_fp_result_ = rfp;
    return result;
  }

  uint64_t last_fp_result() const { return last_fp_result_; }

 private:
  uint64_t RunUntilReturn() {
    returned_ = false;
    for (int guard = 0; guard < 1000000; ++guard) {
      jit_->ClearHalt(Dynarmic::HaltReason::UserDefined1);
      jit_->Run();
      if (returned_) return jit_->GetRegister(0);
      uint64_t pc = jit_->GetPC();
      if (!InGuestCode(pc)) {
        std::fprintf(stderr, "gesher arm64: guest left to host pc 0x%llx (not a trampoline)\n",
                     static_cast<unsigned long long>(pc));
        return jit_->GetRegister(0);
      }
    }
    std::fprintf(stderr, "gesher arm64: guest did not return\n");
    return jit_->GetRegister(0);
  }

  void CallHost(const Trampoline* t) {
    // AAPCS64 -> System V. X0..X5 are the first six System V integer arguments;
    // X6, X7 are System V arguments 7 and 8, which go on the stack; any further
    // arguments (a variadic call with more than eight) are on the guest stack at
    // SP and follow on the host stack. Copying a generous, fixed window of guest
    // stack words covers them without knowing the real count (the callee reads
    // only what its signature or format string names).
    uint64_t gpr[6], xmm[8];
    for (int i = 0; i < 6; ++i) gpr[i] = jit_->GetRegister(i);
    for (int i = 0; i < 8; ++i) xmm[i] = jit_->GetVector(i)[0];
    uint64_t stack[2 + 14];
    stack[0] = jit_->GetRegister(6);
    stack[1] = jit_->GetRegister(7);
    uint64_t gsp = jit_->GetSP();
    for (int i = 0; i < 14; ++i) {
      std::memcpy(&stack[2 + i], reinterpret_cast<void*>(gsp + i * 8), 8);
    }
    HostCall call{t->host_fn, gpr, xmm, stack, 16, 0};
    uint64_t rax = gesher_call_host_sysv(&call);
    jit_->SetRegister(0, rax);
    jit_->SetVector(0, A64::Vector{call.out_xmm0, 0});
  }

  std::unique_ptr<A64::Jit> jit_;
  std::vector<uint64_t> tls_block_;
  uint64_t tls_pointer_;
  std::vector<uint8_t> guest_stack_;
  uint64_t stack_top_;
  uint64_t stack_cur_;
  uint64_t last_fp_result_ = 0;
  bool returned_ = false;
  static std::atomic<size_t> next_id_;
};

std::atomic<size_t> Env::next_id_{0};
thread_local std::unique_ptr<Env> t_env;

Env* ThreadEnv() {
  if (!t_env) t_env = std::make_unique<Env>();
  return t_env.get();
}

uint64_t EnterGuest(const void* fn, const uint64_t gpr[8], const uint64_t xmm[8]) {
  return ThreadEnv()->Enter(fn, gpr, xmm);
}

}  // namespace

// Called (Microsoft ABI) by gesher_guest_entry_shim with the guest function in
// r10 (arg1) and the spilled System V argument registers.
extern "C" uint64_t gesher_guest_entry_trampoline(const void* fn, const uint64_t* gpr,
                                                   const uint64_t* xmm, uint64_t* out_xmm0) {
  uint64_t r = EnterGuest(fn, gpr, xmm);
  if (out_xmm0) *out_xmm0 = t_env->last_fp_result();
  return r;
}

namespace {

// --- host_entry stubs --------------------------------------------------------
std::mutex g_entry_mutex;
std::vector<std::pair<const void*, void*>> g_entries;
uint8_t* g_entry_page = nullptr;
size_t g_entry_used = 0;
size_t g_entry_cap = 0;

void* MakeHostEntry(const void* fn) {
  std::lock_guard<std::mutex> lock(g_entry_mutex);
  for (auto& [g, s] : g_entries) {
    if (g == fn) return s;
  }
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
  g_entries.push_back({fn, p});
  return p;
}

// --- the API table -----------------------------------------------------------
void ApiAddCode(const void* start, size_t size, const char* /*name*/) {
  AddRegion(reinterpret_cast<uintptr_t>(start), size);
}

uint64_t ApiCall(const void* fn, const uint64_t* args, size_t nargs) {
  uint64_t gpr[8] = {0}, xmm[8] = {0};
  for (size_t i = 0; i < nargs && i < 8; ++i) gpr[i] = args[i];
  return EnterGuest(fn, gpr, xmm);
}

void* ApiHostEntry(const void* fn) { return MakeHostEntry(fn); }

void* ApiGuestSymbol(const char* /*name*/) { return nullptr; }

void ApiAddVaListFunction(const void* fn, int index) {
  std::lock_guard<std::mutex> lock(g_va_mutex);
  g_va_functions.push_back({const_cast<void*>(fn), index});
}

void* ApiTlsDescStatic(void) { return nullptr; }

int ApiIsGuestCode(const void* addr) {
  return InGuestCode(reinterpret_cast<uintptr_t>(addr)) ? 1 : 0;
}

// Binds a guest import to its host trampoline: exposed through the loader's own
// relocation path, not the API struct (the loader calls gesher_arm64_bind).
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
};

}  // namespace

// A guest trampoline (guest code) that calls host function `fn`; the loader
// binds a guest import's GOT entry to this address.
extern "C" __declspec(dllexport) void* gesher_arm64_bind(void* fn) { return MakeTrampoline(fn); }

extern "C" __declspec(dllexport) const gesher_arm64_api* gesher_arm64_init(
    const gesher_arm64_host* host) {
  if (host == nullptr || host->version != GESHER_ARM64_API_VERSION) return nullptr;
  g_host = host;
  return &g_api;
}
