// gesher: RGBA8888 AHardwareBuffers for ImageReader images (docs/media.md), as
// opaque handles (an AHardwareBuffer*), so the media code (hwui, camera_frames.cc)
// needs none of hardware_buffer.h's EGL/GL headers. Implemented in
// compat/bionic/hardware_buffer.cc, linked into dalvikvm only.
//
// A reader keeps a small pool of these buffers and writes each frame into one
// that nothing else holds, so the buffers (and their AHardwareBuffer_getId) repeat
// across frames and an app's per-id texture caches keep hitting. Every holder
// (an Image, a Java HardwareBuffer, an EGLImage) owns a reference; the pool
// writes only into a buffer it alone references.
#pragma once

#include <stdint.h>

namespace gesher::bionic::ahb {

// A new buffer, one reference (the caller's); GPU_SAMPLED_IMAGE | CPU_READ_OFTEN
// usage, rows of width * 4 bytes. Null when it cannot be allocated.
void* AllocateRgbaBuffer(uint32_t width, uint32_t height);
void RetainRgbaBuffer(void* buffer);
void ReleaseRgbaBuffer(void* buffer);
// Only the caller holds a reference (so the buffer may be written).
bool RgbaBufferUnshared(void* buffer);
// The CPU memory (width * height * 4 bytes).
uint8_t* RgbaBufferPixels(void* buffer);
// After the CPU wrote RgbaBufferPixels: the buffer's GPU form, if it has one,
// is brought up to date before this returns; otherwise it is made from the
// memory when first used.
void PublishRgbaWrite(void* buffer);
// AHardwareBuffer_getId and the usage AHardwareBuffer_describe reports.
uint64_t RgbaBufferId(void* buffer);
uint64_t RgbaBufferUsage(void* buffer);

}  // namespace gesher::bionic::ahb
