/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <windows.h>
#include <ole2.h>

#include <cerrno>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <gesher/binder_bootstrap.h>

#include "../binder/peer_registry.h"
#include "drop_leases.h"
#include "drop_paths.h"
#include "drop_worker_wire.h"

namespace gesher::windows {
namespace {

namespace wire = drop_wire;
using binder_bootstrap::Transfer;
using binder_peer::OwnedHandle;

constexpr size_t kMaxEvents = 48;
constexpr ULONGLONG kTransactionMillis = 2500;

bool Send(HANDLE pipe, void* bytes, DWORD size, ULONGLONG deadline) {
  return Transfer(pipe, bytes, size, true, deadline);
}

class DropTarget final : public IDropTarget {
 public:
  DropTarget(std::shared_ptr<HostedDropWindow> window, HWND hwnd)
      : window_(std::move(window)), hwnd_(hwnd) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDropTarget) return E_NOINTERFACE;
    AddRef();
    *result = static_cast<IDropTarget*>(this);
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (!left) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL at, DWORD* effect) override {
    if (!effect) return E_POINTER;
    const auto supports = [data](CLIPFORMAT type) {
      FORMATETC format{type, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      return data->QueryGetData(&format) == S_OK;
    };
    OleDropSource authority;
    active_ = (*effect & DROPEFFECT_COPY) != 0 && window_->Live() && data &&
        events_.size() <= kMaxEvents - 6 && authority.Capture() &&
        (supports(CF_HDROP) || supports(CF_UNICODETEXT) || supports(CF_TEXT));
    *effect = active_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (active_) {
      file_drag_ = supports(CF_HDROP);
      Push(wire::Action::kStart, at);
      Push(wire::Action::kEnter, at);
      Push(wire::Action::kLocation, at);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL at, DWORD* effect) override {
    if (!effect) return E_POINTER;
    *effect = active_ && window_->Live() && (*effect & DROPEFFECT_COPY)
        ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (*effect) {
      if (!events_.empty() && events_.back().event.action == wire::Action::kLocation) {
        events_.pop_back();
      }
      if (events_.size() <= kMaxEvents - 3) Push(wire::Action::kLocation, at);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (std::exchange(active_, false)) {
      Push(wire::Action::kExit, {});
      Push(wire::Action::kEnd, {});
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL at, DWORD* effect) override {
    if (!effect) return E_POINTER;
    const bool copy = (*effect & DROPEFFECT_COPY) != 0;
    *effect = DROPEFFECT_NONE;
    if (!std::exchange(active_, false)) return S_OK;
    OleDropSource authority;
    if (copy && data && window_->Live() && authority.Capture()) {
      auto paths = ReadDropPaths(data, wire::kMaxRoots);
      DropLeases::Offer offer;
      if (paths && leases_.Retain(std::move(authority), *paths, &offer) == 0 && window_->Live()) {
        Push(wire::Action::kDrop, at, std::move(offer));
        *effect = DROPEFFECT_COPY;
      } else if (!paths && window_->Live()) {
        auto text = ReadDropText(data, wire::kMaxTextUnits);
        if (!text.empty()) {
          Push(wire::Action::kDrop, at, {}, std::move(text));
          *effect = DROPEFFECT_COPY;
        }
      }
    }
    Push(wire::Action::kEnd, at);
    return S_OK;
  }

  bool Poll(HANDLE pipe, ULONGLONG deadline) {
    if (events_.empty()) {
      wire::Event empty;
      return Send(pipe, &empty, sizeof(empty), deadline);
    }
    Pending pending = std::move(events_.front());
    events_.pop_front();
    if (!Send(pipe, &pending.event, sizeof(pending.event), deadline)) return false;
    if (!pending.text.empty() && !Send(pipe, pending.text.data(),
        pending.event.text_units * sizeof(wchar_t), deadline)) return false;
    for (auto& item : pending.offer.items) {
      const auto size = static_cast<uint32_t>(item.name.size());
      wire::Item header{size, item.directory ? 1u : 0u};
      if (!Send(pipe, &header, sizeof(header), deadline) ||
          !Send(pipe, item.name.data(), size * sizeof(wchar_t), deadline)) return false;
    }
    return true;
  }

  int Open(const wire::Request& request, const std::wstring& relative, HANDLE* result) const {
    return leases_.Open(request.identity, request.item, relative, (request.flags & wire::kOpenDirectory) != 0,
                        (request.flags & wire::kOpenMetadata) != 0, result);
  }

  bool TakeLease(uint64_t identity) { return leases_.Take(identity); }

  void ReleaseLease(uint64_t identity) { leases_.Release(identity); }

  void RetireWindow() {
    active_ = false;
    for (const auto& pending : events_) if (pending.offer.lease) leases_.Release(pending.offer.lease);
    events_.clear();
  }

 private:
  struct Pending {
    wire::Event event;
    DropLeases::Offer offer;
    std::wstring text;
  };

  void Push(wire::Action action, POINTL at, DropLeases::Offer offer = {}, std::wstring text = {}) {
    POINT client{at.x, at.y};
    const auto previous = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd_));
    ScreenToClient(hwnd_, &client);
    if (previous) SetThreadDpiAwarenessContext(previous);
    wire::Event event{action, static_cast<float>(client.x), static_cast<float>(client.y),
                      static_cast<uint32_t>(offer.items.size()), offer.lease,
                      static_cast<uint32_t>(text.size()), file_drag_ ? wire::kFileDrag : 0};
    events_.push_back(Pending{event, std::move(offer), std::move(text)});
  }

  ULONG refs_ = 1;
  std::shared_ptr<HostedDropWindow> window_;
  const HWND hwnd_;
  bool active_ = false;
  bool file_drag_ = false;
  std::deque<Pending> events_;
  DropLeases leases_;
};

bool Valid(const wire::Request& request) {
  if (request.magic != wire::kMagic || request.version != wire::kVersion ||
      request.units > wire::kMaxRelativeUnits) return false;
  if (request.op == wire::Op::kOpen) {
    return request.identity && request.item < wire::kMaxRoots &&
        (request.flags & ~(wire::kOpenDirectory | wire::kOpenMetadata)) == 0;
  }
  if (request.flags || request.units || request.item) return false;
  switch (request.op) {
    case wire::Op::kAttach: return request.identity != 0 && request.identity <= UINTPTR_MAX;
    case wire::Op::kTake:
    case wire::Op::kRelease: return request.identity != 0;
    case wire::Op::kDetach:
    case wire::Op::kPoll: return request.identity == 0;
    default: return false;
  }
}

bool ReadRequest(HANDLE pipe, HANDLE child, const process_identity::Process& guest,
                 wire::Request* request, std::wstring* relative, ULONGLONG deadline) {
  if (!Transfer(pipe, request, sizeof(*request), false, deadline) || !Valid(*request)) return false;
  if (request->units) {
    relative->resize(request->units);
    if (!Transfer(pipe, relative->data(), request->units * sizeof(wchar_t), false, deadline)) return false;
  }
  return process_identity::MatchPipeSender(pipe, child, guest);
}

bool SendHandle(HANDLE pipe, HANDLE child, HANDLE file, ULONGLONG deadline) {
  HANDLE remote = nullptr;
  wire::Reply reply;
  if (!DuplicateHandle(GetCurrentProcess(), file, child, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    reply.status = EACCES;
    return Send(pipe, &reply, sizeof(reply), deadline);
  }
  reply.handle = reinterpret_cast<uintptr_t>(remote);
  uint32_t ack = 0;
  const bool okay = Send(pipe, &reply, sizeof(reply), deadline) &&
      Transfer(pipe, &ack, sizeof(ack), false, deadline) && ack == wire::kMagic &&
      Send(pipe, &ack, sizeof(ack), deadline);
  if (!okay) {
    HANDLE local = nullptr;
    if (DuplicateHandle(child, remote, GetCurrentProcess(), &local, 0, FALSE,
                        DUPLICATE_CLOSE_SOURCE | DUPLICATE_SAME_ACCESS)) CloseHandle(local);
  }
  return okay;
}

int Serve(HANDLE pipe, HANDLE child) {
  process_identity::Process guest;
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!process_identity::ReadLiveProcess(child, &guest) || !guest.principal.container.size ||
      !SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return ERROR_ACCESS_DENIED;

  auto window = std::make_shared<HostedDropWindow>();
  std::wstring relative;
  wire::Request request;
  const ULONGLONG initial = GetTickCount64() + kTransactionMillis;
  if (!ReadRequest(pipe, child, guest, &request, &relative, initial) || request.op != wire::Op::kAttach) {
    return ERROR_ACCESS_DENIED;
  }
  const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(request.identity));
  auto release = [](DropTarget* target) { target->Release(); };
  std::unique_ptr<DropTarget, decltype(release)> target(new DropTarget(window, hwnd), release);
  wire::Reply reply;
  if (FAILED(window->Register(hwnd, child, target.get()))) {
    reply.status = EACCES;
    Send(pipe, &reply, sizeof(reply), initial);
    return ERROR_ACCESS_DENIED;
  }
  auto revoke = [](HostedDropWindow* owned) { owned->Revoke(); };
  const std::unique_ptr<HostedDropWindow, decltype(revoke)> registration(window.get(), revoke);
  if (!Send(pipe, &reply, sizeof(reply), initial)) return ERROR_BROKEN_PIPE;
  while (WaitForSingleObject(child, 0) == WAIT_TIMEOUT) {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) return 0;
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
    if (available < sizeof(wire::Request)) {
      MsgWaitForMultipleObjectsEx(1, &child, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
      continue;
    }
    relative.clear();
    const ULONGLONG deadline = GetTickCount64() + kTransactionMillis;
    if (!ReadRequest(pipe, child, guest, &request, &relative, deadline)) return ERROR_ACCESS_DENIED;
    switch (request.op) {
      case wire::Op::kPoll:
        if (!target->Poll(pipe, deadline)) return ERROR_BROKEN_PIPE;
        break;
      case wire::Op::kDetach:
        window->Revoke();
        target->RetireWindow();
        if (!Send(pipe, &reply, sizeof(reply), deadline)) return ERROR_BROKEN_PIPE;
        break;
      case wire::Op::kTake: {
        wire::Reply result;
        result.status = window->Live() && target->TakeLease(request.identity) ? 0 : EACCES;
        if (!Send(pipe, &result, sizeof(result), deadline)) return ERROR_BROKEN_PIPE;
        break;
      }
      case wire::Op::kRelease:
        target->ReleaseLease(request.identity);
        if (!Send(pipe, &reply, sizeof(reply), deadline)) return ERROR_BROKEN_PIPE;
        break;
      case wire::Op::kOpen: {
        HANDLE opened = INVALID_HANDLE_VALUE;
        wire::Reply result;
        result.status = target->Open(request, relative, &opened);
        const OwnedHandle file(opened);
        const bool okay = result.status ? Send(pipe, &result, sizeof(result), deadline)
                                        : SendHandle(pipe, child, file.get(), deadline);
        if (!okay) return ERROR_BROKEN_PIPE;
        break;
      }
      default: return ERROR_INVALID_DATA;
    }
  }
  return 0;
}

}  // namespace
}  // namespace gesher::windows

int wmain(int count, wchar_t** argv) {
  // The launcher uses an explicit inherited handle list. Neither PID nor pipe
  // name from the command line is used to open a process or a connection.
  if (count != 3) return ERROR_INVALID_PARAMETER;
  HANDLE handles[2]{};
  for (int index = 0; index < 2; ++index) {
    wchar_t* end = nullptr;
    const auto number = _wcstoui64(argv[index + 1], &end, 16);
    if (!number || number > UINTPTR_MAX || !end || *end) return ERROR_INVALID_PARAMETER;
    handles[index] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(number));
  }
  if (GetFileType(handles[0]) != FILE_TYPE_PIPE) return ERROR_INVALID_HANDLE;
  if (FAILED(OleInitialize(nullptr))) return ERROR_NOT_SUPPORTED;
  const int result = SUCCEEDED(gesher::windows::ConfigureDropWorkerCom())
      ? gesher::windows::Serve(handles[0], handles[1])
      : ERROR_ACCESS_DENIED;
  OleUninitialize();
  return result;
}
