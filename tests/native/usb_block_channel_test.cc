// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_broker_client.h"
#include "block_channel.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace gesher::usb_storage;

struct CloseHandleOwner {
  void operator()(void* handle) const { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleOwner>;

struct LocalSession {
  std::unique_ptr<BlockChannel> channel;
  BlockGuestHandles guest;

  bool Create() {
    Handle self(OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, GetCurrentProcessId()));
    return self && BlockChannel::Create(&channel) == ERROR_SUCCESS &&
        channel->Handoff(self.get(), &guest) == ERROR_SUCCESS;
  }

  ~LocalSession() {
    if (guest.revoked) CloseHandle(guest.revoked);
    if (guest.gate) CloseHandle(guest.gate);
    if (guest.pipe) CloseHandle(guest.pipe);
  }
};

bool Transfer(HANDLE pipe, bool write, void* buffer, DWORD count) {
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent) return false;
  DWORD done = 0;
  BOOL ok = write ? WriteFile(pipe, buffer, count, nullptr, &operation)
                  : ReadFile(pipe, buffer, count, nullptr, &operation);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(operation.hEvent, 3000) != WAIT_OBJECT_0) {
      CancelIoEx(pipe, &operation);
      GetOverlappedResult(pipe, &operation, &done, TRUE);
    } else {
      ok = TRUE;
    }
  }
  if (ok) ok = GetOverlappedResult(pipe, &operation, &done, FALSE);
  CloseHandle(operation.hEvent);
  return ok && done == count;
}

bool TestDescribe() {
  LocalSession session;
  if (!session.Create()) return false;
  auto& channel = *session.channel;
  auto& guest = session.guest;
  for (const HANDLE handle : {channel.server(), channel.revocation(), guest.pipe, guest.gate, guest.revoked}) {
    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT) != 0) return false;
  }
  DWORD result = ERROR_INVALID_DATA;
  ClientDiskInfo details;
  std::thread client([&] {
    BlockClient connection(std::exchange(guest.pipe, nullptr), guest.gate, guest.revoked);
    result = connection.Describe(&details);
  });
  BlockRequest request{};
  BlockReply reply{ERROR_SUCCESS, sizeof(BlockDescription) + 3};
  BlockDescription desc{16384, 512, kBlockRemovable, 2, 1};
  char names[] = "mk1";
  const bool ok = Transfer(channel.server(), false, &request, sizeof(request)) &&
      request.version == kBlockProtocolVersion && request.operation == BlockOperation::kDescribe &&
      request.length == 0 && request.reserved == 0 && request.offset == 0 &&
      Transfer(channel.server(), true, &reply, sizeof(reply)) &&
      Transfer(channel.server(), true, &desc, sizeof(desc)) &&
      Transfer(channel.server(), true, names, 3);
  if (!ok) channel.Revoke();
  client.join();
  return ok && result == ERROR_SUCCESS && details.disk.bytes == 16384 &&
      details.disk.sector_bytes == 512 && details.model == "mk" && details.serial == "1";
}

bool TestRevoke() {
  LocalSession session;
  if (!session.Create()) return false;
  auto& guest = session.guest;
  BlockClient client(std::exchange(guest.pipe, nullptr), guest.gate, guest.revoked);
  std::array<std::byte, 512> buffer{};
  if (session.channel->Revoke() != ERROR_SUCCESS) return false;
  return client.Read(0, buffer) == ERROR_OPERATION_ABORTED;
}

bool TestBlockedRevoke() {
  LocalSession session;
  if (!session.Create()) return false;
  auto& channel = *session.channel;
  auto& guest = session.guest;
  DWORD result = ERROR_SUCCESS;
  std::thread client([&] {
    BlockClient connection(std::exchange(guest.pipe, nullptr), guest.gate, guest.revoked);
    std::array<std::byte, 512> buffer{};
    result = connection.Read(0, buffer);
  });
  BlockRequest request{};
  const bool ok = Transfer(channel.server(), false, &request, sizeof(request)) &&
      request.operation == BlockOperation::kRead;
  const bool signaled = channel.Revoke() == ERROR_SUCCESS;
  client.join();
  return ok && signaled && result == ERROR_OPERATION_ABORTED;
}

