// gesher: checked scalar libc bridge for a separate ARM32 address domain.
#include <gesher/arm32_bionic.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace gesher::arm32 {
namespace {
constexpr uint64_t kPage = GuestMemory::kPageSize;
constexpr uint64_t kHeapEnd = 0xe0000000;
constexpr size_t kMaxCopy = 64 * 1024 * 1024;
constexpr size_t kMaxString = 1024 * 1024;
constexpr auto kRW = MemoryPermission::kRead | MemoryPermission::kWrite;

bool Fail(std::string* error, const char* reason) {
  if (error) *error = reason;
  return false;
}
bool GoodArguments(const std::vector<Value>& values, size_t count,
                   std::string* error) {
  return values.size() == count || Fail(error, "invalid Bionic32 host argument count");
}
uint32_t Word(const Value& value) { return static_cast<uint32_t>(value.bits); }
Value Pointer(uint32_t address) { return Value::GuestPointer(address); }
uint64_t RoundUp(uint64_t value) { return (value + kPage - 1) & ~(kPage - 1); }

bool CopyGuest(GuestMemory& memory, uint32_t destination, uint32_t source,
               uint32_t size, std::string* error) {
  if (size == 0) return true;
  if (size > kMaxCopy) return Fail(error, "Bionic32 transfer exceeds size limit");
  if (!memory.IsMapped(source, size, MemoryPermission::kRead) ||
      !memory.IsMapped(destination, size, MemoryPermission::kWrite))
    return Fail(error, "Bionic32 memory transfer outside guest address space");
  // Staging the entire transfer gives memmove behavior for overlaps and
  // prevents a read fault from causing a partially written destination.
  std::vector<uint8_t> buffer(size);
  return memory.Read(source, buffer.data(), buffer.size(), error) &&
         memory.Write(destination, buffer.data(), buffer.size(), error);
}

bool FillGuest(GuestMemory& memory, uint32_t destination, uint32_t value,
               uint32_t size, std::string* error) {
  if (size == 0) return true;
  if (!memory.IsMapped(destination, size, MemoryPermission::kWrite))
    return Fail(error, "Bionic32 memset outside writable guest memory");
  const std::array<uint8_t, 4096> bytes = [&] {
    std::array<uint8_t, 4096> result{};
    result.fill(static_cast<uint8_t>(value));
    return result;
  }();
  uint64_t cursor = destination;
  uint64_t remaining = size;
  while (remaining != 0) {
    const size_t length = static_cast<size_t>(std::min<uint64_t>(remaining, bytes.size()));
    if (!memory.Write(static_cast<uint32_t>(cursor), bytes.data(), length, error))
      return false;
    cursor += length;
    remaining -= length;
  }
  return true;
}

bool CompareGuest(GuestMemory& memory, uint32_t left, uint32_t right,
                  uint32_t count, bool stop_at_null, int32_t* out,
                  std::string* error) {
  const uint32_t bounded = std::min<uint32_t>(count, kMaxString);
  for (uint32_t i = 0; i < bounded; ++i) {
    const uint64_t a = uint64_t{left} + i, b = uint64_t{right} + i;
    uint8_t first = 0, second = 0;
    if (a > UINT32_MAX || b > UINT32_MAX ||
        !memory.Read8(static_cast<uint32_t>(a), &first, error) ||
        !memory.Read8(static_cast<uint32_t>(b), &second, error))
      return Fail(error, "Bionic32 string comparison reads inaccessible guest memory");
    if (first != second) {
      *out = int32_t(first) - int32_t(second);
      return true;
    }
    if (stop_at_null && first == 0) {
      *out = 0;
      return true;
    }
  }
  if (count > bounded)
    return Fail(error, "Bionic32 string comparison exceeds scan limit");
  *out = 0;
  return true;
}
}  // namespace

Bionic32::Bionic32(Program& program) : state_(std::make_shared<State>(program)) {}

