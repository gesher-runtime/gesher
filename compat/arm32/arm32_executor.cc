// gesher: checked Dynarmic A32 adapter; never executes guest bytes natively.
#include <gesher/arm32_executor.h>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace gesher::arm32 {
namespace {
// An intentionally unmapped return marker, not a guest-to-host pointer cast.
constexpr uint32_t kReturnMarker = 0xffff0000;
constexpr uint32_t kHostSvc = 0x004732;
constexpr uint32_t kHostInstruction = 0xef000000 | kHostSvc;
constexpr auto kReadWrite = MemoryPermission::kRead | MemoryPermission::kWrite;
constexpr auto kReadExecute = MemoryPermission::kRead | MemoryPermission::kExecute;

bool Fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

struct BusyGuard {
  std::atomic_flag& flag;
  ~BusyGuard() { flag.clear(); }
};
}  // namespace

struct Executor::Impl final : Dynarmic::A32::UserCallbacks {
  struct Binding {
    std::vector<ValueKind> arguments;
    ValueKind result;
    HostCallback callback;
    size_t stack_words;
  };

  explicit Impl(GuestMemory& mem) : memory(mem), monitor(1) {
    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.global_monitor = &monitor;
    config.processor_id = 0;
    config.check_halt_on_memory_access = true;
    config.fastmem_exclusive_access = false;  // alignment checks live in the explicit callbacks
    config.always_little_endian = true;
    config.hook_isb = true;
    config.hook_hint_instructions = true;
    // Keep finite-budget exits and fault handling observable at each dispatch.
    config.optimizations = Dynarmic::all_safe_optimizations &
        ~(Dynarmic::OptimizationFlag::BlockLinking |
          Dynarmic::OptimizationFlag::ReturnStackBuffer |
          Dynarmic::OptimizationFlag::FastDispatch);
    config.code_cache_size = 16 * 1024 * 1024;
    jit = std::make_unique<Dynarmic::A32::Jit>(config);
  }

  void Stop(ExecutionStop stop, const std::string& error) {
    if (!stopped) {
      outcome.stop = stop;
      outcome.error = error;
      stopped = true;
    }
    jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
  }

  template <typename T>
  T Read(uint32_t address) {
    uint8_t bytes[sizeof(T)]{};
    std::string error;
    if (!memory.Read(address, bytes, sizeof(bytes), &error)) {
      Stop(ExecutionStop::MemoryFault, error);
      return 0;
    }
    uint64_t result = 0;
    for (size_t i = 0; i < sizeof(T); ++i) result |= uint64_t(bytes[i]) << (8 * i);
    return static_cast<T>(result);
  }

