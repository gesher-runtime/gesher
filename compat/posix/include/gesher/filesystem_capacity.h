// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <windows.h>

#include <gesher/rooted_storage_wire.h>

namespace gesher {

// Queries the volume holding an exact, borrowed disk file/directory handle.
// No directory or volume path is resolved or reopened. Returns positive errno.
int QueryFilesystemCapacity(HANDLE file, rooted_storage_wire::Filesystem* capacity);

}  // namespace gesher
