/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gesher/drop_mount.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <gesher/binder_endpoint.h>
#include <gesher/rooted_storage_wire.h>

#include "../../windows/drop_worker_wire.h"

namespace gesher::drop_mount {
namespace {

namespace wire = windows::drop_wire;
namespace storage = rooted_storage_wire;
constexpr ULONGLONG kDeadline = 2500;

struct Alias {
  Channel* owner;
  std::weak_ptr<Channel> channel;
  uint64_t lease;
  uint32_t item;
  std::wstring name;
  std::shared_ptr<Channel> activity;
};

struct Aliases {
  std::mutex lock;
  std::map<uint64_t, Alias> items;
  uint64_t next = 1;
  std::atomic<bool> enabled{false};
};

Aliases& Global() {
  static auto* aliases = new Aliases();
  return *aliases;
}

ULONGLONG Deadline() { return GetTickCount64() + kDeadline; }

bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write, ULONGLONG deadline) {
  return binder_bootstrap::Transfer(pipe, bytes, size, write, deadline);
}

std::wstring AliasPath(uint64_t identity, const std::wstring& name) {
  return std::wstring(kPathPrefix) + std::to_wstring(identity) + L"\\" + name;
}

bool RequestOK(HANDLE pipe, wire::Op operation, uint64_t identity = 0) {
  if (pipe == INVALID_HANDLE_VALUE) return false;
  wire::Request request;
  request.op = operation;
  request.identity = identity;
  wire::Reply reply;
  const auto deadline = Deadline();
  return Transfer(pipe, &request, sizeof(request), true, deadline) &&
      Transfer(pipe, &reply, sizeof(reply), false, deadline) &&
      reply.magic == wire::kMagic && reply.status == 0 && reply.handle == 0;
}

bool PathPart(std::wstring_view name) {
  return !name.empty() && name != L"." && name != L".." &&
      name.find_first_of(L"\\/:") == std::wstring_view::npos &&
      name.find(L'\0') == std::wstring_view::npos;
}

}  // namespace

class Channel::State {
 public:
  explicit State(HANDLE value) : pipe(value) {}
  ~State() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }

  std::mutex lock;
  HANDLE pipe;
  std::weak_ptr<Channel> self;
  std::atomic<bool> detached{false};
};

Channel::Channel(HANDLE pipe) : state_(std::make_unique<State>(pipe)) {}

std::shared_ptr<Channel> Channel::Attach(HWND window) {
  if (!window) return {};
  HANDLE pipe = binder_endpoint::OpenDropWorker();
  if (pipe == INVALID_HANDLE_VALUE) return {};
  auto channel = std::shared_ptr<Channel>(new Channel(pipe));
  wire::Request request;
  request.op = wire::Op::kAttach;
  request.identity = reinterpret_cast<uintptr_t>(window);
  wire::Reply reply;
  const auto deadline = Deadline();
  if (!Transfer(pipe, &request, sizeof(request), true, deadline) ||
      !Transfer(pipe, &reply, sizeof(reply), false, deadline) ||
      reply.magic != wire::kMagic || reply.status != 0 || reply.handle) return {};
  channel->state_->self = channel;
  Global().enabled.store(true, std::memory_order_release);
  return channel;
}

Channel::~Channel() { Close(); }

void Channel::Close() {
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    if (state_->pipe != INVALID_HANDLE_VALUE) CloseHandle(state_->pipe);
    state_->pipe = INVALID_HANDLE_VALUE;
  }
  auto& global = Global();
  std::lock_guard<std::mutex> guard(global.lock);
  for (auto item = global.items.begin(); item != global.items.end();) {
    item = item->second.owner == this ? global.items.erase(item) : std::next(item);
  }
}

