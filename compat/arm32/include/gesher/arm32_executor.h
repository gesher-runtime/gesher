// gesher: bounded ARM/Thumb execution in a separate 32-bit address domain.
// Experimental component only; this is not the Android JNI/Bionic runtime.
#pragma once

#include <gesher/arm32_abi.h>
#include <gesher/arm32_memory.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gesher::arm32 {

struct ElfImage;

// Maps a previously validated ELF32 image transactionally, including per-page
// segment protections and inaccessible holes. Does not run init/fini routines.
bool MapElfImage(const ElfImage& image, GuestMemory* memory, std::string* error = nullptr);

enum class ExecutionStop {
  Returned,
  InvalidCall,
  MemoryFault,
  UnsupportedInstruction,
  UnsupportedSupervisorCall,
  HostCallFailed,
  BudgetExceeded,
};

struct ExecutionResult {
  ExecutionStop stop = ExecutionStop::InvalidCall;
  std::array<uint32_t, 2> registers{};
  uint32_t pc = 0;
  std::string error;
  bool returned() const { return stop == ExecutionStop::Returned; }
};

// No automatic conversion of Pointer32 to a host pointer is performed. A host
// adapter explicitly uses GuestMemory to read/write guest buffers. A callback
// must return the declared scalar kind and report failures through its result.
using HostCallback = std::function<bool(const std::vector<Value>&, Value*, std::string*)>;

class Executor {
 public:
  // The memory owner outlives this object. One call may execute at a time;
  // concurrent/reentrant calls fail explicitly. Shared guest threads, TLS,
  // signals and native JNI entry points are outside this component's contract.
  explicit Executor(GuestMemory& memory);
  ~Executor();
  Executor(const Executor&) = delete;
  Executor& operator=(const Executor&) = delete;

  // Creates a checked ARM-state SVC thunk in one new guest page. It can be used
  // by an ELF32 import resolver. The page belongs to GuestMemory for its lifetime;
  // no host function address is stored in guest memory. Bind only between Calls.
  bool BindHostFunction(GuestAddress page, std::vector<ValueKind> arguments,
                        ValueKind result, HostCallback callback,
                        std::string* error = nullptr);

  // ARM function pointers are word-aligned; Thumb pointers have bit 0 set.
  // stack_top must be 8-byte aligned and point just above writable guest stack.
  // An instruction budget in [1, INT64_MAX] bounds loops (a translated block can consume
  // the last ticks). Every call starts with reset CPU state and a fresh code
  // cache so writes/protection changes between calls cannot reuse stale code.
  ExecutionResult Call(GuestAddress function, const CallFrame& frame,
                       GuestAddress stack_top, uint64_t instruction_budget = 1000000);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace gesher::arm32