bool TestBadFrame(bool describe) {
  LocalSession session;
  if (!session.Create()) return false;
  auto& channel = *session.channel;
  auto& guest = session.guest;
  BlockClient connection(std::exchange(guest.pipe, nullptr), guest.gate, guest.revoked);
  DWORD result = ERROR_SUCCESS;
  std::thread client([&] {
    ClientDiskInfo info;
    std::array<std::byte, 512> buffer{};
    result = describe ? connection.Describe(&info) : connection.Read(0, buffer);
  });
  BlockRequest request{};
  BlockReply bad{ERROR_SUCCESS, describe ? static_cast<DWORD>(sizeof(BlockDescription)) : kBlockTransferLimit + 1};
  BlockDescription invalid{16384, 511, kBlockRemovable, 0, 0};
  const bool ok = Transfer(channel.server(), false, &request, sizeof(request)) &&
      request.operation == (describe ? BlockOperation::kDescribe : BlockOperation::kRead) &&
      Transfer(channel.server(), true, &bad, sizeof(bad)) &&
      (!describe || Transfer(channel.server(), true, &invalid, sizeof(invalid)));
  if (!ok) channel.Revoke();
  client.join();
  DWORD available = 0;
  const BOOL peek = PeekNamedPipe(channel.server(), nullptr, 0, nullptr, &available, nullptr);
  const DWORD peek_error = GetLastError();
  std::array<std::byte, 512> buffer{};
  return ok && result == ERROR_INVALID_DATA && !connection.Live() &&
      connection.Read(0, buffer) == ERROR_OPERATION_ABORTED &&
      !peek && peek_error == ERROR_BROKEN_PIPE &&
      WaitForSingleObject(channel.revocation(), 0) == WAIT_TIMEOUT;
}

bool TestAbandonedGate() {
  LocalSession session;
  if (!session.Create()) return false;
  DWORD acquired = WAIT_FAILED;
  std::thread abandoned([&] { acquired = WaitForSingleObject(session.guest.gate, 1000); });
  abandoned.join();
  if (acquired != WAIT_OBJECT_0) return false;
  auto& guest = session.guest;
  BlockClient client(std::exchange(guest.pipe, nullptr), guest.gate, guest.revoked);
  std::array<std::byte, 512> data{};
  return client.Read(0, data) == ERROR_OPERATION_ABORTED && !client.Live();
}

bool DuplicateDenied(HANDLE handle, DWORD access) {
  const HANDLE self = GetCurrentProcess();
  HANDLE copy = nullptr;
  const bool duplicated = DuplicateHandle(self, handle, self, &copy, access, FALSE, 0);
  if (duplicated) CloseHandle(copy);
  return !duplicated && GetLastError() == ERROR_ACCESS_DENIED;
}

int RunGuest(HANDLE bootstrap) {
  BlockGuestHandles guest;
  DWORD bytes = 0;
  if (!ReadFile(bootstrap, &guest, sizeof(guest), &bytes, nullptr) || bytes != sizeof(guest)) return 10;
  CloseHandle(bootstrap);
  for (HANDLE handle : {guest.pipe, guest.gate, guest.revoked}) {
    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT)) return 11;
  }
  if (SetEvent(guest.revoked) || GetLastError() != ERROR_ACCESS_DENIED) return 12;
  if (ResetEvent(guest.revoked) || GetLastError() != ERROR_ACCESS_DENIED) return 13;
  if (!DuplicateDenied(guest.revoked, EVENT_MODIFY_STATE) ||
      !DuplicateDenied(guest.revoked, WRITE_DAC) || !DuplicateDenied(guest.gate, WRITE_DAC)) return 19;
  if (WaitForSingleObject(guest.revoked, 0) != WAIT_TIMEOUT) return 14;
  BlockClient client(guest.pipe, guest.gate, guest.revoked);
  ClientDiskInfo info;
  if (client.Describe(&info) != ERROR_SUCCESS || info.disk.bytes != 8192) return 15;
  std::array<std::byte, 512> data{};
  if (client.Read(0, data) != ERROR_OPERATION_ABORTED) return 16;
  if (ResetEvent(guest.revoked) || GetLastError() != ERROR_ACCESS_DENIED ||
      WaitForSingleObject(guest.revoked, 0) != WAIT_OBJECT_0 || client.Live()) return 17;
  return client.Write(0, data) == ERROR_OPERATION_ABORTED ? 0 : 18;
}

