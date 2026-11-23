// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "rooted_storage_server.h"

#include <bcrypt.h>

#include <array>
#include <atomic>

#include <gesher/binder_bootstrap.h>
#include <gesher/rooted_storage_wire.h>
#include <gesher/windows_process_identity.h>

#include "../binder/peer_registry.h"
#include "app_container.h"

namespace gesher::rooted_storage {
namespace {

namespace wire = rooted_storage_wire;
using binder_bootstrap::Transfer;
using binder_peer::OwnedHandle;
using process_identity::Process;

constexpr ACCESS_MASK kPeerAccess = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE;
constexpr ULONGLONG kOperationWindowMillis = 5000;

std::wstring PipeName() {
  std::array<UCHAR, 16> nonce{};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                                     BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
  constexpr wchar_t kHex[] = L"0123456789abcdef";
  std::wstring name = L"\\\\.\\pipe\\gesher-rooted-storage-";
  for (UCHAR byte : nonce) {
    name += kHex[byte >> 4];
    name += kHex[byte & 15];
  }
  return name;
}

bool Retain(HANDLE handle, OwnedHandle* retained, ACCESS_MASK rights, DWORD options = 0) {
  HANDLE result = nullptr;
  HANDLE current = GetCurrentProcess();
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      !DuplicateHandle(current, handle, current, &result, rights, FALSE, options)) return false;
  retained->reset(result);
  return true;
}

Area ReadArea(uint32_t area) {
  return area == wire::kCredential ? Area::kCredential : Area::kDevice;
}

bool Valid(const wire::Request& request) {
  if (request.magic != wire::kMagic || request.version != wire::kVersion ||
      request.source_area > wire::kDevice || request.target_area > wire::kDevice ||
      request.path_units > wire::kMaxPathUnits || request.target_units > wire::kMaxPathUnits ||
      (request.operation != wire::Op::kRename &&
          (request.target_units != 0 || request.target_area != wire::kCredential))) return false;
  switch (request.operation) {
    case wire::Op::kOpen:
      return (request.flags & ~wire::kOpenFlags) == 0 &&
          (request.flags & wire::kAccessMask) <= wire::kMetadataWrite &&
          ((request.flags & wire::kShareMask) == 0 || (request.flags & wire::kShareOverride) != 0);
    case wire::Op::kMakeDirectory: return request.flags == 0;
    case wire::Op::kRemove: return (request.flags & ~wire::kDirectory) == 0;
    case wire::Op::kRename: return (request.flags & ~wire::kReplace) == 0;
  }
  return false;
}

OpenRequest DecodeOpen(uint32_t flags) {
  OpenRequest result;
  switch (flags & wire::kAccessMask) {
    case wire::kRead: result.access = Access::kRead; break;
    case wire::kWrite: result.access = Access::kWrite; break;
    case wire::kReadWrite: result.access = Access::kReadWrite; break;
    case wire::kMetadata: result.access = Access::kMetadata; break;
    case wire::kMetadataWrite: result.access = Access::kMetadataWrite; break;
  }
  result.create = (flags & wire::kCreate) != 0;
  result.exclusive = (flags & wire::kExclusive) != 0;
  result.truncate = (flags & wire::kTruncate) != 0;
  result.directory = (flags & wire::kDirectory) != 0;
  result.read_only_create = (flags & wire::kReadOnlyCreate) != 0;
  result.write_through = (flags & wire::kWriteThrough) != 0;
  result.delete_on_close = (flags & wire::kDeleteOnClose) != 0;
  result.overlapped = (flags & wire::kOverlapped) != 0;
  if (flags & wire::kShareOverride) result.share_mode = (flags & wire::kShareMask) >> wire::kShareShift;
  return result;
}

bool ReadName(HANDLE pipe, uint32_t units, std::wstring* name, ULONGLONG deadline) {
  name->resize(units);
  return Transfer(pipe, name->data(), units * sizeof(wchar_t), false, deadline);
}

int Dispatch(const Roots& roots, const wire::Request& request, std::wstring_view path,
             std::wstring_view target, OwnedHandle* opened, bool* created) {
  const Area area = ReadArea(request.source_area);
  switch (request.operation) {
    case wire::Op::kOpen: {
      HANDLE file = INVALID_HANDLE_VALUE;
      int error = Open(roots, area, path, DecodeOpen(request.flags), &file, created);
      if (error == 0) opened->reset(file);
      return error;
    }
    case wire::Op::kMakeDirectory: return MakeDirectory(roots, area, path);
    case wire::Op::kRemove: return Remove(roots, area, path, (request.flags & wire::kDirectory) != 0);
    case wire::Op::kRename:
      return Rename(roots, area, path, ReadArea(request.target_area), target, (request.flags & wire::kReplace) != 0);
  }
  return EINVAL;
}

// The guest adopts the handle only after the complete commit. Before that
// point, a disappearing or stalled connection leaves the handle with us.
class PendingHandle {
 public:
  PendingHandle(HANDLE process, HANDLE file) : process_(process) {
    DuplicateHandle(GetCurrentProcess(), file, process_, &remote_, 0, FALSE, DUPLICATE_SAME_ACCESS);
  }
  ~PendingHandle() {
    if (remote_ != nullptr) {
      HANDLE recovered = nullptr;
      if (DuplicateHandle(process_, remote_, GetCurrentProcess(), &recovered, 0, FALSE,
                          DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) CloseHandle(recovered);
    }
  }
  PendingHandle(const PendingHandle&) = delete;
  PendingHandle& operator=(const PendingHandle&) = delete;

  uint64_t Encode() const { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote_)); }
  void Commit() { remote_ = nullptr; }

