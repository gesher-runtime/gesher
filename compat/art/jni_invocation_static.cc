// Gesher: JniInvocation for a runtime linked into the same executable.
//
// libnativehelper's JniInvocation dlopen()s libart.so and forwards
// JNI_CreateJavaVM and friends to it. Gesher links ART statically, so the
// runtime's own JNI_CreateJavaVM is already there and there is nothing to load.

#include "nativehelper/JniInvocation.h"

struct JniInvocationImpl {};

namespace {
JniInvocationImpl the_instance;
}  // namespace

extern "C" {

JniInvocationImpl* JniInvocationCreate() { return &the_instance; }

bool JniInvocationInit(JniInvocationImpl*, const char*) { return true; }

void JniInvocationDestroy(JniInvocationImpl*) {}

const char* JniInvocationGetLibrary(const char* library, char*) {
  return library != nullptr ? library : "(linked in)";
}

}  // extern "C"
