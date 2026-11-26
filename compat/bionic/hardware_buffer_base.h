// Gesher's host interface to the AHardwareBuffer allocator and ownership.
#pragma once

#include <android/hardware_buffer.h>
#include <stddef.h>
#include <stdint.h>

namespace gesher::bionic::ahb {

struct Buffer;

Buffer* FromApp(const void* p);
inline AHardwareBuffer* ToApp(Buffer* b) { return reinterpret_cast<AHardwareBuffer*>(b); }

bool IsBufferSupported(const AHardwareBuffer_Desc& desc);
Buffer* AllocateBuffer(const AHardwareBuffer_Desc& desc);  // one owned reference on success
void Acquire(Buffer* b);
void Release(Buffer* b);
const AHardwareBuffer_Desc& Desc(const Buffer* b);
uint64_t BufferId(const Buffer* b);
size_t AllocationSize(const Buffer* b);

}  // namespace gesher::bionic::ahb
