// Gesher: the device-only stand-ins' hooks (compat/framework/device_standins.cc).
#pragma once

#include <jni.h>
#include <stdint.h>

// Returns the android.view.Surface (a local reference) that
// BLASTBufferQueue.nativeGetSurface hands out for the BLAST adapter `blast`,
// or null. Gesher's window code installs one (docs/graphics.md, "Window").
typedef jobject (*GesherBlastSurfaceProvider)(JNIEnv* env, int64_t blast);
void gesher_set_blast_surface_provider(GesherBlastSurfaceProvider provider);

// The tag of a stand-in handle (a SurfaceControl's, a BLASTBufferQueue's):
// 0 when none was set. The window code tags the SurfaceControl a window
// session hands out with its window; copies of it
// (nativeCopyFromSurfaceControl) and the BLAST adapter updated with it
// (nativeUpdate) carry the tag, so nativeGetSurface finds the window.
int64_t gesher_standin_tag(int64_t handle);
void gesher_standin_set_tag(int64_t handle, int64_t tag);

// Registers the stand-in tables; called from libandroid_runtime's table.
int register_gesher_device_standins(JNIEnv* env);
