// gesher: isolated multi-DSO ELF32 ARM loading and executable guest entrypoints.
// This runtime does not claim Android APK, JNI32, Bionic32 or native-thread support.
#include <gesher/arm32_program.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace gesher::arm32 {
namespace {
constexpr auto kReadWrite = MemoryPermission::kRead | MemoryPermission::kWrite;
constexpr uint64_t kPage = GuestMemory::kPageSize;
constexpr size_t kMaxLibraries = 64;
constexpr uint64_t kMaxFileBytes = 256ull * 1024 * 1024;

uint16_t Read16(const uint8_t* p) {
  return static_cast<uint16_t>(uint16_t{p[0]} | (uint16_t{p[1]} << 8));
}
uint32_t Read32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
         (uint32_t{p[3]} << 24);
}
uint64_t RoundUp(uint64_t n) { return (n + kPage - 1) & ~(kPage - 1); }
bool Fail(std::string* error, const char* text) {
  if (error) *error = text;
  return false;
}

struct ActiveName {
  std::set<std::string>* set;
  std::set<std::string>::iterator iterator;
  ~ActiveName() { set->erase(iterator); }
};
}  // namespace

Program::Program(LibraryProvider provider, size_t budget)
    : provider_(std::move(provider)), memory_(budget), executor_(memory_) {
  stack_ok_ = memory_.Map(kStackBase, kStackSize, kReadWrite);
}

Program::~Program() { Rollback(0, 0x10000000); }

const Program::Module* Program::Find(std::string_view name) const {
  for (const auto& module : modules_) {
    if (module.requested_name == name || (!module.image.soname.empty() &&
                                          module.image.soname == name)) return &module;
  }
  return nullptr;
}

bool Program::Resolve(std::string_view name, uint32_t* result) const {
  if (!result) return false;
  // We only expose already-relocated dependency symbols and explicit typed
  // host adapters. There is no arbitrary cast of a host pointer to Guest32.
  for (const auto& module : modules_) {
    if (module.image.Lookup(name, result)) return true;
  }
  const auto host = host_symbols_.find(name);
  if (host == host_symbols_.end()) return false;
  *result = host->second;
  return true;
}

bool Program::BindHostSymbol(std::string name, std::vector<ValueKind> arguments,
                             ValueKind result, HostCallback callback,
                             std::string* error) {
  if (!stack_ok_ || name.empty() || !callback || host_symbols_.contains(name) ||
      next_thunk_ >= kStackBase)
    return Fail(error, "invalid or duplicate ARM32 host symbol binding");
  const uint32_t address = static_cast<uint32_t>(next_thunk_);
  try {
    // Reserve the name first. A failed bind rolls it back; a successful bind
    // remains permanent, just like the executable page and callback.
    const auto [it, inserted] = host_symbols_.emplace(std::move(name), address);
    if (!inserted) return Fail(error, "duplicate ARM32 host symbol");
    if (!executor_.BindHostFunction(address, std::move(arguments), result,
                                     std::move(callback), error)) {
      host_symbols_.erase(it);
      return false;
    }
    next_thunk_ += kPage;
  } catch (...) {
    return Fail(error, "cannot allocate ARM32 host symbol");
  }
  if (error) error->clear();
  return true;
}

bool Program::ReserveSpan(const std::vector<uint8_t>& bytes, uint32_t* bias,
                          uint32_t* base, uint64_t* size, std::string* error) {
  // This cheap structural preflight only reserves addresses. The full ELF32
  // parser later verifies every table, section and relocation before mapping.
  if (bytes.size() < 52 || bytes.size() > kMaxFileBytes ||
      bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
      bytes[3] != 'F' || bytes[4] != 1 ||
      Read16(bytes.data() + 42) != 32)
    return Fail(error, "invalid ARM32 library header for address reservation");
  const uint64_t phoff = Read32(bytes.data() + 28);
  const uint32_t phnum = Read16(bytes.data() + 44);
  if (phnum == 0 || phoff > bytes.size() ||
      uint64_t{phnum} * 32 > bytes.size() - phoff)
    return Fail(error, "invalid ARM32 program header range");

  uint64_t minimum = UINT64_MAX, maximum = 0;
  for (uint32_t i = 0; i < phnum; ++i) {
    const auto* ph = bytes.data() + phoff + uint64_t{i} * 32;
    if (Read32(ph) != 1) continue;  // PT_LOAD
    const uint64_t vaddr = Read32(ph + 8), length = Read32(ph + 20);
    if (length == 0 || vaddr + length > UINT32_MAX)
      return Fail(error, "invalid ARM32 load segment extent");
    minimum = std::min(minimum, vaddr & ~(kPage - 1));
    maximum = std::max(maximum, RoundUp(vaddr + length));
  }
  if (minimum == UINT64_MAX || maximum <= minimum ||
      maximum - minimum > kMaxFileBytes)
    return Fail(error, "invalid ARM32 load segment span");

  uint64_t requested = std::max(RoundUp(next_image_), minimum);
  const uint64_t span = maximum - minimum;
  if (requested >= kMaxImageEnd || span > kMaxImageEnd - requested ||
      requested - minimum > UINT32_MAX)
    return Fail(error, "ARM32 guest image address space exhausted");
  *bias = static_cast<uint32_t>(requested - minimum);
  *base = static_cast<uint32_t>(requested);
  *size = span;
  next_image_ = requested + span + kPage;  // Guard gap between ELF objects.
  return true;
}

