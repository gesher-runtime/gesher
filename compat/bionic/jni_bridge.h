// gesher: what the JNI bridge (jni_bridge.cc) offers the rest of the shim.
#pragma once

#include <jni.h>

namespace gesher::bionic {

// The JNIEnv a loaded .so holds is the bridge's System V proxy. Returns the
// runtime's real (Microsoft-ABI) JNIEnv behind it, for Gesher code that calls
// JNI on the .so's behalf (libandroid's AAssetManager_fromJava, ...); any
// other JNIEnv* is returned as it is.
JNIEnv* RealJniEnv(JNIEnv* env);

// For Gesher code that hands JNI to loaded code itself (NativeActivity's
// ANativeActivity: native_activity.cc): this thread's proxy for the runtime's
// `real` JNIEnv (the thread is set up for loaded code too), and the proxy
// JavaVM. The bridge's tables are filled on first use.
JNIEnv* ProxyJniEnv(JNIEnv* real);
JavaVM* ProxyJavaVm(JNIEnv* real);

// Sets the calling thread up for loaded code (its bionic TLS block, ADR 0014),
// once per thread: before Gesher code calls a .so's function pointer.
void EnterLoadedCode();

}  // namespace gesher::bionic
