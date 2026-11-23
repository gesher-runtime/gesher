// Included after the complete production adapter. The external C cores call
// these exact public I/O entries; the optional hook only schedules fault/race
// boundaries before a real callback. It never supplies parser input/results.
#include "stdio_scan_contract.inc"

using ScanHook = void (*)(FILE*, bool, void*);
static thread_local ScanHook scan_hook;
static thread_local void* scan_hook_argument;
static void ScanBoundary(FILE* stream, bool wide) {
  if (scan_hook) scan_hook(stream, wide, scan_hook_argument);
}
extern "C" int gesher_test_scan_getc(FILE* stream) {
  ScanBoundary(stream, false);
  return adapter_getc(stream);
}
extern "C" int gesher_test_scan_ungetc(int value, FILE* stream) {
  return adapter_ungetc(value, stream);
}
extern "C" wint_t gesher_test_scan_getwc(FILE* stream) {
  ScanBoundary(stream, true);
  return adapter_getwc(stream);
}
extern "C" wint_t gesher_test_scan_ungetwc(wint_t value, FILE* stream) {
  return adapter_ungetwc(value, stream);
}
static int ScanInteger(FILE* stream, bool wide, int* value) {
  return wide ? adapter_fwscanf(stream, L"%d", value) : adapter_fscanf(stream, "%d", value);
}
static int ScanCharacter(FILE* stream, bool wide) {
  return wide ? static_cast<int>(adapter_fgetwc(stream)) : adapter_fgetc(stream);
}
static void CheckScopeRestored(FILE* stream, bool wide, int expected) {
  const auto visits = scan_registry_visits;
  check("scan scope restores ordinary callback ownership", ScanCharacter(stream, wide) == expected);
  if (!GESHER_SCAN_MEASURE)
    check("after scan each public read finds its owner", scan_registry_visits - visits == 1);
}
static void TestNormalScopes() {
  for (bool wide : {false, true}) {
    char path[64];
    FILE* stream = ScanFixture("4!", 2, path);
    if (!stream) continue;
    int value = 0;
    check("normal scanner consumes actual input", ScanInteger(stream, wide, &value) == 1 && value == 4);
    CheckScopeRestored(stream, wide, '!');
    ScanClose(stream, path);
  }
}

struct NestedScan {
  FILE* target;
  bool wide;
  bool entered = false;
  int assigned = 0;
};
static void NestedScanHook(FILE*, bool wide, void* raw) {
  auto& nested = *static_cast<NestedScan*>(raw);
  scan_hook = nullptr;
  nested.entered = true;
  check("nested scan retains the requested callback family", wide == nested.wide);
  check("nested scan uses its real stream", ScanInteger(nested.target, wide, &nested.assigned) == 1);
}
static void TestNestedScans(bool budget) {
  for (bool wide : {false, true}) {
    for (bool same : {false, true}) {
      char outer_path[64], inner_path[64];
      const char* data = same ? "17 24!" : "24!";
      FILE* outer = ScanFixture(data, std::strlen(data), outer_path);
      FILE* inner = same ? outer : ScanFixture("17?", 3, inner_path);
      if (!outer || !inner) {
        if (outer) ScanClose(outer, outer_path);
        if (inner && !same) ScanClose(inner, inner_path);
        continue;
      }
      NestedScan nested{inner, wide};
      scan_hook = NestedScanHook;
      scan_hook_argument = &nested;
      const auto visits = scan_registry_visits;
      int value = 0;
      check("outer scan survives a nested parser", ScanInteger(outer, wide, &value) == 1 && value == 24 && nested.entered && nested.assigned == 17);
      scan_hook = nullptr;
      if (budget && !GESHER_SCAN_MEASURE)
        check("nested scopes restore the outer pinned owner", scan_registry_visits - visits == 2);
      CheckScopeRestored(outer, wide, '!');
      if (!same) { CheckScopeRestored(inner, wide, '?'); ScanClose(inner, inner_path); }
      ScanClose(outer, outer_path);
    }
  }
}

