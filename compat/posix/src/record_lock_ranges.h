// Internal advisory record-lock interval semantics. Platform coordination and
// descriptor/process lifetime are separate; this header publishes no state.
#pragma once
#include <errno.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <tuple>
#include <vector>

namespace gesher::record_locks {

constexpr uint64_t kThroughEof = UINT64_C(1) << 63;
enum class Kind : uint8_t { kRead, kWrite, kUnlock };
enum class Ownership : uint8_t { kProcess, kDescription };

struct File {
  uint64_t volume, id_low, id_high;
  auto Tuple() const { return std::tie(volume, id_low, id_high); }
  bool operator==(const File& other) const { return Tuple() == other.Tuple(); }
};
struct Owner {
  Ownership kind;
  // Process locks: PID/creation time; OFD locks: stable random description
  // token propagated across every supported duplicate/descriptor transfer.
  uint64_t low, high;
  auto Tuple() const { return std::tie(kind, low, high); }
  bool operator==(const Owner& other) const { return Tuple() == other.Tuple(); }
};
struct Range { uint64_t start, end; };  // Half-open; kThroughEof includes future EOF.
struct Record { File file; Owner owner; Range range; Kind kind; int32_t pid; };
constexpr size_t kRecordCapacity = 512;
struct Snapshot {
  std::array<Record, kRecordCapacity> records{};
  size_t count = 0;
  auto begin() const { return records.begin(); }
  auto end() const { return records.begin() + count; }
};

inline bool Overlap(const Range& left, const Range& right) {
  return left.start < right.end && right.start < left.end;
}

inline int Normalize(int whence, int64_t start, int64_t length,
                     int64_t current, int64_t file_size, Range* result) {
  int64_t base;
  switch (whence) {
    case 0: base = 0; break;
    case 1: base = current; break;
    case 2: base = file_size; break;
    default: return EINVAL;
  }
  int64_t position;
  if (__builtin_add_overflow(base, start, &position)) return EOVERFLOW;
  if (position < 0) return EINVAL;
  if (length > 0) {
    int64_t last;
    if (__builtin_add_overflow(position, length - 1, &last)) return EOVERFLOW;
    *result = {static_cast<uint64_t>(position), static_cast<uint64_t>(last) + 1};
  } else if (length == 0) {
    *result = {static_cast<uint64_t>(position), kThroughEof};
  } else {
    int64_t first;
    if (__builtin_add_overflow(position, length, &first)) return EOVERFLOW;
    if (first < 0) return EINVAL;
    *result = {static_cast<uint64_t>(first), static_cast<uint64_t>(position)};
  }
  return 0;
}

inline bool Conflicts(const Record& held, const Record& requested) {
  return held.file == requested.file && !(held.owner == requested.owner) &&
         Overlap(held.range, requested.range) &&
         (held.kind == Kind::kWrite || requested.kind == Kind::kWrite);
}

// Callers validate normalized nonempty ranges/kinds before constructing a
// snapshot. Held records never contain kUnlock. This layer allocates no
// interval storage, so capacity failure is explicit even without exceptions.
inline const Record* FindConflict(const Snapshot& records, const Record& requested) {
  for (const Record& held : records) if (Conflicts(held, requested)) return &held;
  return nullptr;
}

// Make a complete candidate off shared state. Neither conflict nor capacity
// failure changes *records. The platform layer must likewise publish its
// serialized snapshot only after the entire candidate is checked/copied.
inline int Replace(Snapshot* records, const Record& requested, size_t capacity) {
  if (records->count > kRecordCapacity || requested.range.start >= requested.range.end ||
      requested.range.end > kThroughEof || static_cast<unsigned>(requested.kind) > 2) return EINVAL;
  for (const Record& held : *records) {
    if (held.range.start >= held.range.end || held.range.end > kThroughEof || held.kind == Kind::kUnlock ||
        static_cast<unsigned>(held.kind) > 2) return EINVAL;
  }
  if (requested.kind != Kind::kUnlock && FindConflict(*records, requested)) return EAGAIN;
  // Replacing one interval can add at most two edge fragments plus the new
  // interval; the overlapped old interval already occupied one slot.
  std::array<Record, kRecordCapacity + 2> candidate{};
  size_t count = 0;
  auto append = [&](const Record& record) {
    if (count == candidate.size()) return false;
    candidate[count++] = record;
    return true;
  };
  for (const Record& held : *records) {
    if (!(held.file == requested.file) || !(held.owner == requested.owner) ||
        !Overlap(held.range, requested.range)) {
      if (!append(held)) return ENOLCK;
      continue;
    }
    if (held.range.start < requested.range.start) {
      Record left = held;
      left.range.end = requested.range.start;
      if (!append(left)) return ENOLCK;
    }
    if (held.range.end > requested.range.end) {
      Record right = held;
      right.range.start = requested.range.end;
      if (!append(right)) return ENOLCK;
    }
  }
  if (requested.kind != Kind::kUnlock && !append(requested)) return ENOLCK;
  std::sort(candidate.begin(), candidate.begin() + count, [](const Record& left, const Record& right) {
    return std::make_tuple(left.file.Tuple(), left.owner.Tuple(), left.kind, left.range.start) <
           std::make_tuple(right.file.Tuple(), right.owner.Tuple(), right.kind, right.range.start);
  });
  size_t merged = 0;
  for (size_t index = 0; index < count; ++index) {
    Record record = candidate[index];
    if (merged != 0) {
      Record& previous = candidate[merged - 1];
      if (previous.file == record.file && previous.owner == record.owner && previous.kind == record.kind &&
          previous.range.end >= record.range.start) {
        previous.range.end = std::max(previous.range.end, record.range.end);
        continue;
      }
    }
    candidate[merged++] = record;
  }
  if (merged > capacity || merged > kRecordCapacity) return ENOLCK;
  std::copy_n(candidate.begin(), merged, records->records.begin());
  records->count = merged;
  return 0;
}

inline void RemoveOwner(Snapshot* records, const Owner& owner, const File* file = nullptr) {
  size_t retained = 0;
  for (size_t index = 0; index < records->count; ++index) {
    const Record& record = records->records[index];
    if (!(record.owner == owner && (file == nullptr || record.file == *file))) {
      records->records[retained++] = record;
    }
  }
  records->count = retained;
}

struct Wait {
  // Identity and liveness belong to the platform's PID+creation and thread+
  // request-token records. This is only the current dependency edge.
  Owner waiting;
  std::vector<Owner> blocking;
};

inline bool Reaches(const std::vector<Wait>& waits, const Owner& from, const Owner& target,
                    std::vector<Owner>* visited) {
  if (from == target) return true;
  if (std::find(visited->begin(), visited->end(), from) != visited->end()) return false;
  visited->push_back(from);
  for (const Wait& wait : waits) if (wait.waiting == from) {
    for (const Owner& next : wait.blocking) if (Reaches(waits, next, target, visited)) return true;
  }
  return false;
}

// Unwired design helper only: mixed POSIX/OFD cycle behavior and allocation-
// free live-request graph storage still require their own actual oracle and
// implementation. Pure interval CI must not qualify this wait graph.
inline bool PosixDeadlock(const std::vector<Wait>& waits, const Owner& requester,
                          const std::vector<Owner>& blockers) {
  if (requester.kind != Ownership::kProcess) return false;
  for (const Owner& blocker : blockers) {
    std::vector<Owner> visited;
    if (Reaches(waits, blocker, requester, &visited)) return true;
  }
  return false;
}

}  // namespace gesher::record_locks