bool SpawnGuest(HANDLE bootstrap, PROCESS_INFORMATION* child) {
  wchar_t executable[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (!length || length >= MAX_PATH) return false;
  std::wstring command = std::wstring(L"\"") + executable + L"\" " +
      std::to_wstring(reinterpret_cast<uintptr_t>(bootstrap));
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  if (!bytes) return false;
  std::vector<std::byte> storage(bytes);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) return false;
  const bool created = UpdateProcThreadAttribute(startup.lpAttributeList, 0,
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &bootstrap, sizeof(bootstrap), nullptr, nullptr) &&
      CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
          EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
          nullptr, nullptr, &startup.StartupInfo, child);
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  return created;
}

bool TestCrossProcess() {
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return false;
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE input = nullptr, output = nullptr;
  if (!CreatePipe(&input, &output, &inherit, 0)) return false;
  Handle reader(input), writer(output);
  if (!SetHandleInformation(writer.get(), HANDLE_FLAG_INHERIT, 0)) return false;
  PROCESS_INFORMATION child{};
  if (!SpawnGuest(reader.get(), &child)) return false;
  Handle process(child.hProcess), thread(child.hThread);
  reader.reset();

  const bool ok = [&] {
    Handle limited(OpenProcess(SYNCHRONIZE, FALSE, child.dwProcessId));
    BlockGuestHandles guest;
    if (!limited || channel->Handoff(limited.get(), &guest) != ERROR_ACCESS_DENIED ||
        guest.pipe || guest.gate || guest.revoked) return false;
    if (channel->Handoff(process.get(), &guest) != ERROR_SUCCESS) return false;
    BlockGuestHandles repeat;
    if (channel->Handoff(process.get(), &repeat) != ERROR_ALREADY_ASSIGNED ||
        repeat.pipe || repeat.gate || repeat.revoked) return false;
    DWORD bytes = 0;
    if (!WriteFile(writer.get(), &guest, sizeof(guest), &bytes, nullptr) ||
        bytes != sizeof(guest) || ResumeThread(thread.get()) != 1) return false;
    writer.reset();

    BlockRequest request{};
    BlockReply reply{ERROR_SUCCESS, sizeof(BlockDescription)};
    BlockDescription desc{8192, 4096, kBlockRemovable, 0, 0};
    const HANDLE pipe = channel->server();
    return Transfer(pipe, false, &request, sizeof(request)) &&
        request.operation == BlockOperation::kDescribe &&
        Transfer(pipe, true, &reply, sizeof(reply)) &&
        Transfer(pipe, true, &desc, sizeof(desc)) &&
        Transfer(pipe, false, &request, sizeof(request)) &&
        request.operation == BlockOperation::kRead && request.length == 512 &&
        channel->Revoke() == ERROR_SUCCESS;
  }();
  if (!ok || WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
    channel->Revoke();
    TerminateProcess(process.get(), 1);
    WaitForSingleObject(process.get(), 5000);
    return false;
  }
  DWORD code = 0;
  if (!GetExitCodeProcess(process.get(), &code) || code != 0) {
    std::fprintf(stderr, "USB block guest exited with code %lu\n", code);
    return false;
  }
  std::unique_ptr<BlockChannel> fresh;
  BlockGuestHandles vanished;
  return BlockChannel::Create(&fresh) == ERROR_SUCCESS &&
      fresh->Handoff(process.get(), &vanished) == ERROR_PROCESS_ABORTED &&
      !vanished.pipe && !vanished.gate && !vanished.revoked;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2) {
    char* end = nullptr;
    const uint64_t value = std::strtoull(argv[1], &end, 10);
    if (!value || value > UINTPTR_MAX || !end || *end) return 2;
    return RunGuest(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)));
  }
  if (argc != 1) return 2;
  const bool describe = TestDescribe();
  const bool revoke = TestRevoke();
  const bool blocked_revoke = TestBlockedRevoke();
  const bool bad_frame = TestBadFrame(false);
  const bool bad_describe = TestBadFrame(true);
  const bool abandoned = TestAbandonedGate();
  const bool cross_process = TestCrossProcess();
  std::printf("USB block channel: describe=%s revoke=%s blocked-revoke=%s bad-frame=%s "
              "bad-describe=%s abandoned=%s cross-process=%s\n",
      describe ? "pass" : "fail", revoke ? "pass" : "fail",
      blocked_revoke ? "pass" : "fail", bad_frame ? "pass" : "fail",
      bad_describe ? "pass" : "fail", abandoned ? "pass" : "fail", cross_process ? "pass" : "fail");
  return describe && revoke && blocked_revoke && bad_frame && bad_describe &&
      abandoned && cross_process ? 0 : 1;
}