bool Channel::Poll(Event* event) {
  if (!event) return false;
  *event = {};
  std::lock_guard<std::mutex> guard(state_->lock);
  HANDLE pipe = state_->pipe;
  if (pipe == INVALID_HANDLE_VALUE || Detached()) return false;
  wire::Request request;
  request.op = wire::Op::kPoll;
  wire::Event result;
  const auto deadline = Deadline();
  if (!Transfer(pipe, &request, sizeof(request), true, deadline) ||
      !Transfer(pipe, &result, sizeof(result), false, deadline) ||
      (result.flags & ~wire::kFileDrag) || result.items > wire::kMaxRoots || result.text_units > wire::kMaxTextUnits ||
      (result.action == wire::Action::kDrop ?
          (result.text_units ? (result.items != 0 || result.lease != 0)
                             : (result.items == 0 || result.lease == 0))
                                          : (result.items != 0 || result.lease != 0 || result.text_units != 0)) ||
      static_cast<uint32_t>(result.action) > static_cast<uint32_t>(wire::Action::kExit)) return false;
  event->action = static_cast<uint32_t>(result.action);
  event->x = result.x;
  event->y = result.y;
  event->file_drag = (result.flags & wire::kFileDrag) != 0;
  if (result.text_units) {
    event->text.resize(result.text_units);
    return Transfer(pipe, event->text.data(), result.text_units * sizeof(wchar_t), false, deadline);
  }
  if (!result.items) return true;

  std::vector<std::wstring> names;
  names.reserve(result.items);
  for (uint32_t i = 0; i < result.items; ++i) {
    wire::Item item{};
    if (!Transfer(pipe, &item, sizeof(item), false, deadline) ||
        item.name_units == 0 || item.name_units > 255 || item.directory > 1) return false;
    std::wstring name(item.name_units, L'\0');
    if (!Transfer(pipe, name.data(), item.name_units * sizeof(wchar_t), false, deadline) ||
        !PathPart(name)) return false;
    names.push_back(std::move(name));
  }

  auto& global = Global();
  std::lock_guard<std::mutex> lock(global.lock);
  if (global.next > UINT64_MAX - result.items) return false;
  for (uint32_t i = 0; i < result.items; ++i) {
    const uint64_t alias = global.next++;
    auto path = AliasPath(alias, names[i]);
    std::replace(path.begin(), path.end(), L'\\', L'/');
    event->paths.push_back(std::move(path));
    global.items.emplace(alias, Alias{this, state_->self, result.lease, i, std::move(names[i]), {}});
  }
  return true;
}

int Channel::Open(uint64_t lease, uint32_t item, std::wstring_view relative, bool directory,
                  bool metadata, HANDLE* file) {
  std::lock_guard<std::mutex> guard(state_->lock);
  HANDLE pipe = state_->pipe;
  if (pipe == INVALID_HANDLE_VALUE) return ENOENT;
  if (relative.size() > wire::kMaxRelativeUnits) return ENAMETOOLONG;
  wire::Request request;
  request.op = wire::Op::kOpen;
  request.identity = lease;
  request.item = item;
  request.units = static_cast<uint32_t>(relative.size());
  request.flags = (directory ? wire::kOpenDirectory : 0) | (metadata ? wire::kOpenMetadata : 0);
  wire::Reply reply;
  const auto deadline = Deadline();
  if (!Transfer(pipe, &request, sizeof(request), true, deadline) ||
      (!relative.empty() && !Transfer(pipe, const_cast<wchar_t*>(relative.data()),
                                     request.units * sizeof(wchar_t), true, deadline)) ||
      !Transfer(pipe, &reply, sizeof(reply), false, deadline) ||
      reply.magic != wire::kMagic || reply.status < 0) return EIO;
  if (reply.status) return reply.status;
  if (!reply.handle || reply.handle > UINTPTR_MAX) return EPROTO;
  uint32_t ack = wire::kMagic, commit = 0;
  if (!Transfer(pipe, &ack, sizeof(ack), true, deadline) ||
      !Transfer(pipe, &commit, sizeof(commit), false, deadline) ||
      commit != wire::kMagic) return EIO;
  *file = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(reply.handle));
  return 0;
}

bool Enabled() { return Global().enabled.load(std::memory_order_acquire); }

Selection Lookup(std::wstring_view mapped) {
  if (!mapped.starts_with(kPathPrefix) || !Enabled()) return {};
  mapped.remove_prefix(std::size(kPathPrefix) - 1);
  const size_t separator = mapped.find(L'\\');
  if (separator == std::wstring_view::npos || separator == 0 || separator > 20) {
    return {true, ENOENT, 0, {}};
  }
  uint64_t identity = 0;
  for (wchar_t digit : mapped.substr(0, separator)) {
    if (digit < L'0' || digit > L'9' || identity > (UINT64_MAX - (digit - L'0')) / 10) {
      return {true, ENOENT, 0, {}};
    }
    identity = identity * 10 + (digit - L'0');
  }
  mapped.remove_prefix(separator + 1);
  const size_t slash = mapped.find(L'\\');
  auto& global = Global();
  std::lock_guard<std::mutex> guard(global.lock);
  const auto found = global.items.find(identity);
  if (found == global.items.end() || mapped.substr(0, slash) != found->second.name) {
    return {true, ENOENT, 0, {}};
  }
  std::wstring relative = slash == std::wstring_view::npos ? L"" : std::wstring(mapped.substr(slash + 1));
  std::replace(relative.begin(), relative.end(), L'\\', L'/');
  return {true, 0, identity, std::move(relative)};
}

