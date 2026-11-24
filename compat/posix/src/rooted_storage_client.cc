// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <gesher/binder_bootstrap.h>
#include <gesher/binder_endpoint.h>
#include <gesher/cohost_bootstrap.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "rooted_storage_client.h"
#include <gesher/drop_mount.h>

namespace gesher::storage_root {
namespace {
namespace wire = rooted_storage_wire;
static_assert(sizeof(wchar_t) == 2);

std::wstring AbsoluteDosPath(std::wstring path) {
  if (path.starts_with(L"\\\\?\\UNC\\")) path.replace(0, 8, L"\\\\");
  if (path.size() >= 7 && path.starts_with(L"\\\\?\\") && path[5] == L':') path.erase(0, 4);
  std::replace(path.begin(), path.end(), L'/', L'\\');
  const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (size == 0 || size > wire::kMaxPathUnits + 1) return {};
  std::wstring full(size, L'\0');
  const DWORD filled = GetFullPathNameW(path.c_str(), size, full.data(), nullptr);
  if (filled == 0 || filled >= size) return {};
  full.resize(filled);
  while (full.size() > 3 && full.back() == L'\\') full.pop_back();
  return full;
}

bool Within(std::wstring_view path, std::wstring_view root) {
  if (root.empty() || path.size() < root.size() ||
      (path.size() != root.size() && root.back() != L'\\' && path[root.size()] != L'\\')) return false;
  path = path.substr(0, root.size());
  if (root.size() >= 3 && root[1] == L':' && root[2] == L'\\') {
    if (CompareStringOrdinal(path.data(), 1, root.data(), 1, TRUE) != CSTR_EQUAL) return false;
    path.remove_prefix(1);
    root.remove_prefix(1);
  }
  // A case-sensitive Windows directory can have two real sibling names.
  // Match only an exact spelling authenticated by the launch metadata.
  return path == root;
}

int Call(wire::Op operation, const Selection& path, const Selection& target, uint32_t flags,
         wire::Reply* reply, wire::Filesystem* filesystem = nullptr) {
  if (path.error != 0) return path.error;
  if (target.error != 0) return target.error;
  if (!path.matched || (operation == wire::Op::kRename && !target.matched)) return EXDEV;
  const auto& state = cohost_bootstrap::Current();
  if (!state || !state.storage_host || state.storage_pipe.empty()) return EACCES;

  HANDLE pipe = INVALID_HANDLE_VALUE;
  const ULONGLONG deadline = GetTickCount64() + 5000;
  do {
    pipe = CreateFileW(state.storage_pipe.c_str(),
        FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    const DWORD error = GetLastError();
    if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return EACCES;
    Sleep(2);
  } while (GetTickCount64() < deadline);
  if (pipe == INVALID_HANDLE_VALUE) return ETIMEDOUT;
  struct ClosePipe {
    HANDLE file;
    ~ClosePipe() { CloseHandle(file); }
  } close_pipe{pipe};
  if (!state.VerifyPeer(pipe)) return EACCES;
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return EIO;

  wire::Request request;
  request.operation = operation;
  request.source_area = path.area;
  request.target_area = target.area;
  request.flags = flags;
  request.path_units = static_cast<uint32_t>(path.relative.size());
  request.target_units = static_cast<uint32_t>(target.relative.size());
  if (request.path_units > wire::kMaxPathUnits || request.target_units > wire::kMaxPathUnits) return ENAMETOOLONG;
  if (!binder_bootstrap::Transfer(pipe, &request, sizeof(request), true, deadline) ||
      !binder_bootstrap::Transfer(pipe, const_cast<wchar_t*>(path.relative.data()),
                                 request.path_units * sizeof(wchar_t), true, deadline) ||
      !binder_bootstrap::Transfer(pipe, const_cast<wchar_t*>(target.relative.data()),
                                 request.target_units * sizeof(wchar_t), true, deadline) ||
      !binder_bootstrap::Transfer(pipe, reply, sizeof(*reply), false, deadline)) return EIO;
  if (reply->magic != wire::kMagic || reply->version != wire::kVersion || reply->status < 0 ||
      reply->status > 4095 || (reply->flags & ~wire::kCreated) != 0) return EPROTO;
  if (reply->status != 0 || !wire::OpensHandle(operation)) {
    if (reply->handle != 0 || reply->flags != 0) return EPROTO;
  } else if (reply->handle == 0 || reply->handle == UINT64_MAX || reply->handle > UINTPTR_MAX ||
             (operation != wire::Op::kOpen && reply->flags != 0)) {
    return EPROTO;
  }
  if (!state.VerifyPeer(pipe)) return EACCES;
  if (reply->status == 0 && operation == wire::Op::kStatAndroidData &&
      (!binder_bootstrap::Transfer(pipe, filesystem, sizeof(*filesystem), false, deadline) ||
       !state.VerifyPeer(pipe))) return EIO;
  if (reply->handle != 0) {
    wire::OpenAck ack, committed;
    // The host starts its five-second delivery window before writing the
    // offer. Keep reading for that whole window once we accept an offer.
    const ULONGLONG handoff_deadline = GetTickCount64() + 5000;
    if (!binder_bootstrap::Transfer(pipe, &ack, sizeof(ack), true, handoff_deadline) ||
        !binder_bootstrap::Transfer(pipe, &committed, sizeof(committed), false, handoff_deadline)) return EIO;
    if (committed.magic != wire::kOpenCommitMagic || committed.version != wire::kVersion) return EPROTO;
    if (!state.VerifyPeer(pipe)) {
      CloseHandle(binder_bootstrap::DecodeHandle(reply->handle));
      return EACCES;
    }
  }
  return reply->status;
}
}  // namespace

bool HasBroker() {
  const auto& binding = binder_endpoint::Current();
  if (binding.IsGuest() && !binding.IsStandalone()) return true;
  const auto& state = cohost_bootstrap::Current();
  return state && !state.storage_pipe.empty();
}

int StatAndroidData(wire::Filesystem* filesystem) {
  if (filesystem == nullptr) return EFAULT;
  const auto& binding = binder_endpoint::Current();
  if (binding.IsGuest() && !binding.IsStandalone()) {
    return binder_endpoint::QueryAndroidDataCapacity(filesystem);
  }
  wire::Reply reply;
  return Call(wire::Op::kStatAndroidData, {true, 0, wire::kCredential, {}}, {}, 0, &reply, filesystem);
}

Selection Lookup(const std::wstring& mapped) {
  auto drop = drop_mount::Lookup(mapped);
  if (drop.matched) return {true, drop.error, wire::kCredential, std::move(drop.relative), drop.alias};
  const auto& state = cohost_bootstrap::Current();
  if (!state || state.storage_pipe.empty()) return {};
  struct Root {
    std::wstring path;
    uint32_t area;
  };
  static const std::array roots{
      Root{AbsoluteDosPath(state.credential), wire::kCredential},
      Root{AbsoluteDosPath(state.device), wire::kDevice},
      Root{AbsoluteDosPath(state.credential_canonical), wire::kCredential},
      Root{AbsoluteDosPath(state.device_canonical), wire::kDevice},
  };
  if (std::any_of(roots.begin(), roots.end(), [](const Root& root) { return root.path.empty(); })) {
    return {false, EACCES, wire::kCredential, {}};
  }
  const std::wstring absolute = AbsoluteDosPath(mapped);
  if (absolute.empty()) return {false, ENAMETOOLONG, wire::kCredential, {}};
  const Root* selected = nullptr;
  for (const auto& root : roots) {
    if (Within(absolute, root.path) && (!selected || root.path.size() > selected->path.size())) selected = &root;
  }
  if (!selected) return {};
  size_t start = selected->path.size();
  if (start < absolute.size() && absolute[start] == L'\\') ++start;
  auto relative = absolute.substr(start);
  std::replace(relative.begin(), relative.end(), L'\\', L'/');
  return {true, 0, selected->area, std::move(relative)};
}

std::wstring DescriptorPath(HANDLE file) {
  const auto& state = cohost_bootstrap::Current();
  if (!state || state.storage_pipe.empty()) return {};
  constexpr DWORD format = FILE_NAME_NORMALIZED | VOLUME_NAME_NT;
  const DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, format);
  if (size == 0 || size > wire::kMaxPathUnits + 1) return {};
  std::wstring name(size, L'\0');
  const DWORD used = GetFinalPathNameByHandleW(file, name.data(), size, format);
  if (used == 0 || used >= size) return {};
  name.resize(used);
  if (Within(name, state.credential_nt)) {
    return state.credential_canonical + name.substr(state.credential_nt.size());
  }
  if (Within(name, state.device_nt)) {
    return state.device_canonical + name.substr(state.device_nt.size());
  }
  return {};
}

std::optional<wire::Op> LookupBootstrap(const std::wstring& mapped) {
  const auto& state = cohost_bootstrap::Current();
  if (!state || (state.build_prop.empty() && state.framework_res.empty())) return std::nullopt;
  const std::wstring absolute = AbsoluteDosPath(mapped);
  static const std::array inputs{
      std::pair{state.build_prop.empty() ? std::wstring{} : AbsoluteDosPath(state.build_prop),
                wire::Op::kOpenBuildProp},
      std::pair{state.framework_res.empty() ? std::wstring{} : AbsoluteDosPath(state.framework_res),
                wire::Op::kOpenFrameworkRes},
  };
  for (const auto& [bound, operation] : inputs) {
    if (absolute.size() == bound.size() && Within(absolute, bound)) return operation;
  }
  return std::nullopt;
}

static int OpenKind(wire::Op operation, const Selection& path, uint32_t flags,
                    HANDLE* file, bool* created) {
  if (!file) return EINVAL;
  *file = INVALID_HANDLE_VALUE;
  if (created) *created = false;
  wire::Reply reply;
  const int error = Call(operation, path, {}, flags, &reply);
  if (error != 0) return error;
  HANDLE result = binder_bootstrap::DecodeHandle(reply.handle);
  if (GetFileType(result) != FILE_TYPE_DISK) {
    CloseHandle(result);
    return EPROTO;
  }
  *file = result;
  if (created) *created = (reply.flags & wire::kCreated) != 0;
  return 0;
}

int Open(const Selection& path, uint32_t flags, HANDLE* file, bool* created) {
  if (path.drop_alias) {
    if (created) *created = false;
    return drop_mount::Open({true, path.error, path.drop_alias, path.relative}, flags, file);
  }
  return OpenKind(wire::Op::kOpen, path, flags, file, created);
}

int OpenBootstrap(wire::Op operation, HANDLE* file) {
  if (operation != wire::Op::kOpenBuildProp && operation != wire::Op::kOpenFrameworkRes) return EINVAL;
  return OpenKind(operation, {true, 0, wire::kCredential, {}}, 0, file, nullptr);
}

int MakeDirectory(const Selection& path) {
  if (path.drop_alias) return EROFS;
  wire::Reply reply;
  return Call(wire::Op::kMakeDirectory, path, {}, 0, &reply);
}

int Remove(const Selection& path, bool directory) {
  if (path.drop_alias) return EROFS;
  wire::Reply reply;
  return Call(wire::Op::kRemove, path, {}, directory ? wire::kDirectory : 0, &reply);
}

int Rename(const Selection& source, const Selection& target, bool replace) {
  if (source.drop_alias || target.drop_alias) return EROFS;
  wire::Reply reply;
  return Call(wire::Op::kRename, source, target, replace ? wire::kReplace : 0, &reply);
}

}  // namespace gesher::storage_root