uint32_t Bionic32::State::Alloc(uint32_t requested) {
  const uint64_t rounded = RoundUp(std::max<uint64_t>(requested, 1));
  if (rounded > kHeapEnd - 0xd1000000) return 0;
  std::lock_guard guard(mutex);
  uint32_t base = 0;
  size_t available = 0;
  auto reusable_it = reusable.end();
  for (auto it = reusable.begin(); it != reusable.end(); ++it) {
    if (it->second >= rounded) {
      base = it->first;
      available = it->second;
      reusable_it = it;
      break;
    }
  }
  const bool reuse = reusable_it != reusable.end();
  if (!reuse) {
    if (next_heap > kHeapEnd || rounded > kHeapEnd - next_heap - kPage)
      return 0;
    base = static_cast<uint32_t>(next_heap);
  }
  if (!program.Memory().Map(base, rounded, kRW)) return 0;
  try {
    live.emplace(base, Allocation{requested, static_cast<size_t>(rounded)});
    if (reuse) {
      reusable.erase(reusable_it);
      if (available > rounded)
        reusable.emplace(static_cast<uint32_t>(uint64_t(base) + rounded),
                         available - static_cast<size_t>(rounded));
    } else {
      next_heap += rounded + kPage;  // Guard page after each allocation.
    }
  } catch (...) {
    program.Memory().Unmap(base, rounded);
    live.erase(base);
    return 0;
  }
  return base;
}

bool Bionic32::State::Free(uint32_t address) {
  if (address == 0) return true;
  std::lock_guard guard(mutex);
  auto it = live.find(address);
  if (it == live.end() ||
      !program.Memory().Unmap(address, it->second.mapped)) return false;
  const size_t mapped = it->second.mapped;
  live.erase(it);
  try { reusable.emplace(address, mapped); }
  catch (...) { /* Free succeeds; only address reuse is lost. */ }
  return true;
}

uint32_t Bionic32::State::Realloc(uint32_t address, uint32_t requested) {
  if (address == 0) return Alloc(requested);
  if (requested == 0) return Free(address) ? 0 : 0;
  uint32_t old_size;
  {
    std::lock_guard guard(mutex);
    auto it = live.find(address);
    if (it == live.end()) return 0;
    if (requested <= it->second.mapped) {
      it->second.requested = requested;
      return address;
    }
    old_size = static_cast<uint32_t>(it->second.requested);
  }
  const uint32_t replacement = Alloc(requested);
  if (!replacement) return 0;
  const auto copy = std::min(old_size, requested);
  if (!CopyGuest(program.Memory(), replacement, address, copy, nullptr)) {
    Free(replacement);
    return 0;
  }
  Free(address);
  return replacement;
}

bool Bionic32::State::ReadCString(uint32_t address, std::string* result,
                                  size_t maximum) const {
  if (!result || address == 0) return false;
  std::string text;
  std::array<char, GuestMemory::kPageSize> buffer;
  uint64_t cursor = address;
  size_t count = 0;
  while (count < maximum) {
    const size_t in_page = static_cast<size_t>(cursor % kPage);
    const size_t block = std::min({maximum - count, size_t(kPage) - in_page,
                                   static_cast<size_t>(GuestMemory::kAddressLimit - cursor)});
    if (block == 0 || !program.Memory().Read(static_cast<uint32_t>(cursor),
                                             buffer.data(), block))
      return false;
    const void* terminator = std::memchr(buffer.data(), 0, block);
    const size_t length = terminator
        ? static_cast<const char*>(terminator) - buffer.data() : block;
    text.append(buffer.data(), length);
    if (terminator) {
      *result = std::move(text);
      return true;
    }
    count += block;
    cursor += block;
  }
  return false;
}

