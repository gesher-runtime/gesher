// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gesher::usb {

// Observations use the USB hub and its exact PnP child, independently of any
// disk, volume, or WinUSB data handle.
struct StorageObservation {
  std::vector<unsigned char> descriptors;
  std::map<unsigned char, std::wstring> strings;
};

std::optional<std::vector<std::wstring>> StorageInterfacePaths();
std::optional<StorageObservation> ObserveStorage(const std::wstring& path);

}  // namespace gesher::usb
