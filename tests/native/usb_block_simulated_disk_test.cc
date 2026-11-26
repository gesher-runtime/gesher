// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Link the real Windows broker, client and channel. Only HostDisk is replaced
// in this executable, which never opens a physical disk.
#include "block_broker_client.h"
#include "block_broker_server.h"
#include "block_channel.h"
#include "host_disk.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>
#include <utility>

namespace {
constexpr size_t kSectorBytes = 512;
constexpr size_t kDiskBytes = 8 * kSectorBytes;
std::array<std::byte, kDiskBytes> media{};
std::atomic<bool> media_changed{false};
}

namespace gesher::usb_storage {
DWORD HostDisk::Open(const std::wstring& path, Access access, std::unique_ptr<HostDisk>* output) {
  if (path != L"simulated-test-disk" || !output || *output) return ERROR_INVALID_PARAMETER;
  auto disk = std::unique_ptr<HostDisk>(new HostDisk(INVALID_HANDLE_VALUE));
  const bool writable = access == Access::kOfflineReadWrite;
  disk->info_ = {kDiskBytes, kSectorBytes, true, writable, writable, "gesher-test-disk", "test-1"};
  *output = std::move(disk);
  return ERROR_SUCCESS;
}

HostDisk::~HostDisk() = default;

DWORD HostDisk::Validate() { return media_changed ? ERROR_MEDIA_CHANGED : ERROR_SUCCESS; }

DWORD HostDisk::Describe(DiskInfo* info) {
  const DWORD error = Validate();
  if (error == ERROR_SUCCESS) *info = info_;
  return error;
}

DWORD HostDisk::SetReadOnly(bool readonly) {
  const DWORD error = Validate();
  if (error != ERROR_SUCCESS) return error;
  if (!readonly && !info_.write_consented) return ERROR_ACCESS_DENIED;
  info_.writable = !readonly;
  return ERROR_SUCCESS;
}

DWORD HostDisk::Flush() { return Validate(); }

bool HostDisk::ValidRange(uint64_t offset, size_t size) const {
  return offset <= media.size() && size <= media.size() - offset &&
      offset % kSectorBytes == 0 && size % kSectorBytes == 0;
}

DWORD HostDisk::Read(uint64_t offset, std::span<std::byte> output) {
  const DWORD error = Validate();
  if (error != ERROR_SUCCESS) return error;
  if (!ValidRange(offset, output.size())) return ERROR_INVALID_PARAMETER;
  std::copy_n(media.begin() + offset, output.size(), output.begin());
  return ERROR_SUCCESS;
}

DWORD HostDisk::Write(uint64_t offset, std::span<const std::byte> input) {
  const DWORD error = Validate();
  if (error != ERROR_SUCCESS) return error;
  if (!info_.writable) return ERROR_WRITE_PROTECT;
  if (!ValidRange(offset, input.size())) return ERROR_INVALID_PARAMETER;
  std::copy(input.begin(), input.end(), media.begin() + offset);
  return ERROR_SUCCESS;
}
}  // namespace gesher::usb_storage

namespace {
using namespace gesher::usb_storage;

bool Check(bool result, const char* context) {
  if (!result) std::fprintf(stderr, "simulated USB disk: %s\n", context);
  return result;
}

bool Run(Access access) {
  media.fill(std::byte{0x27});
  media_changed = false;
  std::unique_ptr<HostDisk> disk;
  std::unique_ptr<BlockChannel> channel;
  if (!Check(HostDisk::Open(L"simulated-test-disk", access, &disk) == ERROR_SUCCESS &&
             BlockChannel::Create(&channel) == ERROR_SUCCESS, "create session")) return false;

  BlockGuestHandles guest{};
  const HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, GetCurrentProcessId());
  if (!Check(process != nullptr, "open local test process")) return false;
  const DWORD handed_off = channel->Handoff(process, &guest);
  CloseHandle(process);
  if (!Check(handed_off == ERROR_SUCCESS, "handoff")) return false;
  DWORD broker_result = ERROR_SUCCESS;
  std::thread broker([&] { broker_result = ServeBlockRequests(*channel, *disk, {}); });
  const bool writable = access == Access::kOfflineReadWrite;
  const bool valid = [&] {
    BlockClient client(guest.pipe, guest.gate, guest.revoked);
    std::array<std::byte, kSectorBytes> given{};
    given.fill(std::byte{0xfe});
    std::array<std::byte, kSectorBytes> observed{};
    ClientDiskInfo info;
    if (!Check(client.Describe(&info) == ERROR_SUCCESS && info.disk.bytes == kDiskBytes &&
                   info.disk.sector_bytes == kSectorBytes && info.model == "gesher-test-disk" &&
                   ((info.disk.flags & kBlockWriteConsent) != 0) == writable,
               "describe simulated media") ||
        !Check(client.Read(0, observed) == ERROR_SUCCESS && observed.front() == std::byte{0x27},
               "read existing sector") ||
        !Check(client.Write(kSectorBytes, given) == (writable ? ERROR_SUCCESS : ERROR_WRITE_PROTECT),
               "enforce write consent") ||
        !Check(client.Read(kSectorBytes, observed) == ERROR_SUCCESS &&
                   (writable ? given == observed : observed.front() == std::byte{0x27}),
               "read after write attempt") ||
        !Check(client.Read(kDiskBytes, observed) == ERROR_INVALID_PARAMETER, "reject outside disk") ||
        !Check(client.SetReadOnly(true) == ERROR_SUCCESS &&
                   client.Write(kSectorBytes, given) == ERROR_WRITE_PROTECT &&
                   client.SetReadOnly(false) == (writable ? ERROR_SUCCESS : ERROR_ACCESS_DENIED),
               "preserve write consent across mode changes") ||
        !Check(client.Flush() == ERROR_SUCCESS, "flush")) return false;
    media_changed = true;
    return Check(client.Read(0, observed) != ERROR_SUCCESS &&
                     client.Read(0, observed) != ERROR_SUCCESS,
                 "revoke on media replacement");
  }();
  if (!valid) channel->Revoke();
  broker.join();
  CloseHandle(guest.gate);
  CloseHandle(guest.revoked);
  return Check(valid && broker_result == ERROR_MEDIA_CHANGED, "broker retired old medium");
}
}  // namespace

int main() {
  if (!Run(Access::kReadOnly) || !Run(Access::kOfflineReadWrite)) return 1;
  std::puts("Simulated USB disk passed over the real Windows block channel");
}
