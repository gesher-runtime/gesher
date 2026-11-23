// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0

#include "block_broker_client.h"

#include <array>
#include <cstring>
#include <memory>

namespace gesher::usb_storage {
namespace {

constexpr DWORD kTimeoutMs = 30000;

struct CloseEvent {
  void operator()(void* event) const { CloseHandle(event); }
};

struct Gate {
  HANDLE mutex;
  ~Gate() { ReleaseMutex(mutex); }
};

DWORD Transfer(HANDLE pipe, HANDLE shutdown, HANDLE done,
               void* bytes, DWORD size, bool writing) {
  auto* cursor = static_cast<std::byte*>(bytes);
  while (size != 0) {
    OVERLAPPED pending{};
    pending.hEvent = done;
    if (!ResetEvent(done)) return GetLastError();
    DWORD transferred = 0;
    BOOL started = writing ? WriteFile(pipe, cursor, size, &transferred, &pending)
                           : ReadFile(pipe, cursor, size, &transferred, &pending);
    if (!started) {
      DWORD error = GetLastError();
      if (error != ERROR_IO_PENDING) return error;
      HANDLE events[] = {shutdown, done};
      const DWORD result = WaitForMultipleObjects(2, events, FALSE, kTimeoutMs);
      if (result != WAIT_OBJECT_0 + 1) {
        error = result == WAIT_FAILED ? GetLastError() :
                result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
        CancelIoEx(pipe, &pending);
        GetOverlappedResult(pipe, &pending, &transferred, TRUE);
        return error;
      }
      if (!GetOverlappedResult(pipe, &pending, &transferred, FALSE)) return GetLastError();
    }
    if (transferred == 0 || transferred > size) return ERROR_BROKEN_PIPE;
    size -= transferred;
    cursor += transferred;
  }
  return ERROR_SUCCESS;
}

}  // namespace

DWORD BlockClient::Exchange(BlockRequest request, std::span<const std::byte> input,
                            std::span<std::byte> output, DWORD* size) {
  HANDLE handles[] = {shutdown_, gate_};
  const DWORD result = WaitForMultipleObjects(2, handles, FALSE, kTimeoutMs);
  if (result == WAIT_OBJECT_0) return ERROR_OPERATION_ABORTED;
  if (result == WAIT_TIMEOUT) return ERROR_TIMEOUT;
  if (result == WAIT_FAILED) return GetLastError();
  if (result != WAIT_OBJECT_0 + 1 && result != WAIT_ABANDONED_0 + 1) return ERROR_INVALID_HANDLE;
  Gate gate{gate_};

  auto poison = [this](DWORD error) {
    SetEvent(shutdown_);
    return error;
  };
  if (result == WAIT_ABANDONED_0 + 1) return poison(ERROR_OPERATION_ABORTED);
  if (WaitForSingleObject(shutdown_, 0) != WAIT_TIMEOUT) return ERROR_OPERATION_ABORTED;

  std::unique_ptr<void, CloseEvent> done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!done) return GetLastError();
  DWORD error = Transfer(pipe_, shutdown_, done.get(), &request, sizeof(request), true);
  if (error != ERROR_SUCCESS) return poison(error);
  // WriteFile doesn't change the caller's bytes.
  error = Transfer(pipe_, shutdown_, done.get(), const_cast<std::byte*>(input.data()),
                   static_cast<DWORD>(input.size()), true);
  if (error != ERROR_SUCCESS) return poison(error);
  BlockReply reply{};
  error = Transfer(pipe_, shutdown_, done.get(), &reply, sizeof(reply), false);
  if (error != ERROR_SUCCESS) return poison(error);
  if (reply.error != ERROR_SUCCESS) {
    return reply.length == 0 ? reply.error : poison(ERROR_INVALID_DATA);
  }
  if (reply.length > output.size() ||
      (request.operation == BlockOperation::kRead && reply.length != output.size())) {
    return poison(ERROR_INVALID_DATA);
  }
  error = Transfer(pipe_, shutdown_, done.get(), output.data(), reply.length, false);
  if (error != ERROR_SUCCESS) return poison(error);
  if (size) *size = reply.length;
  return ERROR_SUCCESS;
}

DWORD BlockClient::Describe(ClientDiskInfo* info) {
  std::array<std::byte, sizeof(BlockDescription) + 2 * kMaxBlockDescriptionString> buffer;
  DWORD length = 0;
  DWORD error = Exchange({kBlockProtocolVersion, BlockOperation::kDescribe, 0, 0, 0},
                         {}, buffer, &length);
  if (error != ERROR_SUCCESS) return error;
  if (length < sizeof(BlockDescription)) return ERROR_INVALID_DATA;
  BlockDescription record{};
  std::memcpy(&record, buffer.data(), sizeof(record));
  if (record.model_bytes > kMaxBlockDescriptionString ||
      record.serial_bytes > kMaxBlockDescriptionString ||
      record.model_bytes + record.serial_bytes != length - sizeof(record) ||
      (record.flags & ~(kBlockWritable | kBlockRemovable)) != 0 ||
      record.sector_bytes < 512 || record.sector_bytes > 65536 ||
      (record.sector_bytes & (record.sector_bytes - 1)) != 0 ||
      record.bytes == 0 || record.bytes % record.sector_bytes != 0) {
    return ERROR_INVALID_DATA;
  }
  const char* text = reinterpret_cast<const char*>(buffer.data() + sizeof(record));
  info->disk = record;
  info->model.assign(text, record.model_bytes);
  info->serial.assign(text + record.model_bytes, record.serial_bytes);
  return ERROR_SUCCESS;
}

DWORD BlockClient::Read(uint64_t offset, std::span<std::byte> output) {
  if (output.empty() || output.size() > kBlockTransferLimit) return ERROR_INVALID_PARAMETER;
  return Exchange({kBlockProtocolVersion, BlockOperation::kRead,
                   static_cast<DWORD>(output.size()), 0, offset}, {}, output, nullptr);
}

DWORD BlockClient::Write(uint64_t offset, std::span<const std::byte> input) {
  if (input.empty() || input.size() > kBlockTransferLimit) return ERROR_INVALID_PARAMETER;
  return Exchange({kBlockProtocolVersion, BlockOperation::kWrite,
                   static_cast<DWORD>(input.size()), 0, offset}, input, {}, nullptr);
}

}  // namespace gesher::usb_storage
