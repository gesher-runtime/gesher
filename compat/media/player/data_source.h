// Gesher media: where media bytes come from (docs/media.md): a file
// descriptor (MediaPlayer.setDataSource(FileDescriptor, offset, length):
// files, assets and raw resources, content URIs), a path, or an http(s) URL
// (WinHTTP, with range requests and the Windows proxy and certificate
// store).
#pragma once

#include <sys/types.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace gesher::media {

class DataSource {
 public:
  virtual ~DataSource() = default;
  // Reads up to `size` bytes at `offset`; 0 at the end, negative on error.
  virtual ssize_t ReadAt(int64_t offset, void* data, size_t size) = 0;
  // The length, or -1 when it is not known.
  virtual int64_t Size() = 0;
  virtual std::string Uri() const { return std::string(); }
  // Network sources: reads may be slow (MediaPlayer reports buffering).
  virtual bool IsNetwork() const { return false; }
  // Stops pending and later reads (from another thread).
  virtual void Cancel() {}
};

// A file descriptor of gesher-base, duplicated: [offset, offset + length);
// length < 0 to the end of the file.
std::unique_ptr<DataSource> OpenFdSource(int fd, int64_t offset, int64_t length, std::string* error);
// A path (or a file:// URI's path).
std::unique_ptr<DataSource> OpenFileSource(const std::string& path, std::string* error);
// An http or https URL, with extra request headers.
std::unique_ptr<DataSource> OpenHttpSource(const std::string& url,
                                           const std::map<std::string, std::string>& headers,
                                           std::string* error);
// Bytes in memory.
std::unique_ptr<DataSource> OpenMemorySource(std::string bytes);

}  // namespace gesher::media
