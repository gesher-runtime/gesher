#ifndef GESHER_ROOTED_STORAGE_H
#define GESHER_ROOTED_STORAGE_H

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace gesher::rooted_storage {

enum class Area { kCredential, kDevice };
enum class Access { kRead, kWrite, kReadWrite, kMetadata, kMetadataWrite };

// Borrowed, separately admitted directory handles. The owner holds each identity
// throughout the broker session. A guest only selects an area, never a HANDLE.
struct Roots {
  HANDLE credential;
  HANDLE device;
};

struct OpenRequest {
  Access access = Access::kRead;
  bool create = false;
  bool exclusive = false;
  bool truncate = false;
  bool directory = false;
  bool read_only_create = false;
  bool write_through = false;
  DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  bool delete_on_close = false;
  bool overlapped = false;
};

struct DirectoryEntry {
  std::wstring name;
  DWORD attributes;
  ULONGLONG file_id;
};

// Paths use encoded on-disk components separated by '/', with empty denoting
// the exact root. No function changes root ACLs. Return 0 or a positive errno.
// A successful Open returns an owned, non-inheritable kernel file handle.
int Open(const Roots& roots, Area area, std::wstring_view path, OpenRequest request, HANDLE* result,
         bool* created = nullptr);
int Stat(const Roots& roots, Area area, std::wstring_view path, BY_HANDLE_FILE_INFORMATION* result);
int MakeDirectory(const Roots& roots, Area area, std::wstring_view path);
int Rename(const Roots& roots, Area source_area, std::wstring_view source,
           Area target_area, std::wstring_view target, bool replace);
int Remove(const Roots& roots, Area area, std::wstring_view path, bool directory);

// Bounded batch from a directory file object returned by Open. When a batch is
// empty, the kernel cursor has reached the end. Names remain on-disk UTF-16.
int ReadDirectory(HANDLE directory, bool restart, std::vector<DirectoryEntry>* entries);

}  // namespace gesher::rooted_storage

#endif
