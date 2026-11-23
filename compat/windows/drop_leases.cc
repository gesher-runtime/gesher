/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include "drop_leases.h"

#include <cerrno>
#include <map>
#include <utility>

#include <gesher/rooted_storage.h>
#include "../binder/peer_registry.h"

namespace gesher::windows {
namespace {

constexpr size_t kMaxLeases = 16;
constexpr size_t kMaxRootsPerLease = 128;
constexpr size_t kMaxRelativeUnits = 32767;
constexpr DWORD kReadShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

int OpenError(DWORD error) {
  switch (error) {
    case ERROR_ACCESS_DENIED: return EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return ENOENT;
    case ERROR_SHARING_VIOLATION: return EBUSY;
    case ERROR_TOO_MANY_OPEN_FILES: return EMFILE;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER: return EINVAL;
    default: return EIO;
  }
}

std::wstring FileName(std::wstring_view path) {
  while (!path.empty() && path.back() == L'\\') path.remove_suffix(1);
  const auto separator = path.find_last_of(L'\\');
  if (separator == std::wstring_view::npos || separator + 1 == path.size()) return {};
  return std::wstring(path.substr(separator + 1));
}

}  // namespace

class DropLeases::State {
 public:
  struct Root {
    binder_peer::OwnedHandle handle;
    bool directory;
  };
  std::map<uint64_t, std::vector<Root>> leases;
  uint64_t next = 1;
};

DropLeases::DropLeases() : state_(std::make_unique<State>()) {}
DropLeases::~DropLeases() = default;

int DropLeases::Retain(const OleDropSource& source, const std::vector<std::wstring>& paths,
                       Offer* offer) {
  if (!offer || paths.empty()) return EINVAL;
  if (paths.size() > kMaxRootsPerLease) return E2BIG;
  if (state_->leases.size() >= kMaxLeases || !state_->next) return EMFILE;

  Offer pending;
  pending.items.reserve(paths.size());
  std::vector<State::Root> roots;
  roots.reserve(paths.size());
  for (const auto& path : paths) {
    std::wstring name = FileName(path);
    if (name.empty() || name == L"." || name == L"..") return EINVAL;
    const HANDLE file = source.Open(path.c_str());
    if (file == INVALID_HANDLE_VALUE) return OpenError(GetLastError());
    binder_peer::OwnedHandle owned(file);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
      return OpenError(GetLastError());
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return ELOOP;
    const bool directory = (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    pending.items.push_back({std::move(name), directory});
    roots.push_back({std::move(owned), directory});
  }
  pending.lease = state_->next++;
  state_->leases.emplace(pending.lease, std::move(roots));
  *offer = std::move(pending);
  return 0;
}

void DropLeases::Release(uint64_t lease) {
  state_->leases.erase(lease);
}

int DropLeases::Open(uint64_t lease, uint32_t item, std::wstring_view relative,
                     bool directory, bool metadata, HANDLE* opened) const {
  if (!opened) return EINVAL;
  *opened = INVALID_HANDLE_VALUE;
  if (relative.size() > kMaxRelativeUnits) return ENAMETOOLONG;
  const auto found = state_->leases.find(lease);
  if (found == state_->leases.end() || item >= found->second.size()) return ENOENT;
  const auto& root = found->second[item];
  if (root.directory) {
    rooted_storage::OpenRequest request;
    request.access = metadata ? rooted_storage::Access::kMetadata : rooted_storage::Access::kRead;
    request.directory = directory;
    const rooted_storage::Roots handles{root.handle.get(), nullptr};
    return rooted_storage::Open(handles, rooted_storage::Area::kCredential, relative, request, opened);
  }

  if (directory || !relative.empty()) return ENOTDIR;
  constexpr DWORD kMetadata = FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
  const HANDLE reopened = ReOpenFile(root.handle.get(), metadata ? kMetadata : GENERIC_READ,
      kReadShare, FILE_FLAG_BACKUP_SEMANTICS);
  if (reopened == INVALID_HANDLE_VALUE) return OpenError(GetLastError());
  *opened = reopened;
  return 0;
}

}  // namespace gesher::windows
