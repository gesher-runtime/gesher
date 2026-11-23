// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace gesher::usb_storage {

class HostDisk;

// Serve one authenticated stream session. The trusted caller holds the
// selected disk, an already-connected overlapped byte-mode pipe, and a
// shutdown event. The caller owns all three and waits for this function to
// return before releasing them or allowing another session to use the disk.
DWORD ServeBlockRequests(HANDLE pipe, HANDLE shutdown, HostDisk& disk);

}  // namespace gesher::usb_storage
