// gesher: what the JNI bridge (jni_bridge.cc) offers the rest of the shim.
#pragma once

#include <jni.h>

namespace gesher::bionic {

// The JNIEnv a loaded .so holds is the bridge's System V proxy. Returns the
// runtime's real (Microsoft-ABI) JNIEnv behind it, for Gesher code that calls
// JNI on the .so's behalf (libandroid's AAssetManager_fromJava, ...); any
// other JNIEnv* is returned as it is.
JNIEnv* RealJniEnv(JNIEnv* env);

}  // namespace gesher::bionic
