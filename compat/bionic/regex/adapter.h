// Allocation, work and byte-ABI boundary for the pinned no-NLS Bionic engine.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace gesher::bionic::posix_regex {

constexpr int kRegOk = 0, kRegNomatch = 1, kRegBadpat = 2, kRegEspace = 12, kRegInvarg = 16;
constexpr int kRegExtended = 1, kRegIcase = 2, kRegNosub = 4, kRegNewline = 8,
              kRegNospec = 16, kRegPend = 32, kRegGnu = 256;
constexpr int kRegNotbol = 1, kRegNoteol = 2, kRegStartend = 4;
constexpr size_t kMaxWorkBytes = 64 * 1024 * 1024;
constexpr size_t kMaxCompileSteps = 50000000, kMaxMatchSteps = 50000000;
constexpr size_t kMaxBackrefSteps = 2000000, kMaxDepth = 256;

// Limits are explicit call parameters. Ordinary entry points always use these
// defaults; small injected limits exercise failure/cleanup without large inputs.
struct Limits {
  size_t bytes = kMaxWorkBytes;
  size_t steps = kMaxMatchSteps;
  size_t depth = kMaxDepth;
};
struct ResourceLimit {};

class Arena {
  struct alignas(std::max_align_t) Block {
    Block* previous;
    Block* next;
    Arena* owner;
    size_t bytes;
  };
  Block* first_ = nullptr;
  size_t used_ = 0;
  size_t limit_;
 public:
  explicit Arena(size_t limit = kMaxWorkBytes) : limit_(limit) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  ~Arena() { while (first_ != nullptr) Free(first_ + 1); }
  size_t used() const { return used_; }
  void SetLimit(size_t limit) { if (used_ > limit) throw ResourceLimit{}; limit_ = limit; }
  void* Allocate(size_t bytes) {
    // Include allocation headers and old/new storage during a realloc copy.
    if (bytes > limit_ || sizeof(Block) > limit_ - bytes ||
        used_ > limit_ - bytes - sizeof(Block)) throw ResourceLimit{};
    size_t total = bytes + sizeof(Block);
    auto* block = static_cast<Block*>(std::malloc(total));
    if (block == nullptr) throw ResourceLimit{};
    *block = {nullptr, first_, this, total};
    if (first_ != nullptr) first_->previous = block;
    first_ = block;
    used_ += total;
    return block + 1;
  }
  static void Free(void* value) noexcept {
    if (value == nullptr) return;
    auto* block = static_cast<Block*>(value) - 1;
    Arena* arena = block->owner;
    if (block->previous != nullptr) block->previous->next = block->next;
    else arena->first_ = block->next;
    if (block->next != nullptr) block->next->previous = block->previous;
    arena->used_ -= block->bytes;
    std::free(block);
  }
  static size_t Size(void* value) {
    return value == nullptr ? 0 : (static_cast<Block*>(value) - 1)->bytes - sizeof(Block);
  }
};

struct Context {
  Arena& arena;
  Limits limits;
  bool unsigned_char;
  size_t mb_cur_max;
  size_t remaining;
  size_t depth = 0;
  Context(Arena& memory, Limits cap, bool byte_abi, size_t mb)
      : arena(memory), limits(cap), unsigned_char(byte_abi), mb_cur_max(mb), remaining(cap.steps) {}
};
inline thread_local Context* current = nullptr;
class Scope {
  Context* previous_;
 public:
  explicit Scope(Context& context) : previous_(current) { current = &context; }
  ~Scope() { current = previous_; }
};
inline void WorkFor(Context& context, size_t count = 1) {
  if (count > context.remaining) throw ResourceLimit{};
  context.remaining -= count;
}
inline void Work(size_t count = 1) { WorkFor(*current, count); }
class DepthGuard {
 public:
  DepthGuard() {
    Work();
    if (current->depth >= current->limits.depth) throw ResourceLimit{};
    ++current->depth;
  }
  ~DepthGuard() { --current->depth; }
};
inline int GuestChar(char c) {
  return current->unsigned_char ? static_cast<unsigned char>(c) : static_cast<signed char>(c);
}
inline int GuestCharMin() { return current->unsigned_char ? 0 : -128; }
inline int GuestCharMax() { return current->unsigned_char ? 255 : 127; }
inline void* Alloc(size_t size) { Work(); return current->arena.Allocate(size); }
inline void* Calloc(size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) throw ResourceLimit{};
  size_t bytes = count * size;
  Work(bytes);
  void* result = Alloc(bytes);
  std::memset(result, 0, bytes);
  return result;
}
template<class T> T* ReallocArray(void* old, size_t count, size_t size) {
  if (size != 0 && count > SIZE_MAX / size) throw ResourceLimit{};
  size_t bytes = count * size;
  size_t copied = std::min(bytes, Arena::Size(old));
  Work(copied);
  void* result = Alloc(bytes);
  if (copied != 0) std::memcpy(result, old, copied);
  Arena::Free(old);
  return static_cast<T*>(result);
}
inline void* CopyFor(Context& context, void* out, const void* in, size_t n) { WorkFor(context, n); return std::memcpy(out, in, n); }
inline void* MoveFor(Context& context, void* out, const void* in, size_t n) { WorkFor(context, n); return std::memmove(out, in, n); }
inline void* FillFor(Context& context, void* out, int value, size_t n) { WorkFor(context, n); return std::memset(out, value, n); }
inline int CompareFor(Context& context, const void* a, const void* b, size_t n) { WorkFor(context, n); return std::memcmp(a, b, n); }
inline size_t Length(const char* value) {
  size_t n = 0;
  for (;;) { Work(); if (value[n] == 0) return n; ++n; }
}
inline int StringCompare(const char* a, const char* b) {
  for (;;) {
    Work();
    int difference = static_cast<unsigned char>(*a) - static_cast<unsigned char>(*b);
    if (difference || !*a) return difference;
    ++a; ++b;
  }
}
inline int StringCompareN(const char* a, const char* b, size_t n) {
  Work(n); return std::strncmp(a, b, n);
}
// Android's no-NLS regex build has C classification even in UTF-8 locales.
inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
inline int islower(int c) { return c >= 'a' && c <= 'z'; }
inline int isalpha(int c) { return isupper(c) || islower(c); }
inline int isdigit(int c) { return c >= '0' && c <= '9'; }
inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
inline int isblank(int c) { return c == ' ' || c == '\t'; }
inline int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
inline int isgraph(int c) { return c >= 33 && c <= 126; }
inline int isprint(int c) { return c >= 32 && c <= 126; }
inline int ispunct(int c) { return isgraph(c) && !isalnum(c); }
inline int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
inline int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
inline int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }

}  // namespace gesher::bionic::posix_regex