static void OtherStreamHook(FILE*, bool wide, void* raw) {
  scan_hook = nullptr;
  FILE* other = static_cast<FILE*>(raw);
  check("scanner callback keeps other-stream identity", ScanCharacter(other, wide) == 'x');
  const int pushed = wide ? static_cast<int>(adapter_ungetwc(L'y', other)) : adapter_ungetc('y', other);
  check("scanner callback pushes back only to the requested stream", pushed == 'y' && ScanCharacter(other, wide) == 'y');
}
static void TestOtherStreamCallbacks() {
  for (bool wide : {false, true}) {
    char outer_path[64], other_path[64];
    FILE* outer = ScanFixture("24!", 3, outer_path);
    FILE* other = ScanFixture("xz", 2, other_path);
    if (!outer || !other) {
      if (outer) ScanClose(outer, outer_path);
      if (other) ScanClose(other, other_path);
      continue;
    }
    scan_hook = OtherStreamHook;
    scan_hook_argument = other;
    int value = 0;
    check("other-stream callback leaves scanning input intact", ScanInteger(outer, wide, &value) == 1 && value == 24);
    CheckScopeRestored(outer, wide, '!');
    CheckScopeRestored(other, wide, 'z');
    ScanClose(other, other_path); ScanClose(outer, outer_path);
  }
}
static void ReregisterHook(FILE* stream, bool, void*) {
  scan_hook = nullptr;
  const int fd = _fileno(stream);
  // Exercise the actual owner retirement/publication boundary with the same
  // live FILE token. A previously buffered owner must not survive registration.
  gesher::stdio::Forget(stream);
  check("reregistered scanner receives replacement bytes", pwrite(fd, "24!", 3, 0) == 3 && lseek(fd, 0, SEEK_SET) == 0);
  check("scanner token registers its new actual owner", gesher::stdio::Register(stream, O_RDONLY) == 0);
}
static void TestReregisteredOwner() {
  for (bool wide : {false, true}) {
    char path[64];
    FILE* stream = ScanFixture("17!", 3, path);
    if (!stream) continue;
    check("old scanner owner has real buffered input", ScanCharacter(stream, wide) == '1');
    const int pushed = wide ? static_cast<int>(adapter_ungetwc(L'1', stream)) : adapter_ungetc('1', stream);
    check("old scanner owner has real pushback", pushed == '1');
    std::weak_ptr<void> old = gesher::stdio::Find(stream, false);
    scan_hook = ReregisterHook;
    int value = 0;
    check("retired scanner owner cannot supply stale buffered bytes", ScanInteger(stream, wide, &value) == 1 && value == 24);
    check("scanner releases its retired owner after the call", old.expired());
    CheckScopeRestored(stream, wide, '!');
    ScanClose(stream, path);
  }
}

static void ThrowingScanHook(FILE*, bool, void*) {
  scan_hook = nullptr;
  throw 23;  // Explicit observer fault; the real parser has not consumed input.
}
static void TestExceptionalScopes() {
  for (bool wide : {false, true}) {
    char path[64];
    FILE* stream = ScanFixture("4!", 2, path);
    if (!stream) continue;
    scan_hook = ThrowingScanHook;
    bool caught = false;
    try { int value; ScanInteger(stream, wide, &value); }
    catch (int value) { caught = value == 23; }
    scan_hook = nullptr;
    check("scan observer exception unwinds the actual parser", caught);
    CheckScopeRestored(stream, wide, '4');
    check("scan exception leaves lock ownership available", adapter_ftrylockfile(stream) == 0);
    adapter_funlockfile(stream);
    ScanClose(stream, path);
  }
}

