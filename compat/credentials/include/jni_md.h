/* PE libraries receive ART's host JNI tables (Microsoft ABI). Only Java_*
 * native entry points use the managed System V ABI; JNI_OnLoad is host ABI.
 * See compat/bionic/jni_bridge.cc and ADR 0011. */
#ifndef GESHER_JNI_MD_H
#define GESHER_JNI_MD_H
#define JNIEXPORT __declspec(dllexport)
#define JNIIMPORT __declspec(dllimport)
#define JNICALL
#if defined(__x86_64__)
#define GESHER_NATIVE __attribute__((sysv_abi))
#else
#define GESHER_NATIVE
#endif
typedef int jint;
typedef long long jlong;
typedef signed char jbyte;
#endif
