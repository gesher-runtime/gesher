// Gesher: the registration functions of libcore's native sources that are
// not ported yet (GESHER_LIBCORE_UNPORTED and GESHER_OPENJDK_UNPORTED in
// cmake/aosp/libcore_native.cmake, which defines GESHER_UNPORTED_<file> for
// each). Register.cpp and OnLoad.cpp call them; they register nothing, so
// calling one of those natives throws UnsatisfiedLinkError.

#include <jni.h>

#if defined(GESHER_UNPORTED_libcore_math_NativeBN)
void register_libcore_math_NativeBN(JNIEnv*) {}
#endif

// UNIXProcess_md.c's natives are compat/libcore/process.cc's (docs/exec.md).