bool Bionic32::Install(std::string* error) {
  const auto s = state_;
  Program& p = s->program;
  const auto bind = [&](const char* name, std::vector<ValueKind> kinds,
                        ValueKind result, HostCallback callback) {
    return p.BindHostSymbol(name, std::move(kinds), result,
                            std::move(callback), error);
  };
  using K = ValueKind;
  if (!bind("malloc", {K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 1, e)) return false;
        *r = Pointer(s->Alloc(Word(a[0])));
        return true;
      }) ||
      !bind("calloc", {K::U32, K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 2, e)) return false;
        uint32_t count = Word(a[0]), size = Word(a[1]);
        if (size != 0 && count > UINT32_MAX / size) {
          *r = Pointer(0);
          return true;
        }
        *r = Pointer(s->Alloc(count * size));
        return true;
      }) ||
      !bind("free", {K::Pointer32}, K::Void,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 1, e) || !s->Free(Word(a[0])))
          return Fail(e, "Bionic32 free on invalid guest allocation");
        *r = {K::Void, 0};
        return true;
      }) ||
      !bind("realloc", {K::Pointer32, K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 2, e)) return false;
        *r = Pointer(s->Realloc(Word(a[0]), Word(a[1])));
        return true;
      }) ||
      !bind("memcpy", {K::Pointer32, K::Pointer32, K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 3, e) ||
            !CopyGuest(s->program.Memory(), Word(a[0]), Word(a[1]), Word(a[2]), e))
          return false;
        *r = Pointer(Word(a[0]));
        return true;
      }) ||
      !bind("memmove", {K::Pointer32, K::Pointer32, K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 3, e) ||
            !CopyGuest(s->program.Memory(), Word(a[0]), Word(a[1]), Word(a[2]), e))
          return false;
        *r = Pointer(Word(a[0]));
        return true;
      }) ||
      !bind("memset", {K::Pointer32, K::I32, K::U32}, K::Pointer32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 3, e) ||
            !FillGuest(s->program.Memory(), Word(a[0]), Word(a[1]), Word(a[2]), e))
          return false;
        *r = Pointer(Word(a[0]));
        return true;
      }) ||
      !bind("strlen", {K::Pointer32}, K::U32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 1, e)) return false;
        std::string str;
        if (!s->ReadCString(Word(a[0]), &str, kMaxString))
          return Fail(e, "Bionic32 strlen on unterminated or unreadable string");
        *r = Value::Unsigned32(static_cast<uint32_t>(str.size()));
        return true;
      }) ||
      !bind("strnlen", {K::Pointer32, K::U32}, K::U32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 2, e)) return false;
        const auto limit = std::min<uint32_t>(Word(a[1]), kMaxString);
        if (limit == 0) { *r = Value::Unsigned32(0); return true; }
        std::string str;
        if (s->ReadCString(Word(a[0]), &str, limit)) {
          *r = Value::Unsigned32(static_cast<uint32_t>(str.size()));
          return true;
        }
        if (!s->program.Memory().IsMapped(Word(a[0]), limit, MemoryPermission::kRead))
          return Fail(e, "Bionic32 strnlen on unreadable guest memory");
        *r = Value::Unsigned32(limit);
        return true;
      }) ||
      !bind("memcmp", {K::Pointer32, K::Pointer32, K::U32}, K::I32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 3, e)) return false;
        const uint32_t count = Word(a[2]);
        if (count > kMaxCopy) return Fail(e, "Bionic32 memcmp exceeds limit");
        int32_t value = 0;
        if (!CompareGuest(s->program.Memory(), Word(a[0]), Word(a[1]),
                          count, false, &value, e)) return false;
        *r = Value::Signed32(value);
        return true;
      }) ||
      !bind("strcmp", {K::Pointer32, K::Pointer32}, K::I32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 2, e)) return false;
        int32_t value = 0;
        if (!CompareGuest(s->program.Memory(), Word(a[0]), Word(a[1]),
                          kMaxString, true, &value, e)) return false;
        *r = Value::Signed32(value);
        return true;
      }) ||
      !bind("strncmp", {K::Pointer32, K::Pointer32, K::U32}, K::I32,
      [s](const auto& a, Value* r, std::string* e) {
        if (!GoodArguments(a, 3, e)) return false;
        int32_t value = 0;
        if (!CompareGuest(s->program.Memory(), Word(a[0]), Word(a[1]),
                          Word(a[2]), true, &value, e)) return false;
        *r = Value::Signed32(value);
        return true;
      }))
    return false;

  if (error) error->clear();
  return true;
}
}  // namespace gesher::arm32