struct CanceledScan {
  FILE* stream;
  bool wide;
  std::atomic<bool> reading{false};
  std::atomic<bool> cleanup{false};
  std::atomic<bool> restored{false};
};
static void ScanReadingHook(FILE*, bool, void* raw) {
  scan_hook = nullptr;
  static_cast<CanceledScan*>(raw)->reading.store(true, std::memory_order_release);
}
static void ScanAfterCancellation(void* raw) {
  auto& call = *static_cast<CanceledScan*>(raw);
  // This runs in the canceled thread after the complete scanner/owner cleanup.
  // PR73 makes zero-length reads return before owner lookup. A rejected EOF
  // pushback still passes through the actual scanner callback/owner path,
  // without consuming input or changing the pushback buffer. It therefore
  // proves that no stale callback context survived the complete unwinding.
  const auto visits = scan_registry_visits;
  const bool rejected = adapter_ungetc(EOF, call.stream) == EOF;
  call.restored = rejected && (GESHER_SCAN_MEASURE || scan_registry_visits - visits == 1);
  call.cleanup = true;
}
static void* CanceledScanCall(void* raw) {
  auto& call = *static_cast<CanceledScan*>(raw);
  gesher::CancellationCleanup after(ScanAfterCancellation, raw);
  scan_hook = ScanReadingHook;
  scan_hook_argument = raw;
  int value;
  ScanInteger(call.stream, call.wide, &value);
  return nullptr;
}
static void TestCanceledScans() {
  for (bool wide : {false, true}) {
    int ends[2];
    check("scan cancellation pipe opens", pipe(ends) == 0);
    FILE* stream = adapter_fdopen(ends[0], "r");
    check("scan cancellation stream opens", stream != nullptr);
    if (!stream) { close(ends[0]); close(ends[1]); continue; }
    CanceledScan call{stream, wide};
    pthread_t worker;
    const int created = pthread_create(&worker, nullptr, CanceledScanCall, &call);
    check("scan cancellation worker starts", created == 0);
    if (created == 0) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!call.reading.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      check("scan blocks in real descriptor input", call.reading.load());
      const int acquired = adapter_ftrylockfile(stream);
      check("scan holds its stream lock across parser input", acquired == EBUSY);
      if (acquired == 0) adapter_funlockfile(stream);
      std::atomic<bool> entered{false}, finished{false};
      std::thread contender([&] {
        entered = true;
        adapter_ungetc(EOF, stream);
        finished = true;
      });
      while (!entered.load()) std::this_thread::yield();
      const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
      while (!finished.load() && std::chrono::steady_clock::now() < wait_until) std::this_thread::yield();
      check("another thread cannot borrow a scanner owner", !finished.load());
      check("real blocked scanner accepts cancellation", pthread_cancel(worker) == 0);
      void* returned = nullptr;
      check("real blocked scanner unwinds and joins", pthread_join(worker, &returned) == 0 && returned == PTHREAD_CANCELED);
      contender.join();
      check("scanner cancellation restores the callback scope", call.cleanup && call.restored);
      check("contending read finishes after scanner cancellation", finished.load());
      const int lock = adapter_ftrylockfile(stream);
      check("scanner cancellation releases CRT and stream locks", lock == 0);
      if (lock == 0) adapter_funlockfile(stream);
      check("scanner recovery receives actual bytes", write(ends[1], "6!", 2) == 2);
      int value = 0;
      check("canceled scanner remains usable", ScanInteger(stream, wide, &value) == 1 && value == 6);
      CheckScopeRestored(stream, wide, '!');
    }
    check("canceled scanner closes normally", adapter_fclose(stream) == 0);
    close(ends[1]);
  }
}

static void TestLineFlush() {
  int input[2], output[2];
  check("scan line-flush input pipe opens", pipe(input) == 0);
  check("scan line-flush output pipe opens", pipe(output) == 0);
  FILE* reader = adapter_fdopen(input[0], "r");
  FILE* writer = adapter_fdopen(output[1], "w");
  check("scan line-flush streams open", reader && writer);
  if (reader && writer) {
    char pending[128];
    check("scan line output configures", adapter_setvbuf(writer, pending, _IOLBF, sizeof(pending)) == 0);
    check("scan input is unbuffered", adapter_setvbuf(reader, nullptr, _IONBF, 0) == 0);
    check("scan line output remains pending", adapter_fputs("pending", writer) >= 0);
    check("scan line-flush supplies input", write(input[1], "7!", 2) == 2);
    int value = 0;
    check("unbuffered scanner consumes actual input", adapter_fscanf(reader, "%d", &value) == 1 && value == 7);
    check("line output peer becomes nonblocking", fcntl(output[0], F_SETFL, O_NONBLOCK) == 0);
    char bytes[7] = {};
    check("unbuffered scanning flushes other line-buffered output", read(output[0], bytes, sizeof(bytes)) == 7 && std::memcmp(bytes, "pending", 7) == 0);
    check("scan line-flush output closes", adapter_fclose(writer) == 0);
    check("scan line-flush input closes", adapter_fclose(reader) == 0);
  } else {
    if (reader) adapter_fclose(reader); else close(input[0]);
    if (writer) adapter_fclose(writer); else close(output[1]);
  }
  close(input[1]); close(output[0]);
}

