// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_broker_client.h"
#include "block_channel.h"

#include <array>
#include <cstdio>
#include <thread>

namespace {

using namespace gesher::usb_storage;

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
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return false;
  for (const HANDLE handle : {channel->server, channel->client, channel->gate, channel->shutdown}) {
    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags) || (flags & HANDLE_FLAG_INHERIT) != 0) return false;
  }
  DWORD result = ERROR_INVALID_DATA;
  ClientDiskInfo details;
  std::thread client([&] {
    BlockClient connection(channel->client, channel->gate, channel->shutdown);
    result = connection.Describe(&details);
  });
  BlockRequest request{};
  BlockReply reply{ERROR_SUCCESS, sizeof(BlockDescription) + 3};
  BlockDescription desc{16384, 512, kBlockRemovable, 2, 1};
  char names[] = "mk1";
  const bool ok = Transfer(channel->server, false, &request, sizeof(request)) &&
      request.version == kBlockProtocolVersion && request.operation == BlockOperation::kDescribe &&
      request.length == 0 && request.reserved == 0 && request.offset == 0 &&
      Transfer(channel->server, true, &reply, sizeof(reply)) &&
      Transfer(channel->server, true, &desc, sizeof(desc)) &&
      Transfer(channel->server, true, names, 3);
  if (!ok) SetEvent(channel->shutdown);
  client.join();
  return ok && result == ERROR_SUCCESS && details.disk.bytes == 16384 &&
      details.disk.sector_bytes == 512 && details.model == "mk" && details.serial == "1";
}

bool TestRevoke() {
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return false;
  BlockClient client(channel->client, channel->gate, channel->shutdown);
  std::array<std::byte, 512> buffer{};
  if (!SetEvent(channel->shutdown)) return false;
  return client.Read(0, buffer) == ERROR_OPERATION_ABORTED;
}

bool TestBlockedRevoke() {
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return false;
  DWORD result = ERROR_SUCCESS;
  std::thread client([&] {
    BlockClient connection(channel->client, channel->gate, channel->shutdown);
    std::array<std::byte, 512> buffer{};
    result = connection.Read(0, buffer);
  });
  BlockRequest request{};
  const bool ok = Transfer(channel->server, false, &request, sizeof(request)) &&
      request.operation == BlockOperation::kRead;
  const BOOL signaled = SetEvent(channel->shutdown);
  client.join();
  return ok && signaled && result == ERROR_OPERATION_ABORTED;
}

bool TestBadFrame() {
  std::unique_ptr<BlockChannel> channel;
  if (BlockChannel::Create(&channel) != ERROR_SUCCESS) return false;
  DWORD result = ERROR_SUCCESS;
  std::thread client([&] {
    BlockClient connection(channel->client, channel->gate, channel->shutdown);
    std::array<std::byte, 512> buffer{};
    result = connection.Read(0, buffer);
  });
  BlockRequest request{};
  BlockReply bad{ERROR_SUCCESS, kBlockTransferLimit + 1};
  const bool ok = Transfer(channel->server, false, &request, sizeof(request)) &&
      request.operation == BlockOperation::kRead &&
      Transfer(channel->server, true, &bad, sizeof(bad));
  if (!ok) SetEvent(channel->shutdown);
  client.join();
  return ok && result == ERROR_INVALID_DATA &&
      WaitForSingleObject(channel->shutdown, 0) == WAIT_OBJECT_0;
}

}  // namespace

int main() {
  const bool describe = TestDescribe();
  const bool revoke = TestRevoke();
  const bool blocked_revoke = TestBlockedRevoke();
  const bool bad_frame = TestBadFrame();
  std::printf("USB block channel: describe=%s revoke=%s blocked-revoke=%s bad-frame=%s\n",
      describe ? "pass" : "fail", revoke ? "pass" : "fail",
      blocked_revoke ? "pass" : "fail", bad_frame ? "pass" : "fail");
  return describe && revoke && blocked_revoke && bad_frame ? 0 : 1;
}
