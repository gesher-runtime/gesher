// Shared native ownership for MediaCodec.PersistentSurface and its consumers.
#pragma once

#include "../video/input_surface.h"

namespace gesher::media {

// Return a real MediaCodec.PersistentSurface with one native producer reference.
jobject NewPersistentInputSurface(JNIEnv* env);
// Take a native reference while holding Surface.mLock, as Android does. A
// concurrent Surface.release cannot revoke a reference already admitted here.
// Null, nonpersistent and released arguments throw IllegalArgumentException.
std::shared_ptr<PersistentInputSurface> GetPersistentInputSurface(JNIEnv* env, jobject surface);
// Retire the Java token; the Java override subsequently releases its Surface.
// Native consumer leases and pending recorder configurations retain ownership.
void ReleasePersistentInputSurface(JNIEnv* env, jobject surface);

}  // namespace gesher::media
