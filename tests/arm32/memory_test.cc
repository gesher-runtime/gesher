// gesher: adversarial checks for the ARM32 guest address space.
// Host tests of the actual guest memory owner; no ARM execution is claimed here.
#include <gesher/arm32_memory.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
// Exercise real allocator failure during a partially staged Map, rather than
// merely verifying that a configured byte budget rejects a large request.
std::atomic<long> allocations_before_failure{-1};
std::atomic<unsigned> checks{0};

void Check(bool passed, const char* expression, int line) {
  if (!passed) {
    std::fprintf(stderr, "memory_test.cc:%d: %s\n", line, expression);
    std::exit(1);
  }
  ++checks;
}
#define CHECK(expression) Check(static_cast<bool>(expression), #expression, __LINE__)
}  // namespace

void* operator new(std::size_t size) {
  long remaining = allocations_before_failure.load(std::memory_order_relaxed);
  if (remaining >= 0) {
    if (remaining == 0) throw std::bad_alloc();
    allocations_before_failure.fetch_sub(1, std::memory_order_relaxed);
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

namespace {
using gesher::arm32::GuestMemory;
using gesher::arm32::MemoryPermission;
constexpr auto R = MemoryPermission::kRead;
constexpr auto W = MemoryPermission::kWrite;
constexpr auto X = MemoryPermission::kExecute;
constexpr std::size_t P = GuestMemory::kPageSize;

void InvalidRangesAndTopPage() {
  GuestMemory memory(P * 2);
  std::string error;
  CHECK(!memory.Map(0, P, R | W, &error));
  CHECK(!error.empty());
  CHECK(!memory.Map(1, P, R | W));
  CHECK(!memory.Map(P, 0, R | W));
  CHECK(!memory.Map(P, P - 1, R | W));
  CHECK(!memory.Map(P + 1, P, R | W));
  CHECK(!memory.Map(0xfffff000u, P * 2, R | W));
  CHECK(!memory.Map(P, std::numeric_limits<std::uint64_t>::max(), R | W));
  CHECK(!memory.Map(P, P, static_cast<MemoryPermission>(8)));
  CHECK(memory.MappedBytes() == 0);

  CHECK(memory.Map(0xfffff000u, P, R | W | X));
  CHECK(memory.Write32(0xfffffffcu, 0x87654321u));
  std::uint32_t word = 0;
  CHECK(memory.Read32(0xfffffffcu, &word) && word == 0x87654321u);
  CHECK(memory.Fetch32(0xfffffffcu, &word) && word == 0x87654321u);
  std::uint8_t byte = 0;
  CHECK(memory.Read8(0xffffffffu, &byte) && byte == 0x87);
  word = 123;
  CHECK(!memory.Read32(0xfffffffdu, &word) && word == 123);
  CHECK(!memory.Write32(0xfffffffdu, 0));
  CHECK(!memory.IsMapped(0xfffffffdu, 4));
  CHECK(!memory.IsMapped(P, std::numeric_limits<std::uint64_t>::max()));
  CHECK(memory.Read(P, nullptr, 0));
  CHECK(memory.Write(P, nullptr, 0));
  CHECK(!memory.Read(0, nullptr, 0));
  CHECK(!memory.Write(P - 1, nullptr, 0));
  CHECK(!memory.Read(0xfffff000u, nullptr, 1));
  CHECK(!memory.Write(0xfffff000u, nullptr, 1));
  CHECK(!memory.Read8(0xfffff000u, nullptr));
  CHECK(!memory.Read32(0xfffff000u, nullptr));
  CHECK(memory.Unmap(0xfffff000u, P));
  CHECK(memory.MappedBytes() == 0);
  CHECK(!memory.Read8(0xffffffffu, &byte));
}

void ScalarsAndPermissions() {
  GuestMemory memory(P * 3);
  CHECK(memory.Map(P, P * 2, R | W));
  std::array<std::uint8_t, 16> zero{};
  std::array<std::uint8_t, 16> bytes;
  bytes.fill(0xff);
  CHECK(memory.Read(P, bytes.data(), bytes.size()) && bytes == zero);
  CHECK(memory.Write64(P + 1, 0x8877665544332211ull));
  CHECK(memory.Read(P + 1, bytes.data(), 8));
  CHECK(bytes[0] == 0x11 && bytes[1] == 0x22 && bytes[7] == 0x88);
  std::uint8_t b = 0;
  std::uint16_t h = 0;
  std::uint32_t w = 0;
  std::uint64_t d = 0;
  CHECK(memory.Read8(P + 1, &b) && b == 0x11);
  CHECK(memory.Read16(P + 2, &h) && h == 0x3322);
  CHECK(memory.Read32(P + 3, &w) && w == 0x66554433);
  CHECK(memory.Read64(P + 1, &d) && d == 0x8877665544332211ull);
  CHECK(memory.Write8(P + 20, 0xa5));
  CHECK(memory.Write16(P + 21, 0xcdef));
  CHECK(memory.Write32(P + 23, 0x12345678));
  CHECK(memory.Read16(P + 21, &h) && h == 0xcdef);
  CHECK(memory.Read32(P + 23, &w) && w == 0x12345678);
  CHECK(!memory.Fetch32(P + 23, &w));
  CHECK(!memory.Fetch16(P + 21, &h));

  CHECK(memory.Protect(P, P, X));
  CHECK(!memory.Read32(P + 23, &w));
  CHECK(!memory.Write32(P + 23, 0));
  CHECK(memory.Fetch32(P + 23, &w) && w == 0x12345678);
  CHECK(memory.Fetch16(P + 21, &h) && h == 0xcdef);
  CHECK(memory.IsMapped(P, P, X));
  CHECK(!memory.IsMapped(P, P, R));
  CHECK(memory.Protect(P, P, W));
  CHECK(memory.Write32(P + 23, 0x87654321));
  CHECK(!memory.Read32(P + 23, &w));
  CHECK(!memory.Fetch32(P + 23, &w));
  CHECK(memory.Protect(P, P, R));
  CHECK(memory.Read32(P + 23, &w) && w == 0x87654321);
  CHECK(!memory.Write32(P + 23, 0));
  CHECK(memory.Protect(P, P, MemoryPermission::kNone));
  CHECK(memory.IsMapped(P, P));
  CHECK(!memory.IsMapped(P, P, W));
  CHECK(!memory.Read8(P, &b));
  CHECK(!memory.Write8(P, 0));
  CHECK(!memory.Protect(P, P, static_cast<MemoryPermission>(0xff)));
  CHECK(memory.IsMapped(P * 2, P, R | W));
}

void TransactionalTransfers() {
  GuestMemory memory(P * 4);
  CHECK(memory.Map(P, P, R | W));
  CHECK(memory.Map(P * 2, P, R | W));
  // Independent adjacent mappings must behave as one contiguous address range.
  CHECK(memory.Write64(P * 2 - 3, 0x123456789abcdef0ull));
  std::uint64_t value = 0;
  CHECK(memory.Read64(P * 2 - 3, &value) && value == 0x123456789abcdef0ull);
  CHECK(memory.Protect(P * 2, P, R));
  CHECK(!memory.Write64(P * 2 - 3, 0));
  CHECK(memory.Read64(P * 2 - 3, &value) && value == 0x123456789abcdef0ull);
  CHECK(memory.Protect(P * 2, P, W));
  value = 0xfeedfacedeadbeefull;
  CHECK(!memory.Read64(P * 2 - 3, &value) && value == 0xfeedfacedeadbeefull);
  std::array<std::uint8_t, 8> output;
  output.fill(0x5a);
  const auto unchanged = output;
  CHECK(!memory.Read(P * 2 - 3, output.data(), output.size()) && output == unchanged);
  CHECK(memory.Unmap(P * 2, P));
  CHECK(!memory.Write64(P * 2 - 3, 0));
  std::array<std::uint8_t, 3> prefix{};
  CHECK(memory.Read(P * 2 - 3, prefix.data(), prefix.size()));
  CHECK(prefix[0] == 0xf0 && prefix[1] == 0xde && prefix[2] == 0xbc);

  CHECK(memory.Map(P * 3, P, R | W));
  const std::array<std::uint8_t, 2> tail{0xaa, 0xbb};
  CHECK(memory.Write(P * 4 - 2, tail.data(), tail.size()));
  // An unmapped hole must be found before touching the initial mapped page.
  std::vector<std::uint8_t> replacement(P * 3, 0);
  CHECK(!memory.Write(P, replacement.data(), replacement.size()));
  CHECK(memory.Read(P * 2 - 3, prefix.data(), prefix.size()) && prefix[0] == 0xf0);
}

void MappingTransactionsAndBudget() {
  GuestMemory memory(P * 3);
  CHECK(memory.Budget() == P * 3);
  CHECK(memory.Map(P * 2, P, R | W));
  CHECK(memory.Write32(P * 2, 0xabcdef98));
  CHECK(!memory.Map(P, P * 2, R | W));
  CHECK(!memory.IsMapped(P, P));
  CHECK(memory.MappedBytes() == P);
  CHECK(memory.Map(P * 4, P * 2, R | W));
  CHECK(memory.MappedBytes() == P * 3);
  CHECK(!memory.Map(P * 7, P, R));
  CHECK(!memory.IsMapped(P * 7, P));
  CHECK(!memory.Unmap(P * 2, P * 3));
  CHECK(!memory.Protect(P * 2, P * 3, MemoryPermission::kNone));
  CHECK(memory.IsMapped(P * 2, P, R | W));
  CHECK(memory.IsMapped(P * 4, P * 2, R | W));
  CHECK(memory.MappedBytes() == P * 3);
  CHECK(!memory.Unmap(P * 4 + 1, P));
  CHECK(!memory.Unmap(P * 4, 0));
  CHECK(!memory.Protect(P * 4, P - 1, R));
  CHECK(memory.Unmap(P * 4, P));
  CHECK(memory.MappedBytes() == P * 2);
  CHECK(memory.Map(P * 7, P, R | W));
  CHECK(memory.MappedBytes() == P * 3);
  std::uint32_t value = 1;
  CHECK(memory.Read32(P * 7, &value) && value == 0);
  CHECK(memory.Read32(P * 2, &value) && value == 0xabcdef98);
  CHECK(memory.Read32(P * 5, &value) && value == 0);
  GuestMemory no_budget(0);
  CHECK(!no_budget.Map(P, P, R));
  CHECK(no_budget.MappedBytes() == 0);
}

void AllocationRollback() {
  GuestMemory memory(P * 4);
  CHECK(memory.Map(P * 8, P, R | W));
  CHECK(memory.Write32(P * 8, 0xdecafbad));
  for (long fail_after : {0, 1, 2, 3}) {
    allocations_before_failure.store(fail_after);
    const bool mapped = memory.Map(P, P * 2, R | W);
    allocations_before_failure.store(-1);
    CHECK(!mapped);
    CHECK(memory.MappedBytes() == P);
    CHECK(!memory.IsMapped(P, P));
    CHECK(!memory.IsMapped(P * 2, P));
    std::uint32_t value = 0;
    CHECK(memory.Read32(P * 8, &value) && value == 0xdecafbad);
  }
  CHECK(memory.Map(P, P * 2, R | W));
  CHECK(memory.MappedBytes() == P * 3);
}

void CompareExchangeAndConcurrency() {
  GuestMemory memory(P * 2);
  CHECK(memory.Map(P, P, R | W));
  bool exchanged = false;
  CHECK(memory.CompareExchange(P, 1, 0, 0xaa, &exchanged) && exchanged);
  CHECK(memory.CompareExchange(P, 1, 0, 0xbb, &exchanged) && !exchanged);
  CHECK(memory.CompareExchange(P, 2, 0xaa, 0xaabb, &exchanged) && exchanged);
  CHECK(memory.CompareExchange(P, 4, 0xaabb, 0x12345678, &exchanged) && exchanged);
  CHECK(memory.CompareExchange(P, 8, 0x12345678, 0xfedcba9876543210ull, &exchanged) && exchanged);
  std::uint64_t value = 0;
  CHECK(memory.Read64(P, &value) && value == 0xfedcba9876543210ull);
  exchanged = true;
  CHECK(!memory.CompareExchange(P + 1, 4, 0, 1, &exchanged) && exchanged);
  CHECK(!memory.CompareExchange(P, 3, 0, 1, &exchanged) && exchanged);
  CHECK(!memory.CompareExchange(P, 1, 0, 256, &exchanged) && exchanged);
  CHECK(!memory.CompareExchange(P, 1, 256, 0, &exchanged) && exchanged);
  CHECK(!memory.CompareExchange(P * 2, 4, 0, 1, &exchanged) && exchanged);
  CHECK(!memory.CompareExchange(P, 4, 0, 1, nullptr));
  CHECK(memory.Protect(P, P, R));
  CHECK(!memory.CompareExchange(P, 8, value, 0, &exchanged) && exchanged);
  CHECK(memory.Protect(P, P, W));
  CHECK(!memory.CompareExchange(P, 8, value, 0, &exchanged) && exchanged);
  CHECK(memory.Protect(P, P, R | W));
  CHECK(memory.Write64(P, 0));

  constexpr unsigned threads = 4;
  constexpr unsigned increments = 2000;
  std::array<std::thread, threads> workers;
  for (auto& worker : workers) {
    worker = std::thread([&] {
      for (unsigned i = 0; i < increments; ++i) {
        bool changed = false;
        do {
          std::uint64_t before = 0;
          CHECK(memory.Read64(P, &before));
          CHECK(memory.CompareExchange(P, 8, before, before + 1, &changed));
        } while (!changed);
      }
    });
  }
  for (auto& worker : workers) worker.join();
  CHECK(memory.Read64(P, &value) && value == threads * increments);

  // A wide transfer crossing two pages remains indivisible to other transfers.
  CHECK(memory.Map(P * 2, P, R | W));
  constexpr std::uint64_t first = 0xaaaaaaaa55555555ull;
  constexpr std::uint64_t second = 0x55555555aaaaaaaaull;
  CHECK(memory.Write64(P * 2 - 3, first));
  std::atomic<bool> start{false};
  std::thread writer([&] {
    while (!start.load()) std::this_thread::yield();
    for (unsigned i = 0; i < increments; ++i) {
      CHECK(memory.Write64(P * 2 - 3, i % 2 == 0 ? second : first));
    }
  });
  start.store(true);
  for (unsigned i = 0; i < increments; ++i) {
    CHECK(memory.Read64(P * 2 - 3, &value));
    CHECK(value == first || value == second);
  }
  writer.join();
}
}  // namespace

int main() {
  InvalidRangesAndTopPage();
  ScalarsAndPermissions();
  TransactionalTransfers();
  MappingTransactionsAndBudget();
  AllocationRollback();
  CompareExchangeAndConcurrency();
  std::printf("ARM32 guest memory passed: %u checks\n", checks.load());
}