  template <typename T>
  void Write(uint32_t address, T value) {
    uint8_t bytes[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    std::string error;
    if (!memory.Write(address, bytes, sizeof(bytes), &error))
      Stop(ExecutionStop::MemoryFault, error);
    // Ordinary stores break this CPU's exclusive reservation, including ABA.
    monitor.Clear();
  }

  template <typename T>
  bool Exclusive(uint32_t address, T value, T expected) {
    bool exchanged = false;
    std::string error;
    if (!memory.CompareExchange(address, sizeof(T), expected, value, &exchanged, &error))
      Stop(ExecutionStop::MemoryFault, error);
    return exchanged;
  }

  template <typename T>
  T ReadExclusive(uint32_t address) {
    if (address % sizeof(T)) {
      Stop(ExecutionStop::MemoryFault, "unaligned ARM32 exclusive load");
      return 0;
    }
    return Read<T>(address);
  }

  std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
    uint32_t value;
    // Translation may read ahead. Report execution faults in ExceptionRaised,
    // not here, so an untaken path cannot spuriously fail a successful call.
    if (!memory.Fetch32(address, &value)) return std::nullopt;
    return value;
  }
  uint8_t MemoryRead8(uint32_t a) override { return Read<uint8_t>(a); }
  uint16_t MemoryRead16(uint32_t a) override { return Read<uint16_t>(a); }
  uint32_t MemoryRead32(uint32_t a) override { return Read<uint32_t>(a); }
  uint64_t MemoryRead64(uint32_t a) override { return Read<uint64_t>(a); }
  uint8_t MemoryReadExclusive8(uint32_t a) override { return ReadExclusive<uint8_t>(a); }
  uint16_t MemoryReadExclusive16(uint32_t a) override { return ReadExclusive<uint16_t>(a); }
  uint32_t MemoryReadExclusive32(uint32_t a) override { return ReadExclusive<uint32_t>(a); }
  uint64_t MemoryReadExclusive64(uint32_t a) override { return ReadExclusive<uint64_t>(a); }
  void MemoryWrite8(uint32_t a, uint8_t v) override { Write(a, v); }
  void MemoryWrite16(uint32_t a, uint16_t v) override { Write(a, v); }
  void MemoryWrite32(uint32_t a, uint32_t v) override { Write(a, v); }
  void MemoryWrite64(uint32_t a, uint64_t v) override { Write(a, v); }
  bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t e) override { return Exclusive(a, v, e); }
  bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override { return Exclusive(a, v, e); }
  bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override { return Exclusive(a, v, e); }
  bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override { return Exclusive(a, v, e); }

  void InterpreterFallback(uint32_t, size_t) override {
    Stop(ExecutionStop::UnsupportedInstruction, "A32 instruction requires an unavailable interpreter");
  }
  void CallSVC(uint32_t number) override {
    const uint32_t next = jit->Regs()[15];
    if (number != kHostSvc || (jit->Cpsr() & 0x20) || next < 4 ||
        bindings.find(next - 4) == bindings.end()) {
      Stop(ExecutionStop::UnsupportedSupervisorCall, "ARM32 system call has no adapter");
      return;
    }
    pending_binding = next - 4;
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
  }
  void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
    using E = Dynarmic::A32::Exception;
    if (exception == E::NoExecuteFault && pc == kReturnMarker) {
      Stop(ExecutionStop::Returned, "");
    } else if (exception == E::NoExecuteFault) {
      Stop(ExecutionStop::MemoryFault, "ARM32 instruction fetch outside executable guest memory");
    } else if (exception == E::PreloadData || exception == E::PreloadDataWithIntentToWrite ||
               exception == E::PreloadInstruction || exception == E::Yield) {
      // Architectural performance hints have no required architectural effect.
    } else {
      Stop(ExecutionStop::UnsupportedInstruction, "ARM32 exception or unsupported system instruction");
    }
  }
  void InstructionSynchronizationBarrierRaised() override { jit->ClearCache(); }
  void AddTicks(uint64_t ticks) override { remaining -= std::min(remaining, ticks); }
  uint64_t GetTicksRemaining() override { return remaining; }

  bool InvokeHost() {
    const Binding& binding = bindings.at(*pending_binding);
    pending_binding.reset();
    CallFrame frame;
    std::copy_n(jit->Regs().begin(), 4, frame.core.begin());
    frame.stack.resize(binding.stack_words);
    const uint32_t sp = jit->Regs()[13];
    if (sp & 7) {
      Stop(ExecutionStop::HostCallFailed, "unaligned ARM32 host-call stack");
      return false;
    }
    std::string error;
    // Read words separately only after validating the complete stack range.
    if (!memory.IsMapped(sp, frame.stack.size() * 4, MemoryPermission::kRead)) {
      Stop(ExecutionStop::MemoryFault, "ARM32 host-call arguments outside readable stack");
      return false;
    }
    for (size_t i = 0; i < frame.stack.size(); ++i) {
      if (!memory.Read32(static_cast<uint32_t>(uint64_t(sp) + i * 4), &frame.stack[i], &error)) {
        Stop(ExecutionStop::MemoryFault, error);
        return false;
      }
    }
    std::vector<Value> arguments;
    Value returned;
    std::array<uint32_t, 2> registers;
    if (!UnpackArguments(binding.arguments, frame, &arguments, &error) ||
        !binding.callback(arguments, &returned, &error) ||
        returned.kind != binding.result || !EncodeReturn(returned, &registers, &error)) {
      Stop(ExecutionStop::HostCallFailed, error.empty() ? "ARM32 host callback returned the wrong type" : error);
      return false;
    }
    jit->Regs()[0] = registers[0];
    jit->Regs()[1] = registers[1];
    // Resume after SVC. BX LR in the thunk performs genuine ARM/Thumb interworking.
    // Adapters may have changed guest code or permissions while execution stopped.
    jit->ClearCache();
    // ClearCache queues work behind its halt flag. Clearing that flag here
    // would cancel invalidation and let old code bypass revoked permissions.
    jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
    monitor.Clear();
    return true;
  }

  GuestMemory& memory;
  Dynarmic::ExclusiveMonitor monitor;
  std::unique_ptr<Dynarmic::A32::Jit> jit;
  std::map<uint32_t, Binding> bindings;
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  std::optional<uint32_t> pending_binding;
  ExecutionResult outcome;
  bool stopped = false;
  uint64_t remaining = 0;
};

Executor::Executor(GuestMemory& memory) : impl_(std::make_unique<Impl>(memory)) {}
Executor::~Executor() = default;

