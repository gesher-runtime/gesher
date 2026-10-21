// Gesher: the registration functions of libcore's native sources that are
// not ported yet (GESHER_LIBCORE_UNPORTED and GESHER_OPENJDK_UNPORTED in
// cmake/aosp/libcore_native.cmake, which defines GESHER_UNPORTED_<file> for
// each). Register.cpp and OnLoad.cpp call them; they register nothing, so
// calling one of those natives throws UnsatisfiedLinkError.

#include <jni.h>

#if defined(GESHER_UNPORTED_libcore_icu_ICU)
void register_libcore_icu_ICU(JNIEnv*) {}
void unregister_libcore_icu_ICU() {}
#endif

#if defined(GESHER_UNPORTED_libcore_math_NativeBN)
void register_libcore_math_NativeBN(JNIEnv*) {}
#endif

#if defined(GESHER_UNPORTED_org_apache_harmony_xml_ExpatParser)
void register_org_apache_harmony_xml_ExpatParser(JNIEnv*) {}
#endif

#if defined(GESHER_UNPORTED_Character)
void register_java_lang_Character(JNIEnv*) {}
#endif

#if defined(GESHER_UNPORTED_UNIXProcess_md)
extern "C" void register_java_lang_UNIXProcess(JNIEnv*) {}
#endif
