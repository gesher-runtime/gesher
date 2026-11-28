// gesher: RGBA8888 AHardwareBuffers for ImageReader images (docs/media.md), as
// opaque handles (an AHardwareBuffer*), so the media code (hwui, camera_frames.cc)
// needs none of hardware_buffer.h's EGL/GL headers. Implemented in
// compat/bionic/hardware_buffer.cc, linked into dalvikvm only.
//
// A reader keeps a pool of these buffers and writes each frame into one that no
// Image, Java HardwareBuffer or app reference holds, so the buffers (and their
// AHardwareBuffer_getId) repeat across frames and an app's per-id texture caches
// keep hitting. As on Android, where a BufferQueue slot is rewritten once its
// Image is closed even if a consumer still maps it, a buffer whose only other
// references are the app's EGLImages is reused too. The new pixels reach the
// GPU form (the texture those EGLImages share) when a consumer next takes the
// buffer (FlushRgbaToGpu, from Image.getHardwareBuffer), on the GPU thread in
// order with the compositor, not for frames that are dropped. That write waits
// for the buffer's release fence (the draws the app issued before it let go; the
// texture is shared between the two devices without a keyed mutex), which has
// normally signalled long ago: the reader writes the buffer released longest ago.
#pragma once

#include <stdint.h>

namespace gesher::bionic::ahb {

// A new buffer, one reference (the caller's), rows of width * 4 bytes. With
// gpu_form: GPU_SAMPLED_IMAGE | CPU_READ_OFTEN usage (an app may make EGLImages
// of it; this starts ANGLE and the GPU thread), or CPU_READ_OFTEN alone (logged
// once) when there is no GPU form to give. Without: CPU_READ_OFTEN only, and
// nothing GPU-side is started. Null only when memory runs out.
void* AllocateRgbaBuffer(uint32_t width, uint32_t height, bool gpu_form);
// Whether the current GPU can provide a sampled buffer at this size.
bool SupportsGpuRgbaBuffer(uint32_t width, uint32_t height);
// As above, preserving the consumer's explicit CPU read usage. An opaque GPU
// consumer passes zero CPU usage and gets no CPU-only fallback.
void* AllocateRgbaBuffer(uint32_t width, uint32_t height, bool gpu_form, uint64_t cpu_usage);
void RetainRgbaBuffer(void* buffer);
void ReleaseRgbaBuffer(void* buffer);
// Only the caller holds a reference.
bool RgbaBufferUnshared(void* buffer);
// Nothing but the caller (the pool) and EGLImages of it hold references: it may be written.
bool RgbaBufferReusable(void* buffer);
// The CPU memory (width * height * 4 bytes).
uint8_t* RgbaBufferPixels(void* buffer);
// The CPU wrote RgbaBufferPixels: the GPU form, if any, is older until FlushRgbaToGpu.
void MarkRgbaWritten(void* buffer);
// Brings an existing GPU form up to date with the memory (before an app samples it).
// If an app sampled the buffer, waits for the buffer's release fence first.
void FlushRgbaToGpu(void* buffer);
// An app-facing holder of the buffer (an Image, a Java HardwareBuffer) is about to
// let go of it: if the app made an EGLImage of it, queues a fence on ANGLE's device
// (what the app drew so far) and keeps it on the buffer as its release fence, as
// Android hands an acquire/release fence with a slot. Takes ANGLE's lock: never
// call it under the reader's mutex.
void NoteRgbaAppReleased(void* buffer);
// AHardwareBuffer_getId and the usage AHardwareBuffer_describe reports.
uint64_t RgbaBufferId(void* buffer);
uint64_t RgbaBufferUsage(void* buffer);

}  // namespace gesher::bionic::ahb
