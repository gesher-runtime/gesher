// The complete production file source, linked separately, on Linux and Windows.
#include "compat/media/player/data_source.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <unistd.h>

namespace {
int checks = 0, failures = 0;
bool Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL media source: %s\n", message); }
  return ok;
}
}

int main() {
  char path[] = "gesher-media-source-XXXXXX";
  int fd = mkstemp(path);
  if (!Check(fd >= 0, "create fixture")) return 1;
  if (!Check(write(fd, "0123456789", 10) == 10, "write fixture")) return 1;
  Check(lseek(fd, 2, SEEK_SET) == 2, "set caller cursor");
  std::string error;
  using gesher::media::OpenFdSource;
  {
    auto huge = OpenFdSource(fd, 4, INT64_MAX, &error);
    Check(huge != nullptr, "large declared range opens");
    if (huge != nullptr && Check(huge->Size() == 6, "large range clamps to six available bytes")) {
      char bytes[16] = {};
      Check(huge->ReadAt(0, bytes, std::numeric_limits<size_t>::max()) == 6,
            "unsigned request clamps before narrowing");
      Check(std::memcmp(bytes, "456789", 6) == 0 && bytes[6] == 0, "bounded slice payload");
      Check(huge->ReadAt(6, bytes, sizeof(bytes)) == 0, "slice EOF");
      Check(huge->ReadAt(INT64_MAX, bytes, sizeof(bytes)) == 0, "far beyond slice EOF");
      Check(huge->ReadAt(-1, bytes, sizeof(bytes)) == -EINVAL, "negative relative offset");
      Check(huge->ReadAt(0, nullptr, 0) == 0, "zero-byte read");
      Check(lseek(fd, 0, SEEK_CUR) == 2, "positioned read leaves caller cursor");
    }
    auto slice = OpenFdSource(fd, 2, 3, &error);
    Check(slice != nullptr && slice->Size() == 3, "ordinary bounded slice");
    if (slice != nullptr) {
      char bytes[16] = {};
      Check(slice->ReadAt(0, bytes, sizeof(bytes)) == 3 && std::memcmp(bytes, "234", 3) == 0,
            "ordinary slice excludes trailing file bytes");
      Check(slice->ReadAt(1, bytes, sizeof(bytes)) == 2 && std::memcmp(bytes, "34", 2) == 0,
            "relative reads honor the source base");
    }
    for (int64_t length : {int64_t{-1}, std::numeric_limits<int64_t>::min()}) {
      auto rest = OpenFdSource(fd, 4, length, &error);
      Check(rest != nullptr && rest->Size() == 6, "internal negative length means to EOF");
    }
    auto empty = OpenFdSource(fd, 4, 0, &error);
    Check(empty != nullptr && empty->Size() == 0, "explicit zero length remains empty");
    auto beyond = OpenFdSource(fd, INT64_MAX, INT64_MAX, &error);
    Check(beyond != nullptr && beyond->Size() == 0, "base beyond EOF normalizes empty");
    auto negative = OpenFdSource(fd, -4, 3, &error);
    char bytes[16] = {};
    Check(negative != nullptr && negative->ReadAt(0, bytes, sizeof(bytes)) == 3
              && std::memcmp(bytes, "012", 3) == 0, "negative base normalizes to zero");
    auto owned = OpenFdSource(fd, 3, 2, &error);
    Check(close(fd) == 0, "close caller descriptor");
    fd = -1;
    Check(owned != nullptr && owned->ReadAt(0, bytes, sizeof(bytes)) == 2
              && std::memcmp(bytes, "34", 2) == 0, "source owns its duplicate");
    auto file = gesher::media::OpenFileSource(path, &error);
    auto uri = gesher::media::OpenFileSource(std::string("file://") + path, &error);
    Check(file != nullptr && file->Size() == 10, "path source keeps EOF sentinel");
    Check(uri != nullptr && uri->ReadAt(8, bytes, sizeof(bytes)) == 2
              && std::memcmp(bytes, "89", 2) == 0, "file URI source");
  }
  Check(unlink(path) == 0, "all source duplicates close");
  auto invalid = OpenFdSource(-1, 0, 1, &error);
  Check(invalid == nullptr && error.rfind("dup: ", 0) == 0, "dup failure is reported");
  auto missing = gesher::media::OpenFileSource(path, &error);
  Check(missing == nullptr && !error.empty(), "missing source reports failure");
  auto memory = gesher::media::OpenMemorySource("abc");
  char bytes[8] = {};
  Check(memory->Size() == 3 && memory->ReadAt(1, bytes, sizeof(bytes)) == 2
            && std::memcmp(bytes, "bc", 2) == 0, "memory source regression");
  Check(memory->ReadAt(-1, bytes, 1) == -EINVAL && memory->ReadAt(3, bytes, 1) == 0,
        "memory source boundaries");
  std::printf("media source contracts: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
