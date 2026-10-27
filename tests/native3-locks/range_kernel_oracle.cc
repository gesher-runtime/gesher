// CI-only interval implementation versus actual Linux OFD locks. Independent
// open descriptions let one process exercise real conflicting kernel owners.
#include "../../compat/posix/src/record_lock_ranges.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace gesher::record_locks;

static bool CapacityFailurePreserves(Snapshot* snapshot, const Record& request, size_t capacity) {
  std::array<unsigned char, sizeof(Snapshot)> before{};
  std::memcpy(before.data(), snapshot, sizeof(Snapshot));
  return Replace(snapshot, request, capacity) == ENOLCK &&
    std::memcmp(before.data(), snapshot, sizeof(Snapshot)) == 0;
}

static bool CapacityContracts() {
  const Record whole{{1, 2, 3}, {Ownership::kDescription, 1, 2}, {0, 100}, Kind::kRead, -1};
  Snapshot empty;
  bool zero = CapacityFailurePreserves(&empty, whole, 0);
  Snapshot split;
  split.records[split.count++] = whole;
  Record middle = whole;
  middle.range = {40, 60};
  middle.kind = Kind::kUnlock;
  bool split_failure = CapacityFailurePreserves(&split, middle, 1);
  bool split_success = Replace(&split, middle, 2) == 0 && split.count == 2 &&
    split.records[0].range.start == 0 && split.records[0].range.end == 40 &&
    split.records[1].range.start == 60 && split.records[1].range.end == 100;
  Snapshot conversion;
  conversion.records[conversion.count++] = whole;
  middle.range = {20, 80};
  middle.kind = Kind::kWrite;
  bool conversion_failure = CapacityFailurePreserves(&conversion, middle, 2);
  bool conversion_success = Replace(&conversion, middle, 3) == 0 && conversion.count == 3;
  middle.kind = Kind::kRead;
  bool merged = Replace(&conversion, middle, 1) == 0 && conversion.count == 1 &&
    conversion.records[0].range.start == 0 && conversion.records[0].range.end == 100;
  Snapshot full;
  for (size_t index = 0; index < kRecordCapacity; ++index) {
    Record record = whole;
    record.range = {index * 4, index * 4 + 1};
    full.records[full.count++] = record;
  }
  Record separate = whole;
  separate.file.id_high = 999;
  bool full_failure = CapacityFailurePreserves(&full, separate, kRecordCapacity);
  std::printf("ACTUAL_RECORD_LOCK_CAPACITY zero=%d split_failure=%d split_success=%d conversion_failure=%d conversion_success=%d merge=%d full_failure=%d byte_unchanged_on_failure=%d\n",
    zero, split_failure, split_success, conversion_failure, conversion_success, merged, full_failure,
    zero && split_failure && conversion_failure && full_failure);
  return zero && split_failure && split_success && conversion_failure && conversion_success && merged && full_failure;
}

