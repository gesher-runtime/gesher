// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "../devsvc/usb_storage_observation.h"

namespace gesher::usb_storage {

enum class UsbObservationKind : uint32_t { Paths = 1, Inspect = 2, Version = 3, Available = 4 };
constexpr size_t kUsbObservationLimit = 1024 * 1024;
constexpr size_t kUsbObservationMaxPaths = 128;
constexpr size_t kUsbObservationPathUnits = 32767;

// Pure metadata: no disk, hub, USB endpoint or other transferable capability.
// Only the field corresponding to the requested kind is encoded.
struct UsbObservationData {
  std::vector<std::wstring> paths;
  usb::StorageObservation storage;
  int64_t version = -1;
  bool available = false;
};

// Explicit little-endian, length-delimited format. Failure leaves output empty
// or the decoded destination unchanged; malformed/trailing data is rejected.
uint32_t EncodeUsbObservation(UsbObservationKind kind, const UsbObservationData& value,
                              std::vector<std::byte>* output);
bool DecodeUsbObservation(UsbObservationKind kind, std::span<const std::byte> bytes,
                          UsbObservationData* value);

// Called only inside the privileged authority after exact system_server peer
// authentication. Inspect/Version require one currently enumerated USBSTOR or
// UASP storage interface; Paths/Available require an empty path. The authority
// sends these bytes through a read-only snapshot, never an opened device handle.
uint32_t QueryProtectedUsbObservation(UsbObservationKind kind, std::wstring_view path,
                                      std::vector<std::byte>* output);

}  // namespace gesher::usb_storage
