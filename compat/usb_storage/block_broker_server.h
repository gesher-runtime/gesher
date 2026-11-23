// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace gesher::usb_storage {

class HostDisk;
class BlockChannel;

// Serve one authorized channel until terminal error or revocation.
// The caller retains the channel and disk until this function returns.
// Revocation cancels pipe I/O. The caller joins this worker before freeing
// the disk, since a physical drive operation already in progress may finish.
DWORD ServeBlockRequests(BlockChannel& channel, HostDisk& disk);

}  // namespace gesher::usb_storage
