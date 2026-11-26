// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <gesher/native_fence.h>
#include <poll.h>

namespace gesher {
// Metadata occupies its own type bits, disjoint from socket state. It must
// equal binder_wire::kCompletedNativeFenceMetadata (checked in native_fence.cc).
inline constexpr uint64_t kTransferredCompletedFence = 0x4;
void ForgetNativeFence(int fd);
void DupNativeFence(int from, int to);
bool TransferNativeFence(int fd, uintptr_t* handle, uint64_t* metadata,
                         std::shared_ptr<NativeFence>* retained = nullptr);
int AdoptNativeFence(HANDLE owned, uint64_t metadata);
// Linux's sync_file ioctls on a native fence fd (<linux/sync_file.h>):
// SYNC_IOC_FILE_INFO; others fail with ENOTTY. False: fd is not a native fence.
bool SyncFileIoctl(int fd, unsigned long request, void* argument, int* result);
short PollNativeFence(const std::shared_ptr<NativeFence>& fence, short events, HANDLE* wake);
}  // namespace gesher