bool Program::LoadRecursive(const std::string& name, std::string* error) {
  if (Find(name)) return true;
  if (modules_.size() + loading_.size() >= kMaxLibraries)
    return Fail(error, "ARM32 dependency count limit exceeded");
  const auto [it, inserted] = loading_.insert(name);
  if (!inserted) return Fail(error, "cyclic ARM32 DT_NEEDED dependency");
  ActiveName active{&loading_, it};

  if (!provider_) return Fail(error, "no ARM32 library provider configured");
  std::vector<uint8_t> bytes;
  if (!provider_(name, &bytes, error))
    return Fail(error, "ARM32 dependency bytes unavailable");

  uint32_t bias = 0, expected_base = 0;
  uint64_t span = 0;
  if (!ReserveSpan(bytes, &bias, &expected_base, &span, error)) return false;

  ElfImage image;
  ElfLoadOptions options;
  options.load_bias = bias;
  options.resolve_import = [this](std::string_view symbol, uint32_t* address) {
    return Resolve(symbol, address);
  };
  std::string child_error;
  options.resolve_dependency = [this, &child_error](std::string_view dependency) {
    return LoadRecursive(std::string(dependency), &child_error);
  };
  if (!LoadElf32(bytes.data(), bytes.size(), options, &image, error)) {
    if (error && !child_error.empty()) *error += " (" + child_error + ")";
    return false;
  }
  if (image.guest_base != expected_base || image.bytes.size() > span ||
      !MapElfImage(image, &memory_, error))
    return Fail(error, "ARM32 mapped DSO does not match reserved address span");

  // Persist modules only after complete ELF validation and mapping. Any
  // failure after a child was mapped is rolled back by the root transaction.
  const uint64_t mapped_length = RoundUp(image.bytes.size());
  try {
    modules_.push_back({name, std::move(image), false});
  } catch (...) {
    // A move may have emptied image.bytes before the allocation failed;
    // retain the complete mapping length separately for reliable rollback.
    memory_.Unmap(expected_base, mapped_length);
    return Fail(error, "cannot retain ARM32 loaded library");
  }
  return true;
}

void Program::Rollback(size_t count, uint64_t next_address) noexcept {
  while (modules_.size() > count) {
    const auto& image = modules_.back().image;
    try {
      memory_.Unmap(image.guest_base, RoundUp(image.bytes.size()));
    } catch (...) {
      // GuestMemory's checked Unmap is nonthrowing for mapped ranges. A bad
      // allocation while reporting an error must not escape the destructor.
    }
    modules_.pop_back();
  }
  next_image_ = next_address;
}

bool Program::LoadLibrary(std::string_view name, std::string* error) {
  if (!stack_ok_ || name.empty())
    return Fail(error, "ARM32 program stack unavailable or empty library name");
  const size_t before = modules_.size();
  const uint64_t old_next = next_image_;
  try {
    if (LoadRecursive(std::string(name), error)) {
      if (error) error->clear();
      return true;
    }
  } catch (...) {
    if (error) {
      try { *error = "ARM32 library provider or loader threw an exception"; }
      catch (...) { error->clear(); }
    }
  }
  Rollback(before, old_next);
  return false;
}

bool Program::Initialize(std::string* error) {
  if (!stack_ok_ || initialization_failed_)
    return Fail(error, "ARM32 program initialization unavailable after a failure");
  for (auto& module : modules_) {
    if (module.initialized) continue;
    for (uint32_t address : module.image.constructors) {
      const auto result = executor_.Call(address, {}, kStackTop);
      if (!result.returned()) {
        initialization_failed_ = true;
        if (error) *error = "ARM32 constructor failed: " + result.error;
        return false;
      }
    }
    module.initialized = true;
  }
  if (error) error->clear();
  return true;
}

bool Program::CallExport(std::string_view library, std::string_view symbol,
                         const std::vector<Value>& arguments, ValueKind result_kind,
                         Value* result, std::string* error, uint64_t budget) {
  if (!stack_ok_ || initialization_failed_ || !result)
    return Fail(error, "ARM32 program not callable");
  const Module* module = Find(library);
  uint32_t entry = 0;
  if (!module || !module->image.Lookup(symbol, &entry))
    return Fail(error, "ARM32 library export was not found");
  CallFrame frame;
  if (!PackArguments(arguments, &frame, error)) return false;
  const auto executed = executor_.Call(entry, frame, kStackTop, budget);
  if (!executed.returned()) {
    if (error) *error = "ARM32 guest call failed: " + executed.error;
    return false;
  }
  if (!DecodeReturn(result_kind, executed.registers, result, error)) return false;
  if (error) error->clear();
  return true;
}

}  // namespace gesher::arm32
