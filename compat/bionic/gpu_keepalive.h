// gesher: keeps the GPU from sitting idle between the frames Gesher reads back.
//
// Gesher reads every frame of an EGL child surface (egl_child_surface.cc) and
// of an ASurfaceControl composition (gpu_interop.cc) back for its window, and
// waits for the GPU there. On some machines Direct3D 11 work submitted after the
// GPU sat idle for more than about 30 ms completes only about half a second
// later, up to once a second, whatever waits for it (Map, an event query
// polled by glFinish): measured on an Intel UHD Graphics 770 (driver
// 32.0.101.6556, Windows Server 2025), with a plain Direct3D 11 program and
// without Gesher as well (ANGLE, a 1280x720 frame drawn, read back and paced
// on a timer: at 5, 10, 15, 20 and 25 frames a second 24 to 57 waits of 488
// to 520 ms a minute; at 30, 40, 50 and 60 none). An app drawing fewer than
// about 30 frames a second froze for half a second at a time: Firefox's
// compositor showing a 25 fps video, 5 to 24 times a minute.
//
// While frames come, a thread of its own clears a 1x1 texture of a Direct3D 11
// device of its own, on the frames' adapter, and flushes, whenever no frame
// came for 10 ms; a second after the last frame it stops until the next one.
// The GPU then never sits idle long between an app's frames (the same program
// at 25 frames a second: no waits). Not on a software adapter (WARP).
// GESHER_GPU_KEEPALIVE=0 turns it off.
#pragma once

#include <cstdint>

struct ID3D11Device;

namespace gesher::bionic {

// A frame was just drawn and read back on ANGLE's display `egl_display` (an
// EGLDisplay of ANGLE's). The first call starts the thread on that display's
// adapter; later calls are an atomic store.
void GpuKeepAliveAfterFrame(void* egl_display);

// The same for a frame drawn and read back on `device`.
void GpuKeepAliveAfterFrameOn(ID3D11Device* device);

// For tests: whether the thread was started, and how many times it has given
// the GPU work.
bool GpuKeepAliveStarted();
uint64_t GpuKeepAliveSubmissions();

}  // namespace gesher::bionic