bool Executor::BindHostFunction(GuestAddress page, std::vector<ValueKind> arguments,
                                ValueKind result, HostCallback callback, std::string* error) {
  auto& state = *impl_;
  if (state.busy.test_and_set()) return Fail(error, "ARM32 executor is already active");
  BusyGuard guard{state.busy};
  if (!callback || page == kReturnMarker || page % GuestMemory::kPageSize != 0 ||
      state.bindings.find(page) != state.bindings.end())
    return Fail(error, "invalid ARM32 host thunk address or callback");
  std::vector<Value> zeros;
  for (ValueKind kind : arguments) zeros.push_back({kind, 0});
  CallFrame frame;
  std::array<uint32_t, 2> ignored;
  if (!PackArguments(zeros, &frame, error) || !EncodeReturn({result, 0}, &ignored, error)) return false;
  if (!state.memory.Map(page, GuestMemory::kPageSize, kReadWrite, error)) return false;
  try {
    if (!state.memory.Write32(page, kHostInstruction, error) ||
        !state.memory.Write32(page + 4, 0xe12fff1e, error) ||
        !state.memory.Protect(page, GuestMemory::kPageSize, kReadExecute, error)) {
      state.memory.Unmap(page, GuestMemory::kPageSize);
      return false;
    }
    state.bindings.emplace(page, Impl::Binding{std::move(arguments), result,
                                              std::move(callback), frame.stack.size()});
  } catch (...) {
    state.memory.Unmap(page, GuestMemory::kPageSize);
    return Fail(error, "cannot allocate ARM32 host binding");
  }
  if (error) error->clear();
  return true;
}

ExecutionResult Executor::Call(GuestAddress function, const CallFrame& frame,
                               GuestAddress stack_top, uint64_t instruction_budget) {
  auto& state = *impl_;
  if (state.busy.test_and_set()) return {ExecutionStop::InvalidCall, {}, 0, "ARM32 executor is already active"};
  BusyGuard guard{state.busy};
  state.outcome = {};
  state.stopped = false;
  state.pending_binding.reset();
  state.remaining = instruction_budget;
  uint32_t sp;
  std::string error;
  const uint32_t pc = function & ~uint32_t{1};
  if (instruction_budget == 0 || instruction_budget > uint64_t(std::numeric_limits<int64_t>::max()) ||
      (!(function & 1) && (function & 3)) ||
      !StackPointerFor(frame, stack_top, &sp, &error) || sp < GuestMemory::kPageSize + 8 ||
      !state.memory.IsMapped(sp - 8, 8, kReadWrite) ||
      !state.memory.IsMapped(sp, frame.stack.size() * 4, kReadWrite) ||
      state.memory.IsMapped(kReturnMarker, GuestMemory::kPageSize) ||
      !state.memory.IsMapped(pc, (function & 1) ? 2 : 4, MemoryPermission::kExecute)) {
    state.outcome.error = error.empty() ? "invalid ARM32 entry, stack, reserved marker or instruction budget" : error;
    return state.outcome;
  }
  try {
    state.jit->Reset();
    state.jit->ClearCache();
    state.monitor.Clear();
    for (size_t i = 0; i < frame.stack.size(); ++i) {
      if (!state.memory.Write32(static_cast<uint32_t>(uint64_t(sp) + i * 4), frame.stack[i], &error))
        return {ExecutionStop::MemoryFault, {}, pc, error};
    }
    std::copy(frame.core.begin(), frame.core.end(), state.jit->Regs().begin());
    state.jit->Regs()[13] = sp;
    state.jit->Regs()[14] = kReturnMarker;
    state.jit->Regs()[15] = pc;
    state.jit->SetCpsr(0x10 | ((function & 1) ? 0x20 : 0));
    while (!state.stopped) {
      auto reason = state.jit->Run();
      if (state.stopped) break;
      if (state.pending_binding) {
        if (!state.InvokeHost()) break;
      } else if (!Dynarmic::Has(reason, Dynarmic::HaltReason::CacheInvalidation) && state.remaining != 0) {
        state.Stop(ExecutionStop::UnsupportedInstruction, "unexpected ARM32 execution stop");
      }
      if (state.remaining == 0 && !state.stopped)
        state.Stop(ExecutionStop::BudgetExceeded, "ARM32 instruction budget exhausted");
    }
  } catch (const std::exception& exception) {
    state.Stop(ExecutionStop::HostCallFailed, exception.what());
  } catch (...) {
    state.Stop(ExecutionStop::HostCallFailed, "ARM32 execution adapter failed");
  }
  state.outcome.pc = state.jit->Regs()[15];
  state.outcome.registers = {state.jit->Regs()[0], state.jit->Regs()[1]};
  return state.outcome;
}

}  // namespace gesher::arm32
