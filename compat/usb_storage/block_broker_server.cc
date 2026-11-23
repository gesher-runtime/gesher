// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_broker_server.h"

#include "block_broker.h"
#include "host_disk.h"

#include <cstddef>
#include <memory>
#include <new>
#include <span>

namespace gesher::usb_storage {
namespace {

static_assert(kBlockTransferLimit == kMaxTransfer);

struct CloseEvent {
  void operator()(void* event) const { CloseHandle(event); }
};

enum class PipeDirection { kRead, kWrite };

DWORD PipeTransfer(HANDLE pipe, HANDLE event, HANDLE shutdown, PipeDirection direction,
                   void* bytes, DWORD size) {
  auto* cursor = static_cast<std::byte*>(bytes);
  while (size != 0) {
    const DWORD revoked = WaitForSingleObject(shutdown, 0);
    if (revoked != WAIT_TIMEOUT) {
      return revoked == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError();
    }
    if (!ResetEvent(event)) return GetLastError();
    OVERLAPPED request{};
    request.hEvent = event;
    DWORD transferred = 0;
    const BOOL complete = direction == PipeDirection::kRead
        ? ReadFile(pipe, cursor, size, nullptr, &request)
        : WriteFile(pipe, cursor, size, nullptr, &request);
    if (!complete) {
      const DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) return error;
      const HANDLE waits[] = {shutdown, event};
      const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (result != WAIT_OBJECT_0 + 1) {
        const DWORD wait_error = result == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError();
        CancelIoEx(pipe, &request);
        // The OVERLAPPED and caller's buffer must remain alive after cancel.
        GetOverlappedResult(pipe, &request, &transferred, TRUE);
        return wait_error;
      }
    }
    if (!GetOverlappedResult(pipe, &request, &transferred, FALSE)) return GetLastError();
    if (transferred == 0 || transferred > size) return ERROR_BROKEN_PIPE;
    cursor += transferred;
    size -= transferred;
  }
  return ERROR_SUCCESS;
}

bool ValidRequest(const BlockRequest& request) {
  if (request.version != kBlockProtocolVersion || request.reserved != 0) return false;
  if (request.operation == BlockOperation::kDescribe) {
    return request.length == 0 && request.offset == 0;
  }
  return (request.operation == BlockOperation::kRead || request.operation == BlockOperation::kWrite)
      && request.length != 0 && request.length <= kBlockTransferLimit
      && request.offset <= UINT64_MAX - request.length;
}

}  // namespace

DWORD ServeBlockRequests(HANDLE pipe, HANDLE shutdown, HostDisk& disk) {
  const std::unique_ptr<void, CloseEvent> event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return GetLastError();
  auto receive = [&](void* buffer, DWORD size) {
    return PipeTransfer(pipe, event.get(), shutdown, PipeDirection::kRead, buffer, size);
  };
  auto send = [&](const void* buffer, DWORD size) {
    return PipeTransfer(pipe, event.get(), shutdown, PipeDirection::kWrite,
                        const_cast<void*>(buffer), size);
  };
  const DiskInfo& info = disk.info();
  if (info.model.size() > kMaxBlockDescriptionString || info.serial.size() > kMaxBlockDescriptionString) {
    return ERROR_INVALID_DATA;
  }
  const BlockDescription description{
      info.bytes, info.sector_bytes,
      (info.writable ? kBlockWritable : 0) | (info.removable ? kBlockRemovable : 0),
      static_cast<uint32_t>(info.model.size()), static_cast<uint32_t>(info.serial.size())};
  std::unique_ptr<std::byte[]> buffer(new (std::nothrow) std::byte[kBlockTransferLimit]);
  if (!buffer) return ERROR_NOT_ENOUGH_MEMORY;
  for (;;) {
    BlockRequest request{};
    DWORD error = receive(&request, sizeof(request));
    if (error != ERROR_SUCCESS) return error;
    if (!ValidRequest(request)) return ERROR_INVALID_DATA;
    if (request.operation == BlockOperation::kDescribe) {
      const DWORD active = disk.Validate();
      const BlockReply reply{active, active == ERROR_SUCCESS
          ? static_cast<uint32_t>(sizeof(description)) + description.model_bytes + description.serial_bytes
          : 0};
      error = send(&reply, sizeof(reply));
      if (active != ERROR_SUCCESS) return error == ERROR_SUCCESS ? active : error;
      if (error == ERROR_SUCCESS) error = send(&description, sizeof(description));
      if (error == ERROR_SUCCESS) error = send(info.model.data(), description.model_bytes);
      if (error == ERROR_SUCCESS) error = send(info.serial.data(), description.serial_bytes);
      if (error != ERROR_SUCCESS) return error;
      continue;
    }

    const std::span<std::byte> bytes(buffer.get(), request.length);
    if (request.operation == BlockOperation::kWrite) {
      error = receive(bytes.data(), request.length);
      if (error != ERROR_SUCCESS) return error;
    }
    const DWORD revoked = WaitForSingleObject(shutdown, 0);
    if (revoked != WAIT_TIMEOUT) {
      return revoked == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError();
    }
    const DWORD io_error = request.operation == BlockOperation::kRead
        ? disk.Read(request.offset, bytes)
        : disk.Write(request.offset, bytes);
    const uint32_t reply_length =
        io_error == ERROR_SUCCESS && request.operation == BlockOperation::kRead ? request.length : 0;
    const BlockReply reply{io_error, reply_length};
    error = send(&reply, sizeof(reply));
    if (error == ERROR_SUCCESS && reply_length != 0) error = send(bytes.data(), reply_length);
    if (error != ERROR_SUCCESS) return error;
  }
}

}  // namespace gesher::usb_storage
