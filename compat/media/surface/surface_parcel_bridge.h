// Gesher: what android.view.Surface's Parcel natives (compat/framework/
// surface_parcel.cc, which has libbinder) need of the surfaces here
// (surface_jni.cc, compiled into hwui). A Surface's native object
// (Surface.mNativeObject) is an android::Surface*; these take it as a jlong.
#pragma once

#include <jni.h>
#include <stdint.h>

namespace android {
class RefBase;
}

extern "C" {

// A frame queued to a forwarding surface (RGBA_8888, width pixels a row), on
// its producer's thread, after the frame is the surface's latest.
typedef void (*GesherSurfaceFrameFn)(void* context, jlong surface, int width, int height,
                                     int64_t timestamp_ns, const uint32_t* pixels);
typedef void (*GesherSurfaceReleaseFn)(void* context);

// Whether the surface is a QueueSurface (a SurfaceTexture's, a SurfaceView's,
// an ImageReader's, a forwarding one) and so can travel; its consumer's default
// buffer size (the size of a buffer its producer gives none).
bool gesher_surface_queue_size(jlong surface, int* width, int* height);

// Queues a frame from a producer in another process (QueueSurface::PostFrame);
// false once the surface is abandoned or is not a QueueSurface.
bool gesher_surface_post(jlong surface, int width, int height, int stride, int64_t timestamp_ns,
                         const uint32_t* pixels);

// A new QueueSurface with one strong reference (for a Java Surface's
// mNativeObject) whose every frame goes to on_frame. `context` is released
// (release(context)) when the surface goes or is abandoned, once no frame is
// being handed over.
jlong gesher_surface_create_forwarding(int width, int height, GesherSurfaceFrameFn on_frame,
                                       GesherSurfaceReleaseFn release, void* context);

// The default buffer size of a QueueSurface (its consumer's).
void gesher_surface_set_default_size(jlong surface, int width, int height);

// No producer may queue to the QueueSurface any more (its consumer is gone).
void gesher_surface_abandon(jlong surface);

// The surface's RefBase: its strong and weak references.
android::RefBase* gesher_surface_refbase(jlong surface);

}  // extern "C"
