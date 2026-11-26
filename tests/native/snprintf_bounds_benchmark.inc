// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Separate allocation-observed builds; CPU comparisons use ordinary allocators.
namespace snprintf_allocation {
struct Counts { size_t calls = 0, requested = 0, live = 0, peak = 0; };
thread_local Counts counts;
thread_local bool active = false;
#ifdef SNPRINTF_OBSERVE_ALLOCATIONS
struct alignas(std::max_align_t) Header { size_t bytes; bool observed; };
[[gnu::noinline]] void* Allocate(size_t size) {
  if (size > SIZE_MAX - sizeof(Header)) throw std::bad_alloc();
  auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
  if (!header) throw std::bad_alloc();
  header->bytes = size;
  header->observed = active;
  if (active) {
    ++counts.calls;
    counts.requested += size;
    counts.live += size;
    counts.peak = std::max(counts.peak, counts.live);
  }
  return header + 1;
}
[[gnu::noinline]] void Release(void* pointer) noexcept {
  if (!pointer) return;
  auto* header = static_cast<Header*>(pointer) - 1;
  if (header->observed) counts.live -= header->bytes;
  std::free(header);
}
#endif
}  // namespace snprintf_allocation
#ifdef SNPRINTF_OBSERVE_ALLOCATIONS
void* operator new(size_t size) { return snprintf_allocation::Allocate(size); }
void* operator new[](size_t size) { return snprintf_allocation::Allocate(size); }
void operator delete(void* pointer) noexcept { snprintf_allocation::Release(pointer); }
void operator delete[](void* pointer) noexcept { snprintf_allocation::Release(pointer); }
void operator delete(void* pointer, size_t) noexcept { snprintf_allocation::Release(pointer); }
void operator delete[](void* pointer, size_t) noexcept { snprintf_allocation::Release(pointer); }
#endif

static uint64_t SnprintfCpuTime() {
  timespec now;
  Check("caller CPU clock available", clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
  return static_cast<uint64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

static int SnprintfBenchmark(const std::string& name, size_t repetitions) {
  std::string input, format, expected;
  size_t capacity = 0;
  int kind = 0;
  if (name == "tiny") {
    kind = 1; capacity = 8; expected = "42";
  } else if (name == "short") {
    kind = 2; capacity = 64; expected = "-214/payload:feed1234";
  } else if (name == "padding") {
    kind = 3; capacity = 32;
    expected.assign(1048575, ' '); expected += 'x';
  } else if (name == "mixed") {
    kind = 4; capacity = 128; input.assign(262144, 'y');
    expected = "p:" + input + ":00000123:tail";
  } else if (name == "literal") {
    kind = 5; capacity = 32; format.assign(4194304, 'L'); expected = format;
  } else {
    Check("known benchmark case", name == "truncated" || name == "sizing" ||
          name == "medium" || name == "complete");
    size_t length = name == "medium" ? 1024 : name == "complete" ? 1048576 : 4194304;
    input.assign(length, 'a');
    for (size_t i = 0; i < input.size(); i += 257) input[i] = 'Q';
    expected = input;
    capacity = name == "sizing" ? 0 : name == "truncated" ? 32 : length + 1;
  }
  std::vector<unsigned char> destination(capacity + 16, 0xa5);
  size_t retained = capacity ? std::min(capacity - 1, expected.size()) : 0;
  char* output = capacity ? reinterpret_cast<char*>(destination.data()) : nullptr;
  auto once = [&]() {
    int count;
    switch (kind) {
      case 1: count = Invoke(output, capacity, "%d", 42); break;
      case 2: count = Invoke(output, capacity, "%d/%s:%08x", -214, "payload", 0xfeed1234U); break;
      case 3: count = Invoke(output, capacity, "%1048576s", "x"); break;
      case 4: count = Invoke(output, capacity, "p:%s:%08d:tail", input.c_str(), 123); break;
      case 5: count = Invoke(output, capacity, format.c_str()); break;
      default: count = Invoke(output, capacity, "%s", input.c_str()); break;
    }
    Check("benchmark complete would-have-written count", count == static_cast<int>(expected.size()));
    if (capacity) {
      Check("benchmark retained bytes", std::memcmp(output, expected.data(), retained) == 0);
      Check("benchmark terminator", output[retained] == 0);
    }
    for (size_t i = capacity; i < destination.size(); ++i)
      Check("benchmark destination canary", destination[i] == 0xa5);
  };
  for (const char* phase : {"first", "steady"}) {
    size_t iterations = phase[0] == 'f' ? 1 : repetitions;
    Check("no live observed storage before phase", snprintf_allocation::counts.live == 0);
    snprintf_allocation::counts = {};
    snprintf_allocation::active = true;
    auto wall_start = std::chrono::steady_clock::now();
    uint64_t cpu_start = SnprintfCpuTime();
    for (size_t i = 0; i < iterations; ++i) once();
    uint64_t cpu = SnprintfCpuTime() - cpu_start;
    auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_start).count();
    snprintf_allocation::active = false;
    Check("format storage released before return", snprintf_allocation::counts.live == 0);
    std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"iterations\":%zu,"
                "\"would_bytes\":%zu,\"retained_bytes\":%zu,\"cpu_ns\":%llu,\"wall_ns\":%lld,",
                name.c_str(), phase, iterations, expected.size(), retained,
                static_cast<unsigned long long>(cpu), static_cast<long long>(wall));
#ifdef SNPRINTF_OBSERVE_ALLOCATIONS
    const auto& a = snprintf_allocation::counts;
    std::printf("\"cpp_allocations\":%zu,\"cpp_requested_bytes\":%zu,\"cpp_peak_live_bytes\":%zu}\n",
                a.calls, a.requested, a.peak);
#else
    std::printf("\"cpp_allocations\":null,\"cpp_requested_bytes\":null,\"cpp_peak_live_bytes\":null}\n");
#endif
  }
  return 0;
}

