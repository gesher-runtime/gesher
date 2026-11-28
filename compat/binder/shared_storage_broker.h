// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <gesher/rooted_storage.h>
#include <gesher/shared_storage_policy.h>

#include <cerrno>
#include "peer_registry.h"

namespace gesher::shared_storage_broker {
namespace wire = rooted_storage_wire;
namespace rooted = rooted_storage;
namespace policy = shared_storage_policy;
using binder_peer::OwnedHandle;
using binder_peer::Peer;

inline bool ValidPackage(std::string_view package) {
  if (package.empty()) return false;
  bool component = false;
  for (const char c : package) {
    if (c == '.') {
      if (!component) return false;
      component = false;
    } else {
      if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
          !(c >= '0' && c <= '9') && c != '_') return false;
      component = true;
    }
  }
  return component;
}

inline std::string InstallationString(const std::array<uint8_t, 16>& id) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
    const uint8_t byte = id[(i & 8) + 7 - (i & 7)];
    result += kHex[byte >> 4];
    result += kHex[byte & 15];
  }
  return result;
}

inline bool SameName(std::wstring_view a, std::wstring_view b) {
  return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
      static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

enum class Namespace { kShared, kBoundary, kPrivate, kOwn, kDenied };

inline Namespace Classify(std::wstring_view path, std::wstring_view package) {
  if (path.empty() || SameName(path, L"Android")) return Namespace::kBoundary;
  size_t slash = path.find(L'/');
  if (slash == std::wstring_view::npos || !SameName(path.substr(0, slash), L"Android")) {
    return Namespace::kShared;
  }
  path.remove_prefix(slash + 1);
  slash = path.find(L'/');
  const auto area = path.substr(0, slash);
  const bool private_area = SameName(area, L"data") || SameName(area, L"obb");
  if (!private_area && !SameName(area, L"media")) return Namespace::kShared;
  if (slash == std::wstring_view::npos) return private_area ? Namespace::kPrivate : Namespace::kShared;
  path.remove_prefix(slash + 1);
  const auto owner = path.substr(0, path.find(L'/'));
  if (owner == package) return Namespace::kOwn;
  return private_area ? Namespace::kDenied : Namespace::kShared;
}

inline bool CanWrite(Namespace area, policy::StorageMode mode) {
  return area == Namespace::kOwn || (area == Namespace::kShared &&
      (mode == policy::StorageMode::kManager || mode == policy::StorageMode::kLegacyRw));
}

inline bool Valid(const wire::Request& request) {
  if (request.magic != wire::kMagic || request.version != wire::kVersion ||
      request.source_area != wire::kInstalledMedia || request.path_units > 4096 ||
      request.target_units > 4096) return false;
  if (request.operation == wire::Op::kRename) {
    return request.target_area == wire::kInstalledMedia && (request.flags & ~wire::kReplace) == 0;
  }
  if (request.target_units != 0 || request.target_area != wire::kCredential) return false;
  switch (request.operation) {
    case wire::Op::kOpen:
      return (request.flags & ~wire::kOpenFlags) == 0 &&
          (request.flags & wire::kAccessMask) <= wire::kMetadataWrite &&
          ((request.flags & wire::kShareMask) == 0 || (request.flags & wire::kShareOverride) != 0);
    case wire::Op::kMakeDirectory: return request.flags == 0;
    case wire::Op::kRemove: return (request.flags & ~wire::kDirectory) == 0;
    default: return false;
  }
}

inline int OpenUnder(const rooted::Roots& roots, std::wstring_view path,
                     rooted::OpenRequest request, OwnedHandle* result, bool* created = nullptr) {
  HANDLE opened = INVALID_HANDLE_VALUE;
  const int error = rooted::Open(roots, rooted::Area::kCredential, path, request, &opened, created);
  if (!error) result->reset(opened);
  return error;
}

inline bool Load(const rooted::Roots& device, const Peer& guest, std::wstring_view package,
                 policy::StorageMode* mode, int* media_mask) {
  OwnedHandle snapshot;
  rooted::OpenRequest request;
  if (OpenUnder(device, std::wstring(L"system/storage-policy/") + std::wstring(package),
                request, &snapshot)) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(snapshot.get(), &size) || size.QuadPart <= 0 || size.QuadPart > 65536) return false;
  std::string content(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  return ReadFile(snapshot.get(), content.data(), static_cast<DWORD>(content.size()), &read, nullptr) &&
      read == content.size() && policy::ParsePolicyContent(content, guest.package, std::to_string(guest.uid),
          mode, media_mask, InstallationString(guest.installation));
}

inline bool MediaPermitted(HANDLE handle, int mask) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) return false;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
  wchar_t path[4097];
  const DWORD used = GetFinalPathNameByHandleW(handle, path, std::size(path),
                                               FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
  if (used == 0 || used >= std::size(path)) return false;
  const std::wstring_view name(path, used);
  const size_t slash = name.find_last_of(L'\\');
  return slash != std::wstring_view::npos && policy::HasMediaExtension(name.substr(slash + 1), mask);
}