static uint64_t ScanCpuNs() {
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) std::abort();
  return uint64_t(time.tv_sec) * 1000000000ull + time.tv_nsec;
}
struct ScanWorkStart {
  uint64_t visits = scan_registry_visits, locks = scan_state_locks, crt = crt_lock_attempts.load();
  uint64_t cpu = ScanCpuNs();
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};
static void ReportScanWork(const char* name, unsigned round, unsigned records, uint64_t consumed,
                           const ScanWorkStart& start, bool budget) {
  const uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start.start).count();
  const uint64_t cpu_ns = ScanCpuNs() - start.cpu;
  const uint64_t visits = scan_registry_visits - start.visits, locks = scan_state_locks - start.locks;
  const uint64_t crt = crt_lock_attempts.load() - start.crt;
  if (budget && !GESHER_SCAN_MEASURE) {
    check("one registry visit per formatted-input call", visits == records);
    check("one state lock per formatted-input call", locks == records);
    check("one CRT lock per formatted-input call", crt == records);
  }
  char line[512];
  const int length = std::snprintf(line, sizeof(line),
      "SCAN_SAMPLE {\"name\":\"%s\",\"round\":%u,\"records\":%u,\"input_bytes\":%llu,\"registry_visits\":%llu,\"state_locks\":%llu,\"crt_locks\":%llu,\"cpu_ns\":%llu,\"elapsed_ns\":%llu}\n",
      name, round, records, static_cast<unsigned long long>(consumed), static_cast<unsigned long long>(visits),
      static_cast<unsigned long long>(locks), static_cast<unsigned long long>(crt),
      static_cast<unsigned long long>(cpu_ns), static_cast<unsigned long long>(elapsed));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(line) || write(STDOUT_FILENO, line, length) != length) std::abort();
}
static void TokenWork(const char* name, size_t width, unsigned records, bool wide, bool budget, bool timing) {
  std::string token(width, 'q'), input;
  input.reserve((width + 1) * records);
  for (unsigned i = 0; i < records; ++i) { input += token; input += '!'; }
  char path[64];
  FILE* stream = ScanFixture(input.data(), input.size(), path);
  if (!stream) return;
  const std::string format = "%" + std::to_string(width) + "[a-z]!%n";
  const std::wstring wformat = L"%" + std::to_wstring(width) + L"l[a-z]!%n";
  std::vector<char> text(width + 1);
  std::vector<wchar_t> wtext(width + 1);
  const unsigned rounds = timing ? 8 : 1;
  for (unsigned round = 0; round < rounds; ++round) {
    check("scan work repositions real input", adapter_fseek(stream, 0, SEEK_SET) == 0);
    const ScanWorkStart start;
    uint64_t consumed = 0;
    for (unsigned i = 0; i < records; ++i) {
      int count = -1;
      const int result = wide ? adapter_fwscanf(stream, wformat.c_str(), wtext.data(), &count)
                              : adapter_fscanf(stream, format.c_str(), text.data(), &count);
      bool exact = result == 1 && count == static_cast<int>(width + 1);
      if (wide) exact = exact && wtext[width] == 0 && std::all_of(wtext.begin(), wtext.begin() + width, [](wchar_t c) { return c == L'q'; });
      else exact = exact && text[width] == 0 && std::memcmp(text.data(), token.data(), width) == 0;
      check("scanner work preserves every destination and count", exact);
      consumed += exact ? static_cast<unsigned>(count) : 0;
    }
    ReportScanWork(name, round, records, consumed, start, budget);
  }
  ScanClose(stream, path);
}
static void NumericWork(const char* name, bool wide, bool miss, bool budget, bool timing) {
  const unsigned records = miss ? 32768 : 16384;
  std::string input;
  if (miss) input = "X";
  else for (unsigned i = 0; i < records; ++i) input += "1234,5.25,label!";
  char path[64];
  FILE* stream = ScanFixture(input.data(), input.size(), path);
  if (!stream) return;
  for (unsigned round = 0; round < (timing ? 8u : 1u); ++round) {
    check("numeric scan work repositions real input", adapter_fseek(stream, 0, SEEK_SET) == 0);
    const ScanWorkStart start;
    uint64_t consumed = 0;
    for (unsigned i = 0; i < records; ++i) {
      int value = 61, count = -1;
      if (miss) {
        check("failed numeric scan leaves the destination untouched", ScanInteger(stream, wide, &value) == 0 && value == 61);
      } else {
        double number = 0;
        char label[6] = {};
        wchar_t wlabel[6] = {};
        const int result = wide ? adapter_fwscanf(stream, L"%d,%lf,%5l[a-z]!%n", &value, &number, wlabel, &count)
                                : adapter_fscanf(stream, "%d,%lf,%5[a-z]!%n", &value, &number, label, &count);
        const bool exact = result == 3 && value == 1234 && number == 5.25 && count == 16 &&
            (wide ? std::wcscmp(wlabel, L"label") == 0 : std::strcmp(label, "label") == 0);
        check("mixed numeric scan preserves values and assignment count", exact);
        consumed += exact ? count : 0;
      }
    }
    ReportScanWork(name, round, records, consumed, start, budget);
    if (miss) CheckScopeRestored(stream, wide, 'X');
  }
  ScanClose(stream, path);
}

