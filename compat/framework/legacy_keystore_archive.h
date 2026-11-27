// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace gesher::keystore {

// Called by the trusted host before application code. No key bytes cross JNI.
// archive_root is the device's host-private system directory, binding a host
// derived 64-hex package/installation/data-area identifier. Returns Win32 status.
uint32_t ArchiveLegacy(const std::u16string& data_directory,
                       const std::u16string& archive_root,
                       const std::u16string& binding);

}  // namespace gesher::keystore
