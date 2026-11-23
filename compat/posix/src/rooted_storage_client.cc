// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <errno.h>
#include <gesher/binder_bootstrap.h>
#include <gesher/cohost_bootstrap.h>

#include <algorithm>
#include <string_view>
#include <utility>

#include "rooted_storage_client.h"

namespace gesher::storage_root {
namespace {
namespace wire = rooted_storage_wire;
static_assert(sizeof(wchar_t) == 2);

std::wstring AbsoluteDosPath(std::wstring path) {
  if (path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\?\\UNC\\")) path.erase(0, 4);
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
  return !root.empty() && path.size() >= root.size() &&
      CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(),
                           static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
      (path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\');
}

int Call(wire::Op operation, const Selection& path, const Selection& target, uint32_t flags,
         wire::Reply* reply) {
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
  if (reply->status != 0 || operation != wire::Op::kOpen) {
    if (reply->handle != 0 || reply->flags != 0) return EPROTO;
  } else if (reply->handle == 0 || reply->handle == UINT64_MAX || reply->handle > UINTPTR_MAX) {
    return EPROTO;
  }
  if (!state.VerifyPeer(pipe)) return EACCES;
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

Selection Lookup(const std::wstring& mapped) {
  const auto& state = cohost_bootstrap::Current();
  if (!state || state.storage_pipe.empty()) return {};
  struct Roots {
    std::wstring credential, device;
  };
  static const Roots roots{AbsoluteDosPath(state.credential), AbsoluteDosPath(state.device)};
  if (roots.credential.empty() || roots.device.empty()) return {false, EACCES, wire::kCredential, {}};
  const std::wstring absolute = AbsoluteDosPath(mapped);
  if (absolute.empty()) return {false, ENAMETOOLONG, wire::kCredential, {}};
  const bool ce = Within(absolute, roots.credential);
  const bool de = Within(absolute, roots.device);
  if (!ce && !de) return {};
  const bool choose_device = de && (!ce || roots.device.size() > roots.credential.size());
  const auto& root = choose_device ? roots.device : roots.credential;
  size_t start = root.size();
  if (start < absolute.size() && absolute[start] == L'\\') ++start;
  auto relative = absolute.substr(start);
  std::replace(relative.begin(), relative.end(), L'\\', L'/');
  return {true, 0, choose_device ? wire::kDevice : wire::kCredential, std::move(relative)};
}

int Open(const Selection& path, uint32_t flags, HANDLE* file, bool* created) {
  if (!file) return EINVAL;
  *file = INVALID_HANDLE_VALUE;
  if (created) *created = false;
  wire::Reply reply;
  const int error = Call(wire::Op::kOpen, path, {}, flags, &reply);
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

int MakeDirectory(const Selection& path) {
  wire::Reply reply;
  return Call(wire::Op::kMakeDirectory, path, {}, 0, &reply);
}

int Remove(const Selection& path, bool directory) {
  wire::Reply reply;
  return Call(wire::Op::kRemove, path, {}, directory ? wire::kDirectory : 0, &reply);
}

int Rename(const Selection& source, const Selection& target, bool replace) {
  wire::Reply reply;
  return Call(wire::Op::kRename, source, target, replace ? wire::kReplace : 0, &reply);
}

}  // namespace gesher::storage_root
