// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows native synchronization objects behind Android native-fence fds.
// A descriptor is never inferred to be a fence merely because poll says it
// is readable. Only these registered objects can be imported by EGL.
#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>

struct ID3D11Device;

namespace gesher {

enum class FenceStatus { kPending, kSignaled, kFailed };
class NativeFence;
// A distinct, named, initially unsignaled native completion event. Its name
// marks the descriptor type across processes; exported handles cannot reset it.
HANDLE CreateNativeFenceEvent();

// Takes a reference to device.
// The event is owned by the result (also on failure) and must be a manual-reset
// completion event submitted to DXGI. It stays alive until DXGI signals it,
// even when the caller destroys the sync before completion.
std::shared_ptr<NativeFence> NativeFenceFromEvent(ID3D11Device* device, HANDLE owned_event);
FenceStatus NativeFenceStatus(const std::shared_ptr<NativeFence>& fence);
FenceStatus WaitNativeFence(const std::shared_ptr<NativeFence>& fence, uint64_t timeout_ns);
// Called under ANGLE's submission mutex. Local fences on this very device
// are already queue-ordered; imported fences have completed before export.
HRESULT ServerWaitNativeFence(const std::shared_ptr<NativeFence>& fence,
                             ID3D11Device* device);

// Exports a read-only, ALREADY COMPLETED native event. This may wait here,
// outside ANGLE's mutex, and is called lazily by eglDupNativeFenceFDANDROID.
// Descriptor duplication and interprocess transfers keep the same object.
int ExportNativeFence(const std::shared_ptr<NativeFence>& fence);
std::shared_ptr<NativeFence> LookupNativeFence(int fd);

}  // namespace gesher
