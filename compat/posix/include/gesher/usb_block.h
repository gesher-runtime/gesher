// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

namespace gesher::usb_block {

// Called by the trusted guest launcher after admission, before its shell or
// application code runs. Duplicates all handles without making them inheritable.
// There is no environment or command-line route to this function.
int AttachBroker(HANDLE pipe, HANDLE mutex, HANDLE shutdown);

}  // namespace gesher::usb_block