int main(int argc, char** argv) {
  const bool timing = argc > 1 && std::strcmp(argv[1], "timing") == 0;
  const bool budget = argc <= 1 || std::strcmp(argv[1], "gate") == 0;
  // Each compiled negative variant runs its named contract in isolation. An
  // unrelated later crash must never be mistaken for the expected assertion.
  if (argc == 3) {
    if (std::strcmp(argv[2], "restore") == 0) TestNormalScopes();
    else if (std::strcmp(argv[2], "nested") == 0) TestNestedScans(budget);
    else if (std::strcmp(argv[2], "other") == 0) TestOtherStreamCallbacks();
    else if (std::strcmp(argv[2], "retired") == 0) TestReregisteredOwner();
    else if (std::strcmp(argv[2], "cancel") == 0) TestCanceledScans();
    else return 2;
    return failures ? 1 : 0;
  }
  TestNormalScopes();
  if (failures) return 1;
  test_stdio_scan_contracts();
  test_stdio_scan_wide_contracts();
  if (failures) return 1;
  TestNestedScans(budget);
  if (failures) return 1;
  TestOtherStreamCallbacks();
  if (failures) return 1;
  TestReregisteredOwner();
  if (failures) return 1;
  TestExceptionalScopes();
  if (failures) return 1;
  TestCanceledScans();
  if (failures) return 1;
  TestLineFlush();
  if (failures) return 1;
  TokenWork("narrow-long", 65536, 64, false, budget, timing);
  TokenWork("wide-long", 65536, 64, true, budget, timing);
  TokenWork("narrow-short", 16, 4096, false, budget, timing);
  TokenWork("wide-short", 16, 4096, true, budget, timing);
  NumericWork("narrow-numeric", false, false, budget, timing);
  NumericWork("wide-numeric", true, false, budget, timing);
  NumericWork("narrow-mismatch", false, true, budget, timing);
  NumericWork("wide-mismatch", true, true, budget, timing);
  char summary[128];
  const int count = std::snprintf(summary, sizeof(summary), "stdio scanning: %d checks, %d failures\n", checks, failures);
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(summary) || write(STDOUT_FILENO, summary, count) != count) return 2;
  return failures ? 1 : 0;
}
