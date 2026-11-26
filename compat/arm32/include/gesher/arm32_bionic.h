// gesher: bounded ILP32 Bionic-like libc adapters for an isolated ARM32 guest.
// This is deliberately not a complete Android libc or an ART/JNI bridge.
#pragma once

#include <gesher/arm32_program.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace gesher::arm32 {

// A single-program, pointer-width-correct subset of Android's libc surface.
// Every native pointer stays in GuestMemory. Never reinterpret_cast a guest
// value to a host pointer. Instances are cheap: registered callbacks retain
// shared state until Program is destroyed. Program must outlive this object.
class Bionic32 {
 public:
  explicit Bionic32(Program& program);
  ~Bionic32() = default;
  Bionic32(const Bionic32&) = delete;
  Bionic32& operator=(const Bionic32&) = delete;

  // Adds malloc/calloc/realloc/free, strlen/strnlen, memcpy/memmove/memset,
  // memcmp, strcmp and strncmp as typed ARM32 import thunks. Refuses duplicate
  // names. Installation is for a newly created Program with no guest code
  // loaded: successful registrations are not automatically removed on error.
  bool Install(std::string* error = nullptr);

 private:
  struct State {
    explicit State(Program& p) : program(p) {}
    Program& program;
    std::mutex mutex;
    struct Allocation { size_t requested; size_t mapped; };
    std::map<uint32_t, Allocation> live;
    std::map<uint32_t, size_t> reusable;
    uint64_t next_heap = 0xd1000000;

    uint32_t Alloc(uint32_t requested);
    bool Free(uint32_t address);
    uint32_t Realloc(uint32_t address, uint32_t requested);
    bool ReadCString(uint32_t address, std::string* result,
                     size_t max_length = 1024 * 1024) const;
  };
  std::shared_ptr<State> state_;
};

}  // namespace gesher::arm32
