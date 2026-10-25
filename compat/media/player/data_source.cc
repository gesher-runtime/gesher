// Gesher media: file descriptor, path and memory sources (data_source.h).

#include "data_source.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace gesher::media {
namespace {

class FdSource : public DataSource {
 public:
  FdSource(int fd, int64_t offset, int64_t length) : fd_(fd), offset_(offset), length_(length) {}
  ~FdSource() override { close(fd_); }

  ssize_t ReadAt(int64_t offset, void* data, size_t size) override {
    if (offset < 0) return -EINVAL;
    if (length_ >= 0) {
      if (offset >= length_) return 0;
      size = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(size), length_ - offset));
    }
    ssize_t n = pread(fd_, data, size, static_cast<off_t>(offset_ + offset));
    return n < 0 ? -errno : n;
  }

  int64_t Size() override { return length_; }

 private:
  int fd_;
  int64_t offset_;
  int64_t length_;
};

class MemorySource : public DataSource {
 public:
  explicit MemorySource(std::string bytes) : bytes_(std::move(bytes)) {}
  ssize_t ReadAt(int64_t offset, void* data, size_t size) override {
    if (offset < 0) return -EINVAL;
    if (offset >= static_cast<int64_t>(bytes_.size())) return 0;
    size_t n = std::min(size, bytes_.size() - static_cast<size_t>(offset));
    memcpy(data, bytes_.data() + offset, n);
    return static_cast<ssize_t>(n);
  }
  int64_t Size() override { return static_cast<int64_t>(bytes_.size()); }

 private:
  std::string bytes_;
};

int64_t FdLength(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  return static_cast<int64_t>(st.st_size);
}

}  // namespace

std::unique_ptr<DataSource> OpenFdSource(int fd, int64_t offset, int64_t length, std::string* error) {
  int copy = dup(fd);
  if (copy < 0) {
    *error = std::string("dup: ") + strerror(errno);
    return nullptr;
  }
  int64_t total = FdLength(copy);
  if (offset < 0) offset = 0;
  if (total >= 0) {
    if (offset > total) offset = total;
    if (length < 0 || offset + length > total) length = total - offset;
  }
  return std::make_unique<FdSource>(copy, offset, length);
}

std::unique_ptr<DataSource> OpenFileSource(const std::string& path, std::string* error) {
  std::string p = path;
  if (p.rfind("file://", 0) == 0) p = p.substr(7);
  int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *error = p + ": " + strerror(errno);
    return nullptr;
  }
  auto s = OpenFdSource(fd, 0, -1, error);
  close(fd);
  return s;
}

std::unique_ptr<DataSource> OpenMemorySource(std::string bytes) {
  return std::make_unique<MemorySource>(std::move(bytes));
}

}  // namespace gesher::media
