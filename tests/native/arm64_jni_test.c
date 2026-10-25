// An arm64-v8a Android .so with JNI natives, loaded by dalvikvm through
// System.load (tests/native/java/gesher/Arm64Jni.java): ART's host
// libnativeloader path dlopen()s it, gesher-base hands it to the ELF loader,
// which sees EM_AARCH64 and runs it through the translator (gesher_arm64.dll),
// and the JNI bridge (compat/bionic/jni_bridge.cc) connects the guest to ART
// with guest-callable JNIEnv/JavaVM proxy tables (ADR 0047).
//
// Covers, for a guest library: JNI_OnLoad (GetEnv, FindClass, NewGlobalRef,
// RegisterNatives), natives found by name and registered, arguments both ways,
// strings, static/instance/variadic calls back into ART, fields, a Java
// exception seen and cleared, and a native throw. No thread-locals or threads
// yet (built with -fno-stack-protector); those are the next step.

#include <jni.h>
#include <stdio.h>
#include <string.h>

static jclass g_class;  // gesher/Arm64Jni, a global reference
static int g_onload_ran;
static int failures;

static void check(const char* what, int ok) {
  printf("    native: %-52s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok) ++failures;
}

// --- natives found by name ---------------------------------------------------

JNIEXPORT jint JNICALL Java_gesher_Arm64Jni_add(JNIEnv* env, jclass cls, jint a, jint b) {
  (void)env;
  (void)cls;
  return a + b;
}

JNIEXPORT jstring JNICALL Java_gesher_Arm64Jni_greet(JNIEnv* env, jclass cls, jstring who) {
  (void)cls;
  const char* s = (*env)->GetStringUTFChars(env, who, NULL);
  char buf[128];
  snprintf(buf, sizeof(buf), "Hello, %s!", s);
  (*env)->ReleaseStringUTFChars(env, who, s);
  return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT void JNICALL Java_gesher_Arm64Jni_throwFromNative(JNIEnv* env, jclass cls) {
  (void)cls;
  jclass ex = (*env)->FindClass(env, "java/lang/UnsupportedOperationException");
  (*env)->ThrowNew(env, ex, "thrown by guest native code");
}

JNIEXPORT jboolean JNICALL Java_gesher_Arm64Jni_onLoadRan(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  return g_onload_ran ? JNI_TRUE : JNI_FALSE;
}

// --- calls back into ART -----------------------------------------------------

JNIEXPORT jint JNICALL Java_gesher_Arm64Jni_callbacks(JNIEnv* env, jclass cls, jobject self) {
  failures = 0;
  check("GetVersion >= 1.6", (*env)->GetVersion(env) >= JNI_VERSION_1_6);

  jclass found = (*env)->FindClass(env, "gesher/Arm64Jni");
  check("FindClass", found != NULL && (*env)->IsSameObject(env, found, cls));

  jmethodID twice = (*env)->GetStaticMethodID(env, cls, "twice", "(I)I");
  check("GetStaticMethodID", twice != NULL);
  check("CallStaticIntMethod (variadic)", (*env)->CallStaticIntMethod(env, cls, twice, 21) == 42);
  jvalue arg;
  arg.i = 50;
  check("CallStaticIntMethodA", (*env)->CallStaticIntMethodA(env, cls, twice, &arg) == 100);

  // Mixed arguments through the variadic bridge (ints, long, double, float).
  jmethodID describe = (*env)->GetStaticMethodID(env, cls, "describe", "(IJDF)Ljava/lang/String;");
  jstring desc = (jstring)(*env)->CallStaticObjectMethod(env, cls, describe, (jint)-3,
                                                         (jlong)0x123456789LL, 2.5, (jfloat)0.75f);
  const char* d = desc ? (*env)->GetStringUTFChars(env, desc, NULL) : NULL;
  int desc_ok = d != NULL && strcmp(d, "-3 4886718345 2.5 0.75") == 0;
  if (!desc_ok) printf("    native: describe returned '%s'\n", d ? d : "(null)");
  check("CallStaticObjectMethod with mixed arguments", desc_ok);
  if (d) (*env)->ReleaseStringUTFChars(env, desc, d);

  // An instance method (variadic), and a field.
  jmethodID imix = (*env)->GetMethodID(env, cls, "instanceMix", "(IJ)J");
  check("CallLongMethod",
        (*env)->CallLongMethod(env, self, imix, (jint)1, (jlong)2) == (1 + 2 + 1000));
  jfieldID base = (*env)->GetFieldID(env, cls, "base", "J");
  check("GetLongField", (*env)->GetLongField(env, self, base) == 1000);
  jfieldID counter = (*env)->GetFieldID(env, cls, "counter", "I");
  (*env)->SetIntField(env, self, counter, 77);
  check("SetIntField / GetIntField", (*env)->GetIntField(env, self, counter) == 77);

  // A Java exception, seen and cleared by guest native code.
  jmethodID thrower = (*env)->GetStaticMethodID(env, cls, "thrower", "()V");
  (*env)->CallStaticVoidMethod(env, cls, thrower);
  check("ExceptionCheck after a Java throw", (*env)->ExceptionCheck(env));
  (*env)->ExceptionClear(env);
  check("ExceptionClear", !(*env)->ExceptionCheck(env));

  // The JavaVM, and the JNIEnv it gives back (both proxies).
  JavaVM* vm = NULL;
  check("GetJavaVM", (*env)->GetJavaVM(env, &vm) == JNI_OK && vm != NULL);
  JNIEnv* again = NULL;
  check("GetEnv gives this JNIEnv",
        (*vm)->GetEnv(vm, (void**)&again, JNI_VERSION_1_6) == JNI_OK && again == env);
  return failures;
}

// --- registered in JNI_OnLoad ------------------------------------------------

static jint registered_twice(JNIEnv* env, jclass cls, jint x) {
  (void)env;
  (void)cls;
  return x * 2;
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = (*env)->FindClass(env, "gesher/Arm64Jni");
  if (cls == NULL) return JNI_ERR;
  g_class = (jclass)(*env)->NewGlobalRef(env, cls);
  static const JNINativeMethod methods[] = {
      {"registeredTwice", "(I)I", (void*)registered_twice},
  };
  if ((*env)->RegisterNatives(env, cls, methods, 1) != JNI_OK) return JNI_ERR;
  g_onload_ran = 1;
  return JNI_VERSION_1_6;
}
