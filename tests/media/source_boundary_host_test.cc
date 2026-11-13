// Entire data_source.cc with only descriptor syscalls replaced. This isolates
// unknown-length/OS-error decisions without allocating or reading huge buffers.
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include "compat/media/player/data_source.h"

namespace {
int checks = 0, failures = 0, closes = 0, reads = 0;
size_t observed_size = 0;
off_t observed_offset = 0;
ssize_t read_result = 1;
bool duplicate_fails = false;
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL source boundary: %s\n", reason); }
  return ok;
}
int SourceDup(int) { if (duplicate_fails) { errno = EMFILE; return -1; } return 42; }
int SourceClose(int fd) { Check(fd == 42, "close only duplicate"); ++closes; return 0; }
int SourceFstat(int, struct stat*) { errno = EIO; return -1; }
int SourceOpen(const char*, int) { errno = ENOENT; return -1; }
ssize_t SourcePread(int fd, void*, size_t count, off_t at) {
  Check(fd == 42, "pread duplicate");
  ++reads; observed_size = count; observed_offset = at;
  if (read_result < 0) errno = EIO;
  return read_result;
}
}
#define dup SourceDup
#define close SourceClose
#define fstat SourceFstat
#define open SourceOpen
#define pread SourcePread
#include GESHER_DATA_SOURCE
#undef dup
#undef close
#undef fstat
#undef open
#undef pread

int main() {
  using gesher::media::OpenFdSource;
  std::string error;
  {
    auto unknown = OpenFdSource(7, 4, -1, &error);
    Check(unknown != nullptr && unknown->Size() == -1, "failed fstat preserves unknown length");
    char byte = 0;
    Check(unknown->ReadAt(3, &byte, 1) == 1 && observed_offset == 7,
          "unknown source still uses physical base");
    int before = reads;
    Check(unknown->ReadAt(INT64_MAX, &byte, 1) == -EOVERFLOW && reads == before,
          "unrepresentable physical offset rejected before OS call");
    Check(unknown->ReadAt(0, &byte, std::numeric_limits<size_t>::max()) == 1
              && observed_size == static_cast<size_t>(std::numeric_limits<ssize_t>::max()),
          "OS count remains representable without a huge memory access");
    read_result = -1;
    Check(unknown->ReadAt(0, &byte, 1) == -EIO, "pread errno propagates");
    read_result = 0;
    Check(unknown->ReadAt(0, &byte, 1) == 0, "pread EOF propagates");
  }
  Check(closes == 1, "unknown source closes once");
  {
    auto bounded = OpenFdSource(7, INT64_MAX - 1, INT64_MAX, &error);
    Check(bounded != nullptr && bounded->Size() == 1, "unknown file range bounds physical end");
    int before = reads;
    char byte = 0;
    Check(bounded->ReadAt(1, &byte, 1) == 0 && reads == before, "normalized range ends before pread");
  }
  duplicate_fails = true;
  Check(OpenFdSource(7, 0, -1, &error) == nullptr && closes == 2,
        "failed duplicate creates no owner");
  std::printf("media source boundary contracts: %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
