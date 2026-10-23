// Gesher: the device-only stand-ins' hooks (compat/framework/device_standins.cc).
#pragma once

#include <jni.h>
#include <stdint.h>

// Returns the android.view.Surface (a local reference) that
// BLASTBufferQueue.nativeGetSurface hands out for the BLAST adapter `blast`,
// or null. Gesher's window code installs one (docs/graphics.md, "Window").
typedef jobject (*GesherBlastSurfaceProvider)(JNIEnv* env, int64_t blast);
void gesher_set_blast_surface_provider(GesherBlastSurfaceProvider provider);

// Registers the stand-in tables; called from libandroid_runtime's table.
int register_gesher_device_standins(JNIEnv* env);
