// gesher: linked ELF32 ARM program execution in an isolated guest address space.
// This is a standalone native-library runtime, NOT an ART JNI32 or Android APK bridge.
#pragma once

#include <gesher/arm32_executor.h>
#include <gesher/arm32_elf.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::arm32 {

// Supplies exact guest ELF32 bytes for the requested root or DT_NEEDED soname.
// The provider owns filesystem/namespace policy: this runtime never opens a
// path or trusts a dependency name as a filesystem path.
using LibraryProvider =
    std::function<bool(std::string_view name, std::vector<uint8_t>* bytes,
                       std::string* error)>;

class Program {
 public:
  explicit Program(LibraryProvider provider,
                   size_t guest_memory_budget = GuestMemory::kDefaultBudget);
  ~Program();
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;

  // Typed host entrypoints are explicit, never raw host addresses. A function
  // is assigned one executable guest thunk page and resolved by its symbol.
  // A binding is permanent for the lifetime of this Program.
  bool BindHostSymbol(std::string name, std::vector<ValueKind> arguments,
                      ValueKind result, HostCallback callback,
                      std::string* error = nullptr);

  // Recursively loads a dependency DAG using the provider. Cycles, unresolved
  // imports, unsupported ELF features, overlapping images, and budget failures
  // are explicit errors. All newly mapped modules are rolled back on failure;
  // modules and typed host thunks from previous successful calls remain.
  // No guest constructors run during loading.
  bool LoadLibrary(std::string_view name, std::string* error = nullptr);

  // Run constructors in completed dependency-first order, at most once each.
  // A guest fault leaves the Program in a failed initialization state. No
  // attempt is made to undo arbitrary side effects performed by constructors.
  bool Initialize(std::string* error = nullptr);

  bool CallExport(std::string_view library, std::string_view symbol,
                  const std::vector<Value>& arguments, ValueKind result_kind,
                  Value* result, std::string* error = nullptr,
                  uint64_t instruction_budget = 1000000);

  GuestMemory& Memory() { return memory_; }
  const GuestMemory& Memory() const { return memory_; }
  size_t LibraryCount() const { return modules_.size(); }

 private:
  struct Module {
    std::string requested_name;
    ElfImage image;
    bool initialized = false;
  };

  bool LoadRecursive(const std::string& name, std::string* error);
  bool ReserveSpan(const std::vector<uint8_t>& bytes, uint32_t* bias,
                   uint32_t* base, uint64_t* size, std::string* error);
  bool Resolve(std::string_view name, uint32_t* result) const;
  void Rollback(size_t count, uint64_t next_address) noexcept;
  const Module* Find(std::string_view name) const;

  LibraryProvider provider_;
  GuestMemory memory_;
  Executor executor_;
  std::vector<Module> modules_;
  std::set<std::string> loading_;
  std::map<std::string, uint32_t, std::less<>> host_symbols_;
  uint64_t next_image_ = 0x10000000;
  uint64_t next_thunk_ = 0xe0000000;
  bool stack_ok_ = false;
  bool initialization_failed_ = false;
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;

  static constexpr uint32_t kStackBase = 0xeff00000;
  static constexpr uint32_t kStackSize = 0x100000;
  static constexpr uint32_t kStackTop = kStackBase + kStackSize;
  static constexpr uint64_t kMaxImageEnd = 0xd0000000;
};

}  // namespace gesher::arm32
