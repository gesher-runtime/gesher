// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// An independent raw protocol receiver. It intentionally loses acknowledged
// frames/disconnects at real wire boundaries, without production test hooks.
#include <windows.h>
#include "sg-actor.h"
#include "../../compat/posix/src/binder_wire.h"

namespace {
namespace wire = gesher::binder_wire;
using binder_sg_fixture::Append;
using binder_sg_fixture::Report;
struct Bytes {
  std::vector<uint8_t> data;
  size_t at = 0;
  template <typename T> void Put(const T& value) { Append(&data, value); }
  template <typename T> bool Get(T* value) {
    if (at > data.size() || sizeof(T) > data.size() - at) return false;
    memcpy(value, data.data() + at, sizeof(T)); at += sizeof(T); return true;
  }
};
bool Exact(HANDLE pipe, void* bytes, size_t size, bool write) {
  auto* cursor = static_cast<uint8_t*>(bytes);
  while (size) {
    DWORD count = 0;
    DWORD want = static_cast<DWORD>(size > 65536 ? 65536 : size);
    if (!(write ? WriteFile(pipe, cursor, want, &count, nullptr) : ReadFile(pipe, cursor, want, &count, nullptr)) || count == 0) return false;
    cursor += count; size -= count;
  }
  return true;
}
bool Send(HANDLE pipe, Bytes message) {
  uint32_t length = static_cast<uint32_t>(message.data.size());
  return Exact(pipe, &length, sizeof(length), true) && Exact(pipe, message.data.data(), length, true);
}
bool Receive(HANDLE pipe, Bytes* reply) {
  uint32_t length;
  if (!Exact(pipe, &length, sizeof(length), false) || length > (64u << 20)) return false;
  reply->data.resize(length); reply->at = 0;
  return Exact(pipe, reply->data.data(), length, false);
}
bool Status(HANDLE pipe) {
  Bytes reply;
  int32_t status;
  return Receive(pipe, &reply) && reply.Get(&status) && status == 0 && reply.at == reply.data.size();
}
bool PeekSuccessfulAck(HANDLE pipe) {
  for (unsigned i = 0; i < 500; ++i) {
    uint32_t frame[2]{};
    DWORD copied = 0, available = 0;
    if (!PeekNamedPipe(pipe, frame, sizeof(frame), &copied, &available, nullptr)) return false;
    // The previous frame was fully consumed, so these bytes identify the
    // exact next status ACK: length4/status0. Peek leaves it unread.
    if (copied == sizeof(frame)) return frame[0] == 4 && frame[1] == 0 && available == sizeof(frame);
    Sleep(20);
  }
  return false;
}
HANDLE Connect() {
  const char* device = getenv("GESHER_BINDER");
  if (!device) return INVALID_HANDLE_VALUE;
  std::wstring name = wire::kPipePrefix;
  for (const char* p = device; *p; ++p) name += static_cast<unsigned char>(*p);
  for (unsigned i = 0; i < 1500; ++i) {
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return pipe;
    Sleep(20);
  }
  return INVALID_HANDLE_VALUE;
}
struct Peer {
  HANDLE process = INVALID_HANDLE_VALUE, thread = INVALID_HANDLE_VALUE;
  uint32_t id = 0;
  uint64_t key = 0;
  ~Peer() { CloseThread(); if (process != INVALID_HANDLE_VALUE) CloseHandle(process); }
  void CloseThread() { if (thread != INVALID_HANDLE_VALUE) CloseHandle(thread); thread = INVALID_HANDLE_VALUE; }
  bool OpenThread() {
    thread = Connect();
    Bytes hello;
    hello.Put<uint32_t>(wire::kHelloThread); hello.Put(wire::kMagic); hello.Put(wire::kVersion);
    hello.Put(id); hello.Put(key); hello.Put<uint32_t>(GetCurrentThreadId());
    return thread != INVALID_HANDLE_VALUE && Send(thread, hello) && Status(thread);
  }
  bool Open() {
    process = Connect();
    Bytes hello, reply;
    hello.Put<uint32_t>(wire::kHelloProc); hello.Put(wire::kMagic); hello.Put(wire::kVersion);
    hello.Put<uint32_t>(GetCurrentProcessId()); hello.Put<uint32_t>(10000);
    int32_t status;
    return process != INVALID_HANDLE_VALUE && Send(process, hello) && Receive(process, &reply) &&
        reply.Get(&status) && status == 0 && reply.Get(&id) && reply.Get(&key) && OpenThread();
  }
  bool Ioctl(uint32_t request) {
    Bytes message;
    message.Put<uint32_t>(wire::kIoctl); message.Put(request);
    if (request == BINDER_SET_CONTEXT_MGR_EXT) {
      flat_binder_object object{}; object.flags = FLAT_BINDER_FLAG_ACCEPTS_FDS; message.Put(object);
    } else if (request == BINDER_SET_MAX_THREADS) message.Put<uint32_t>(0);
    return Send(thread, message) && Status(thread);
  }
  bool Buffer(uint32_t operation, uint64_t buffer, bool accepted = false, bool lose_ack = false) {
    Bytes message;
    message.Put(operation); message.Put(buffer);
    if (operation == wire::kFinishBuffer) message.Put<uint32_t>(accepted ? 1 : 0);
    return Send(thread, message) && (lose_ack ? PeekSuccessfulAck(thread) : Status(thread));
  }
  bool Read(Bytes* reply, bool enter = false) {
    Bytes message;
    message.Put<uint32_t>(wire::kWriteRead); message.Put<uint64_t>(enter ? 0 : 4096);
    message.Put<uint32_t>(1); message.Put<uint32_t>(1);
    if (enter) message.Put<uint32_t>(BC_ENTER_LOOPER);
    return Send(thread, message) && Receive(thread, reply);
  }
  bool Free(uint64_t id) {
    Bytes message, reply;
    message.Put<uint32_t>(wire::kWriteRead); message.Put<uint64_t>(0);
    message.Put<uint32_t>(1); message.Put<uint32_t>(1);
    message.Put<uint32_t>(BC_FREE_BUFFER); message.Put(id);
    int32_t status; uint32_t consumed;
    return Send(thread, message) && Receive(thread, &reply) && reply.Get(&status) && status == 0 &&
        reply.Get(&consumed) && consumed == 1 && reply.at == reply.data.size();
  }
};
struct Delivery {
  uint64_t buffer = 0;
  HANDLE staging = nullptr;
  uint32_t code = 0;
  int32_t sender_pid = 0;
};
bool Next(Peer* peer, Delivery* delivery) {
  for (unsigned i = 0; i < 1500; ++i) {
    Bytes reply;
    if (!peer->Read(&reply)) return false;
    int32_t status; uint32_t consumed;
    if (!reply.Get(&status) || !reply.Get(&consumed) || (status != 0 && status != -EAGAIN)) return false;
    while (reply.at < reply.data.size()) {
      uint32_t command;
      if (!reply.Get(&command)) return false;
      const size_t size = _IOC_SIZE(command);
      if (command != BR_TRANSACTION) {
        if (size > reply.data.size() - reply.at) return false;
        reply.at += size; continue;
      }
      binder_transaction_data tr; uint64_t extra;
      if (!reply.Get(&tr) || !reply.Get(&delivery->buffer) || !reply.Get(&extra)) return false;
      const uint64_t remaining = reply.data.size() - reply.at;
      if (tr.data_size > remaining || tr.offsets_size > remaining - tr.data_size ||
          extra > remaining - tr.data_size - tr.offsets_size || tr.offsets_size % 8) return false;
      const uint8_t* data = reply.data.data() + reply.at;
      const uint8_t* offsets = data + tr.data_size;
      for (size_t at = 0; at < tr.offsets_size; at += 8) {
        uint64_t offset; memcpy(&offset, offsets + at, 8);
        if (offset > tr.data_size || sizeof(binder_fd_object) > tr.data_size - offset) continue;
        binder_fd_object object; memcpy(&object, data + offset, sizeof(object));
        if (object.hdr.type == BINDER_TYPE_FD) delivery->staging = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(object.pad_binder));
      }
      delivery->code = tr.code;
      delivery->sender_pid = tr.sender_pid;
      return delivery->staging != nullptr;
    }
    Sleep(20);
  }
  return false;
}
bool IsLive(HANDLE handle) { DWORD flags; return GetHandleInformation(handle, &flags) != 0; }
bool WaitClosed(HANDLE handle) {
  for (unsigned i = 0; i < 500; ++i) { if (!IsLive(handle)) return true; Sleep(20); }
  return false;
}
bool ProveOwnedFile(HANDLE handle) {
  static const char expected[] = "sg-owned-file";
  LARGE_INTEGER zero{};
  if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN)) return false;
  char bytes[sizeof(expected)]{}; DWORD count = 0;
  return ReadFile(handle, bytes, sizeof(bytes), &count, nullptr) && count == sizeof(expected) &&
      memcmp(bytes, expected, sizeof(expected)) == 0;
}
bool RejectedDimensions(Peer* peer) {
  for (unsigned test = 0; test < 4; ++test) {
    Bytes message, reply;
    message.Put<uint32_t>(wire::kWriteRead); message.Put<uint64_t>(0);
    message.Put<uint32_t>(1); message.Put<uint32_t>(1); message.Put<uint32_t>(BC_TRANSACTION_SG);
    binder_transaction_data_sg sg{};
    if (test == 0) sg.transaction_data.data_size = UINT64_MAX;
    if (test == 1) { sg.transaction_data.data_size = 1; sg.transaction_data.offsets_size = (64u << 20); }
    if (test == 2) { sg.transaction_data.data_size = 136; sg.transaction_data.offsets_size = 24; sg.buffers_size = 120; } // absent body
    if (test == 3) sg.buffers_size = 1;
    message.Put(sg); message.Put<int32_t>(0);
    int32_t status; uint32_t consumed;
    const int32_t expected = test == 2 ? -EFAULT : -EINVAL;
    if (!Send(peer->thread, message) || !Receive(peer->thread, &reply) ||
        !reply.Get(&status) || status != expected || !reply.Get(&consumed) ||
        consumed != 0 || !peer->Ioctl(BINDER_SET_MAX_THREADS)) return false;
  }
  return true;
}
int Main(const char* scenario, const std::string& prefix) {
  Report report{"wire"};
  Peer peer;
  if (!report.Check(peer.Open() && peer.Ioctl(BINDER_SET_CONTEXT_MGR_EXT) && peer.Ioctl(BINDER_SET_MAX_THREADS), "independent raw receiver connects to actual broker")) return report.Finish();
  if (!report.Check(RejectedDimensions(&peer), "raw malicious dimensions rejected before allocation and next command works")) return report.Finish();
  Bytes enter;
  if (!report.Check(peer.Read(&enter, true) && binder_sg_fixture::Mark(prefix + ".ready"), "raw looper registered")) return report.Finish();
  if (!report.Check(binder_sg_fixture::WaitFile(prefix + ".release"), "delivery held until real sender snapshot")) return report.Finish();
  Delivery delivery;
  if (!report.Check(Next(&peer, &delivery) && IsLive(delivery.staging), "broker supplies actual live FD staging handle")) return report.Finish();
  report.Check(delivery.sender_pid > 0 && static_cast<DWORD>(delivery.sender_pid) != GetCurrentProcessId(),
      "actual SG sender PID differs from raw receiver PID");
  const bool before = !strcmp(scenario, "wire-beforeclaim");
  const bool death_before = !strcmp(scenario, "wire-death-beforeclaim");
  const bool death_claimed = !strcmp(scenario, "wire-death-afterclaim");
  const bool death_installed = !strcmp(scenario, "wire-death-afterinstall");
  const bool death_finished = !strcmp(scenario, "wire-death-afterfinish");
  const bool claim_loss = !strcmp(scenario, "wire-claimloss");
  const bool finish_loss = !strcmp(scenario, "wire-finishloss");
  const bool queue = !strcmp(scenario, "wire-queue");
  HANDLE owned = nullptr;
  auto die = [&](const char* boundary) {
    report.Check(binder_sg_fixture::Mark(prefix + ".death"), boundary);
    // Abrupt real process exit deliberately skips all Peer/FD destructors.
    _exit(report.Finish());
  };
  if (death_before) die("receiver dies with actual unclaimed pending delivery");
  if (!before) {
    if (!report.Check(peer.Buffer(wire::kClaimBuffer, delivery.buffer, false, claim_loss),
        claim_loss ? "exact length4 status0 claim ACK produced but never consumed" : "claim acknowledged without ownership transfer")) return report.Finish();
    if (!claim_loss) {
      if (death_claimed) die("receiver dies after actual claim success before adoption");
      if (!report.Check(DuplicateHandle(GetCurrentProcess(), delivery.staging, GetCurrentProcess(), &owned,
          0, FALSE, DUPLICATE_SAME_ACCESS) && owned != delivery.staging && ProveOwnedFile(owned), "distinct adopted copy reads sender file identity")) return report.Finish();
      if (death_installed) die("receiver dies with actual separately adopted file copy");
      if (death_finished) {
        report.Check(peer.Buffer(wire::kFinishBuffer, delivery.buffer, true), "actual finish accepted before receiver death");
        die("receiver dies after actual committed finish");
      }
      if (finish_loss) {
        if (!report.Check(peer.Buffer(wire::kFinishBuffer, delivery.buffer, true, true),
            "exact successful finish ACK produced but never consumed after local install")) return report.Finish();
      } else if (!report.Check(peer.Buffer(wire::kFinishBuffer, delivery.buffer, false), "installed-copy abort acknowledged")) return report.Finish();
    }
  }
  peer.CloseThread();
  report.Check(WaitClosed(delivery.staging), "disconnect/finish closes broker staging exactly once");
  if (owned) {
    report.Check(IsLive(owned) && ProveOwnedFile(owned), "caller-owned copy survives lost ACK and broker rollback");
    if (!report.Check(peer.OpenThread() && peer.Free(delivery.buffer), "fresh receiver thread can free old delivery after disconnect")) return report.Finish();
    report.Check(IsLive(owned) && ProveOwnedFile(owned), "real buffer free never closes distinct adopted descriptor");
    CloseHandle(owned);
    HANDLE reused = nullptr;
    std::vector<HANDLE> held;
    for (unsigned i = 0; i < 4096 && !reused; ++i) {
      HANDLE created = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!created) break;
      held.push_back(created);
      if (created == owned) reused = created;
    }
    report.Check(reused != nullptr, "actual owned-handle number reused by distinct receiver event");
    report.Check(peer.Free(delivery.buffer) && reused && SetEvent(reused) && WaitForSingleObject(reused, 0) == WAIT_OBJECT_0,
        "repeated old-buffer cleanup leaves actual reused receiver handle intact");
    for (HANDLE handle : held) CloseHandle(handle);
    peer.CloseThread();
  }
  if (queue) {
    if (!report.Check(peer.OpenThread(), "receiver reconnects after aborted first one-way")) return report.Finish();
    Bytes reply;
    if (!report.Check(peer.Read(&reply, true), "replacement actual looper enters")) return report.Finish();
    Delivery next;
    if (!report.Check(Next(&peer, &next) && next.code == 2 && next.buffer != delivery.buffer,
        "second one-way progresses after first pending delivery abort")) return report.Finish();
    report.Check(peer.Buffer(wire::kClaimBuffer, next.buffer) && peer.Buffer(wire::kFinishBuffer, next.buffer, false),
        "second one-way cleans its actual buffer and staging");
    report.Check(WaitClosed(next.staging), "queued second FD staging closes");
  }
  return report.Finish();
}
}  // namespace
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 4 || strcmp(argv[1], "server")) return 2;
  return Main(argv[2], argv[3]);
}
