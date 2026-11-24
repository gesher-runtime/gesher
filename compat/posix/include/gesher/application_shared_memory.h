// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

namespace gesher {

// Called by ApplicationSharedMemory.nativeCreate with its actual new region.
// Ordinary Android startup requires no authority and is unchanged. Protected
// publication authenticates the exact producer through the Binder endpoint.
int PublishApplicationSharedMemory(int fd, size_t size);

// A kernel read-only duplicate of the same object, never a filename reopen or
// a shared, writable file-description mapping. Both APIs report errno on error.
int DuplicateApplicationSharedMemoryReadOnly(int fd);
bool UsesProtectedBinder();

}  // namespace gesher