int Open(const Selection& selection, uint32_t flags, HANDLE* file) {
  if (!file) return EINVAL;
  *file = INVALID_HANDLE_VALUE;
  if (selection.error) return selection.error;
  const uint32_t access = flags & storage::kAccessMask;
  const bool metadata = access == storage::kMetadata;
  constexpr uint32_t writable = storage::kCreate | storage::kTruncate |
      storage::kDeleteOnClose | storage::kWriteThrough;
  if ((access != storage::kRead && !metadata) || (flags & writable) != 0) return EROFS;
  if (flags & storage::kOverlapped) return EOPNOTSUPP;

  Alias item{};
  auto& global = Global();
  {
    std::lock_guard<std::mutex> guard(global.lock);
    const auto found = global.items.find(selection.alias);
    if (found == global.items.end()) return ENOENT;
    if (!found->second.activity) return EACCES;
    item = found->second;
  }
  const auto channel = item.channel.lock();
  if (!channel) return ENOENT;
  const int error = channel->Open(item.lease, item.item, selection.relative,
      (flags & storage::kDirectory) != 0, metadata, file);
  if (error) return error;

  return 0;
}

bool Channel::Detached() const { return state_->detached.load(std::memory_order_acquire); }

void Channel::Detach() {
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    if (state_->detached.exchange(true, std::memory_order_acq_rel)) return;
  }
  std::set<uint64_t> discarded;
  bool retained = false;
  auto& global = Global();
  {
    std::lock_guard<std::mutex> guard(global.lock);
    for (auto item = global.items.begin(); item != global.items.end();) {
      if (item->second.owner != this) {
        ++item;
      } else if (item->second.activity) {
        retained = true;
        ++item;
      } else {
        discarded.insert(item->second.lease);
        item = global.items.erase(item);
      }
    }
  }
  if (!retained) {
    Close();
    return;
  }
  bool failed = false;
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    HANDLE pipe = state_->pipe;
    if (pipe == INVALID_HANDLE_VALUE) return;
    failed = !RequestOK(pipe, wire::Op::kDetach);
  }
  if (failed) {
    Close();
    return;
  }
  for (const auto lease : discarded) Release(lease);
}

bool Channel::Take(uint64_t lease) {
  std::lock_guard<std::mutex> guard(state_->lock);
  return !Detached() && RequestOK(state_->pipe, wire::Op::kTake, lease);
}

void Channel::Release(uint64_t lease) {
  bool failed = false;
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    if (state_->pipe == INVALID_HANDLE_VALUE) return;
    failed = !RequestOK(state_->pipe, wire::Op::kRelease, lease);
  }
  if (failed) Close();
}

bool Keep(std::wstring_view path) {
  std::wstring mapped(path);
  std::replace(mapped.begin(), mapped.end(), L'/', L'\\');
  const auto selection = Lookup(mapped);
  if (!selection.alias) return false;
  auto& global = Global();
  std::shared_ptr<Channel> channel;
  uint64_t lease = 0;
  {
    std::lock_guard<std::mutex> guard(global.lock);
    const auto found = global.items.find(selection.alias);
    if (found == global.items.end()) return false;
    if (found->second.activity) return true;
    channel = found->second.channel.lock();
    lease = found->second.lease;
  }
  if (!channel || !channel->Take(lease)) return false;
  std::lock_guard<std::mutex> guard(global.lock);
  if (!global.items.contains(selection.alias) || channel->Detached()) return false;
  for (auto& entry : global.items) {
    auto& item = entry.second;
    if (item.owner == channel.get() && item.lease == lease) item.activity = channel;
  }
  return true;
}

void Release(std::wstring_view path) {
  std::wstring mapped(path);
  std::replace(mapped.begin(), mapped.end(), L'/', L'\\');
  const auto selection = Lookup(mapped);
  if (!selection.alias) return;
  std::shared_ptr<Channel> channel;
  uint64_t lease = 0;
  auto& global = Global();
  {
    std::lock_guard<std::mutex> guard(global.lock);
    const auto found = global.items.find(selection.alias);
    if (found == global.items.end()) return;
    channel = found->second.channel.lock();
    lease = found->second.lease;
    for (auto item = global.items.begin(); item != global.items.end();) {
      item = item->second.owner == channel.get() && item->second.lease == lease
          ? global.items.erase(item) : std::next(item);
    }
  }
  if (channel) channel->Release(lease);
}

}  // namespace gesher::drop_mount