int main() {
  if (!CapacityContracts()) return 1;
  char path[] = "/tmp/gesher-lock-range-XXXXXX";
  int fd[2] = {mkstemp(path), -1};
  if (fd[0] < 0 || (fd[1] = open(path, O_RDWR)) < 0) return 2;
  unlink(path);
  if (ftruncate(fd[0], 4096) != 0) return 2;
  Snapshot records;
  std::mt19937 random(0x47657368);
  unsigned failures = 0;
  unsigned operations = 0, queries = 0;
  struct Boundary { int whence; int64_t start, length, current; };
  constexpr int64_t maximum = INT64_MAX;
  const Boundary boundaries[] = {
    {0, 0, 0, 0}, {0, maximum, 0, 0}, {0, maximum, 1, 0}, {0, maximum, 2, 0},
    {0, maximum - 1, 2, 0}, {0, maximum - 1, 3, 0}, {0, -1, 1, 0},
    {0, 0, -1, 0}, {0, maximum, INT64_MIN, 0}, {0, 0, INT64_MIN, 0},
    {1, maximum, 1, 1}, {2, maximum, 0, 0}, {3, 0, 1, 0},
    {1, -64, 32, 64}, {2, -16, 8, 0}, {0, 50, -5, 0}, {0, 50, 5, 0},
  };
  unsigned boundary_count = 0;
  for (const Boundary& boundary : boundaries) {
    if (lseek(fd[0], boundary.current, SEEK_SET) != boundary.current) return 2;
    Range expected_range{};
    int expected = Normalize(boundary.whence, boundary.start, boundary.length,
                             boundary.current, 4096, &expected_range);
    struct flock actual{};
    actual.l_type = F_WRLCK;
    actual.l_whence = static_cast<short>(boundary.whence);
    actual.l_start = boundary.start;
    actual.l_len = boundary.length;
    errno = 0;
    int code = fcntl(fd[0], F_OFD_SETLK, &actual);
    int error = code < 0 ? errno : 0;
    bool matched = error == expected;
    if (code == 0) {
      struct flock query{};
      query.l_type = F_WRLCK;
      query.l_start = static_cast<int64_t>(expected_range.start);
      query.l_len = 1;
      if (fcntl(fd[1], F_OFD_GETLK, &query) != 0) return 2;
      int64_t expected_length = expected_range.end == kThroughEof ? 0 :
        static_cast<int64_t>(expected_range.end - expected_range.start);
      matched = matched && query.l_type == F_WRLCK && query.l_whence == 0 && query.l_pid == -1 &&
        query.l_start == static_cast<int64_t>(expected_range.start) && query.l_len == expected_length;
      struct flock clear{};
      clear.l_type = F_UNLCK;
      if (fcntl(fd[0], F_OFD_SETLK, &clear) != 0) return 2;
    }
    std::printf("ACTUAL_RECORD_LOCK_KERNEL_BOUNDARY row=%u whence=%d start=%lld len=%lld current=%lld expected_errno=%d actual_errno=%d matched=%d\n",
      boundary_count, boundary.whence, static_cast<long long>(boundary.start),
      static_cast<long long>(boundary.length), static_cast<long long>(boundary.current), expected, error, matched);
    ++boundary_count;
    if (!matched) ++failures;
  }
  if (failures) return 1;
  for (unsigned index = 0; index < 10000; ++index) {
    unsigned who = random() & 1;
    int whence = random() % 3;
    int64_t current = random() % 256;
    int64_t start = static_cast<int64_t>(random() % 512) - 128;
    int64_t length = static_cast<int64_t>(random() % 128) - 64;
    Kind kind = static_cast<Kind>(random() % 3);
    if (lseek(fd[who], current, SEEK_SET) != current) return 2;
    Range normalized{};
    int expected = Normalize(whence, start, length, current, 4096, &normalized);
    Record request{{1, 2, 3}, {Ownership::kDescription, who, 0}, normalized, kind, -1};
    Snapshot candidate = records;
    if (expected == 0) expected = Replace(&candidate, request, 512);
    struct flock lock{};
    lock.l_type = kind == Kind::kRead ? F_RDLCK : kind == Kind::kWrite ? F_WRLCK : F_UNLCK;
    lock.l_whence = static_cast<short>(whence);
    lock.l_start = start;
    lock.l_len = length;
    errno = 0;
    int actual = fcntl(fd[who], F_OFD_SETLK, &lock);
    ++operations;
    int error = actual < 0 ? errno : 0;
    if (error != expected) {
      std::printf("FAIL range kernel step=%u owner=%u kind=%u whence=%d start=%lld len=%lld expected=%d actual=%d\n",
        index, who, static_cast<unsigned>(kind), whence, static_cast<long long>(start),
        static_cast<long long>(length), expected, error);
      ++failures;
      break;
    }
    if (actual == 0) records = candidate;
    for (unsigned query_owner = 0; query_owner < 2; ++query_owner) {
      Range query_range{static_cast<uint64_t>(random() % 5000), 0};
      query_range.end = query_range.start + 1;
      Record query{{1, 2, 3}, {Ownership::kDescription, query_owner, 0}, query_range, Kind::kWrite, -1};
      struct flock result{};
      result.l_type = F_WRLCK;
      result.l_start = static_cast<off_t>(query_range.start);
      result.l_len = 1;
      if (fcntl(fd[query_owner], F_OFD_GETLK, &result) != 0) return 2;
      ++queries;
      bool exists = FindConflict(records, query) != nullptr;
      bool matched = !exists && result.l_type == F_UNLCK;
      if (exists && result.l_type != F_UNLCK) {
        for (const Record& held : records) if (Conflicts(held, query)) {
          int64_t expected_length = held.range.end == kThroughEof ? 0 :
            static_cast<int64_t>(held.range.end - held.range.start);
          if (result.l_type == (held.kind == Kind::kRead ? F_RDLCK : F_WRLCK) &&
              result.l_whence == SEEK_SET && result.l_start == static_cast<int64_t>(held.range.start) &&
              result.l_len == expected_length && result.l_pid == -1) matched = true;
        }
      }
      if (!matched) {
        std::printf("FAIL range kernel query step=%u owner=%u exists=%d\n", index, query_owner, exists);
        ++failures;
        break;
      }
    }
    if (failures) break;
  }
  close(fd[0]);
  close(fd[1]);
  std::printf("ACTUAL_RECORD_LOCK_KERNEL_RANGE_SUMMARY boundaries=%u seed=0x47657368 operations=%u queries=%u failures=%u\n",
              boundary_count, operations, queries, failures);
  return failures == 0 ? 0 : 1;
}
