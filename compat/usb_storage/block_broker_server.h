// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <functional>

namespace gesher::usb_storage {

class HostDisk;
class BlockChannel;

// Serve one authorized channel until terminal error or revocation.
// The caller retains the channel and disk until this function returns.
// Revocation cancels pipe I/O. The caller joins this worker before freeing
// the disk, since a physical drive operation already in progress may finish.
// The authority callback checks the retained process and grant deadline before
// every operation. Revocation still interrupts idle pipe I/O independently.
DWORD ServeBlockRequests(BlockChannel& channel, HostDisk& disk,
                         const std::function<DWORD()>& authorize = {});

}  // namespace gesher::usb_storage
