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
// Consumes owned on every outcome. Binder keeps the default; SCM_RIGHTS
// supplies the receiving message's MSG_CMSG_CLOEXEC choice explicitly.
int AdoptNativeFence(HANDLE owned, uint64_t metadata, bool close_on_exec = true);
short PollNativeFence(const std::shared_ptr<NativeFence>& fence, short events, HANDLE* wake);
}  // namespace gesher