 private:
  HANDLE process_;
  HANDLE remote_ = nullptr;
};

}  // namespace

class Server::State {
 public:
  explicit State(const Roots& roots) : name(PipeName()) {
    if (!Retain(roots.credential, &ce_, 0, DUPLICATE_SAME_ACCESS) ||
        !Retain(roots.device, &de_, 0, DUPLICATE_SAME_ACCESS)) {
      ce_.reset();
      de_.reset();
    }
  }

  ~State() { Stop(); }

  DWORD Start(HANDLE child, HANDLE job) {
    if (name.empty() || !ce_ || !de_ || child_ || stopped_) return ERROR_INVALID_PARAMETER;
    BY_HANDLE_FILE_INFORMATION ce{}, de{};
    if (Stat(roots(), Area::kCredential, L"", &ce) != 0 || Stat(roots(), Area::kDevice, L"", &de) != 0 ||
        (ce.dwVolumeSerialNumber == de.dwVolumeSerialNumber && ce.nFileIndexHigh == de.nFileIndexHigh &&
            ce.nFileIndexLow == de.nFileIndexLow)) return ERROR_INVALID_PARAMETER;
    BOOL member = FALSE;
    if (!IsProcessInJob(child, job, &member) || !member ||
        !process_identity::ReadLiveProcess(child, &launch_) || launch_.principal.container.size == 0 ||
        launch_.principal.integrity >= SECURITY_MANDATORY_MEDIUM_RID) return ERROR_ACCESS_DENIED;
    if (!Retain(child, &child_, kPeerAccess) || !Retain(job, &job_, JOB_OBJECT_QUERY)) return GetLastError();

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD error = appcontainer::AppContainerPipeSecurity(&descriptor, launch_.principal.container.bytes.data());
    if (error != ERROR_SUCCESS) return error;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    for (size_t index = 0; index < instances_.size(); ++index) {
      Instance& instance = instances_[index];
      instance.owner = this;
      const DWORD first = index == 0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0;
      HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | first,
          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
          static_cast<DWORD>(instances_.size()), 4096, 4096, 0, &attributes);
      if (pipe == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        break;
      }
      instance.pipe.reset(pipe);
      instance.worker.reset(CreateThread(nullptr, 0, RunInstance, &instance, 0, nullptr));
      if (!instance.worker) {
        error = GetLastError();
        break;
      }
    }
    LocalFree(descriptor);
    if (error != ERROR_SUCCESS) Stop();
    return error;
  }

  const std::wstring name;

 private:
  struct Instance {
    State* owner = nullptr;
    OwnedHandle pipe;
    OwnedHandle worker;
  };

  Roots roots() const { return {ce_.get(), de_.get()}; }

  bool IsLive() const {
    return !stopped_.load(std::memory_order_acquire) && child_ &&
        WaitForSingleObject(child_.get(), 0) == WAIT_TIMEOUT;
  }

  bool PinPeer(HANDLE pipe, OwnedHandle* peer, Process* identity) const {
    ULONG pid = 0;
    if (!IsLive() || !GetNamedPipeClientProcessId(pipe, &pid) || pid == 0) return false;
    if (pid == launch_.pid) {
      if (!Retain(child_.get(), peer, kPeerAccess)) return false;
    } else {
      peer->reset(OpenProcess(kPeerAccess, FALSE, pid));
    }
    BOOL member = FALSE;
    return *peer && IsProcessInJob(peer->get(), job_.get(), &member) && member &&
        process_identity::ReadLiveProcess(peer->get(), identity) && identity->pid == pid &&
        (pid == launch_.pid ? identity->birth == launch_.birth : identity->birth >= launch_.birth) &&
        identity->principal == launch_.principal;
  }

  static DWORD WINAPI RunInstance(void* opaque) {
    auto& instance = *static_cast<Instance*>(opaque);
    HANDLE pipe = instance.pipe.get();
    while (instance.owner->IsLive()) {
      if (!ConnectNamedPipe(pipe, nullptr)) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_LISTENING) { Sleep(2); continue; }
        if (error != ERROR_PIPE_CONNECTED) {
          if (error == ERROR_NO_DATA) { DisconnectNamedPipe(pipe); continue; }
          break;
        }
      }
      instance.owner->Serve(pipe);
      DisconnectNamedPipe(pipe);
    }
    return 0;
  }

  void Serve(HANDLE pipe) {
    OwnedHandle peer;
    Process identity;
    if (!PinPeer(pipe, &peer, &identity)) return;
    wire::Request request;
    std::wstring path, target;
    const ULONGLONG read_deadline = GetTickCount64() + kOperationWindowMillis;
    if (!Transfer(pipe, &request, sizeof(request), false, read_deadline) || !Valid(request) ||
        !ReadName(pipe, request.path_units, &path, read_deadline) ||
        !ReadName(pipe, request.target_units, &target, read_deadline) ||
        !process_identity::MatchPipeSender(pipe, peer.get(), identity) || !IsLive()) return;

    OwnedHandle file;
    bool created = false;
    wire::Reply reply;
    reply.status = Dispatch(roots(), request, path, target, &file, &created);
    if (!IsLive()) return;
    const ULONGLONG send_deadline = GetTickCount64() + kOperationWindowMillis;
    if (file) {
      if (!process_identity::MatchPipeSender(pipe, peer.get(), identity)) return;
      if (!TransferFile(pipe, peer.get(), identity, file.get(), created, send_deadline)) return;
    } else if (!Transfer(pipe, &reply, sizeof(reply), true, send_deadline)) {
      return;
    }
    // DisconnectNamedPipe discards unread response bytes. An honest peer
    // closes after reading; any other connection is bounded by send_deadline.
    while (IsLive() && GetTickCount64() < send_deadline &&
           PeekNamedPipe(pipe, nullptr, 0, nullptr, nullptr, nullptr)) Sleep(1);
  }

  bool TransferFile(HANDLE pipe, HANDLE peer, const Process& identity, HANDLE file,
                    bool created, ULONGLONG deadline) const {
    PendingHandle handle(peer, file);
    wire::Reply reply;
    reply.handle = handle.Encode();
    if (reply.handle == 0) {
      reply.status = EIO;
      return Transfer(pipe, &reply, sizeof(reply), true, deadline);
    }
    if (created) reply.flags = wire::kCreated;
    wire::OpenAck acknowledgement{};
    if (!Transfer(pipe, &reply, sizeof(reply), true, deadline) ||
        !Transfer(pipe, &acknowledgement, sizeof(acknowledgement), false, deadline) ||
        acknowledgement.magic != wire::kOpenAckMagic || acknowledgement.version != wire::kVersion ||
        !IsLive() || !process_identity::MatchPipeSender(pipe, peer, identity)) return false;
    wire::OpenAck commit{wire::kOpenCommitMagic, wire::kVersion};
    if (!Transfer(pipe, &commit, sizeof(commit), true, deadline)) return false;
    handle.Commit();
    return true;
  }

  void Stop() {
    stopped_.store(true, std::memory_order_release);
    for (auto& instance : instances_) {
      if (instance.worker) CancelSynchronousIo(instance.worker.get());
    }
    for (auto& instance : instances_) {
      if (instance.worker) WaitForSingleObject(instance.worker.get(), INFINITE);
      instance.worker.reset();
      instance.pipe.reset();
    }
  }

  OwnedHandle ce_, de_, child_, job_;
  Process launch_;
  std::atomic<bool> stopped_{false};
  std::array<Instance, 2> instances_;
};

Server::Server(const Roots& roots) : state_(new State(roots)) {}
Server::~Server() = default;
const std::wstring& Server::name() const { return state_->name; }
DWORD Server::Start(HANDLE child, HANDLE job) { return state_->Start(child, job); }

}  // namespace gesher::rooted_storage