inline int Dispatch(const wchar_t* data_root, const Peer& guest, const wire::Request& request,
                    const std::wstring& path, const std::wstring& target, OwnedHandle* opened,
                    bool* created) {
  if (!data_root || !*data_root || !ValidPackage(guest.package)) return EACCES;
  OwnedHandle device_root(CreateFileW(data_root, FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (device_root.get() == INVALID_HANDLE_VALUE) return EACCES;
  const rooted::Roots device{device_root.get(), INVALID_HANDLE_VALUE};
  const std::wstring package(guest.package.begin(), guest.package.end());
  policy::StorageMode mode = policy::StorageMode::kScoped;
  int media_mask = 0;
  if (!Load(device, guest, package, &mode, &media_mask)) return EACCES;

  const Namespace area = Classify(path, package);
  const uint32_t access = request.flags & wire::kAccessMask;
  const bool metadata = request.operation == wire::Op::kOpen && access == wire::kMetadata &&
      !(request.flags & (wire::kCreate | wire::kTruncate | wire::kDeleteOnClose));
  const bool writes = request.operation != wire::Op::kOpen ||
      access == wire::kWrite || access == wire::kReadWrite || access == wire::kMetadataWrite ||
      (request.flags & (wire::kCreate | wire::kTruncate | wire::kDeleteOnClose));
  if (area == Namespace::kDenied || (area == Namespace::kPrivate && !metadata)) return EACCES;
  if (writes && !CanWrite(area, mode)) return EACCES;
  if (request.operation == wire::Op::kRename && !CanWrite(Classify(target, package), mode)) return EACCES;
  if (mode == policy::StorageMode::kScoped && area != Namespace::kOwn &&
      !(metadata && (area == Namespace::kBoundary || area == Namespace::kPrivate))) return EACCES;

  OwnedHandle media;
  rooted::OpenRequest directory;
  directory.access = rooted::Access::kMetadata;
  directory.directory = true;
  directory.share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
  int error = OpenUnder(device, L"media/0", directory, &media);
  if (error) return error;
  const rooted::Roots roots{media.get(), INVALID_HANDLE_VALUE};
  constexpr auto root = rooted::Area::kCredential;
  switch (request.operation) {
    case wire::Op::kOpen: {
      rooted::OpenRequest flags;
      switch (access) {
        case wire::kRead: flags.access = rooted::Access::kRead; break;
        case wire::kWrite: flags.access = rooted::Access::kWrite; break;
        case wire::kReadWrite: flags.access = rooted::Access::kReadWrite; break;
        case wire::kMetadata: flags.access = rooted::Access::kMetadata; break;
        case wire::kMetadataWrite: flags.access = rooted::Access::kMetadataWrite; break;
        default: return EINVAL;
      }
      flags.create = request.flags & wire::kCreate;
      flags.exclusive = request.flags & wire::kExclusive;
      flags.truncate = request.flags & wire::kTruncate;
      flags.directory = request.flags & wire::kDirectory;
      flags.read_only_create = request.flags & wire::kReadOnlyCreate;
      flags.write_through = request.flags & wire::kWriteThrough;
      flags.delete_on_close = request.flags & wire::kDeleteOnClose;
      flags.overlapped = request.flags & wire::kOverlapped;
      if (request.flags & wire::kShareOverride) flags.share_mode = (request.flags & wire::kShareMask) >> wire::kShareShift;
      error = OpenUnder(roots, path, flags, opened, created);
      if (!error && mode == policy::StorageMode::kMediaRo && area == Namespace::kShared &&
          !MediaPermitted(opened->get(), media_mask)) return EACCES;
      return error;
    }
    case wire::Op::kMakeDirectory: return rooted::MakeDirectory(roots, root, path);
    case wire::Op::kRemove: return rooted::Remove(roots, root, path, request.flags & wire::kDirectory);
    case wire::Op::kRename: return rooted::Rename(roots, root, path, root, target, request.flags & wire::kReplace);
    default: return EINVAL;
  }
}
}  // namespace gesher::shared_storage_broker
