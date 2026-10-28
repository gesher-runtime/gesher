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
// exception seen and cleared, and a native throw; the va_list forms
// (Call*MethodV from the guest's own variadic helper); guest natives nested
// inside Java called from guest natives; and a guest pthread that attaches
// to the VM (AttachCurrentThread), calls into Java and detaches, with its own
// thread-locals. Built with the stack protector (a TLS slot on arm64).

#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
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

// --- the va_list forms -----------------------------------------------------------

static jint call_static_int_v(JNIEnv* env, jclass cls, jmethodID mid, ...) {
  va_list ap, copy;
  va_start(ap, mid);
  va_copy(copy, ap);
  jint r = (*env)->CallStaticIntMethodV(env, cls, mid, ap);
  jint again = (*env)->CallStaticIntMethodV(env, cls, mid, copy);
  va_end(copy);
  va_end(ap);
  check("CallStaticIntMethodV independent va_copy", r == again);
  return r;
}

static jobject call_static_object_v(JNIEnv* env, jclass cls, jmethodID mid, ...) {
  va_list ap, copy;
  va_start(ap, mid);
  va_copy(copy, ap);
  jobject r = (*env)->CallStaticObjectMethodV(env, cls, mid, ap);
  jobject again = (*env)->CallStaticObjectMethodV(env, cls, mid, copy);
  const char* first = r ? (*env)->GetStringUTFChars(env, (jstring)r, NULL) : NULL;
  const char* second = again ? (*env)->GetStringUTFChars(env, (jstring)again, NULL) : NULL;
  check("CallStaticObjectMethodV independent va_copy", first && second && strcmp(first, second) == 0);
  if (first) (*env)->ReleaseStringUTFChars(env, (jstring)r, first);
  if (second) (*env)->ReleaseStringUTFChars(env, (jstring)again, second);
  if (again) (*env)->DeleteLocalRef(env, again);
  va_end(copy);
  va_end(ap);
  return r;
}

JNIEXPORT jint JNICALL Java_gesher_Arm64Jni_vaForms(JNIEnv* env, jclass cls) {
  failures = 0;
  jmethodID twice = (*env)->GetStaticMethodID(env, cls, "twice", "(I)I");
  check("CallStaticIntMethodV (guest va_list)", call_static_int_v(env, cls, twice, 33) == 66);
  jmethodID describe = (*env)->GetStaticMethodID(env, cls, "describe", "(IJDF)Ljava/lang/String;");
  jstring desc = (jstring)call_static_object_v(env, cls, describe, (jint)7, (jlong)-5LL, 0.5,
                                               (double)(jfloat)1.25f);
  const char* d = desc ? (*env)->GetStringUTFChars(env, desc, NULL) : NULL;
  int ok = d != NULL && strcmp(d, "7 -5 0.5 1.25") == 0;
  if (!ok) printf("    native: describe (V) returned '%s'\n", d ? d : "(null)");
  check("CallStaticObjectMethodV with mixed arguments", ok);
  if (d) (*env)->ReleaseStringUTFChars(env, desc, d);
  return failures;
}

// --- nesting: Java -> guest -> Java -> guest ... ------------------------------------

JNIEXPORT jint JNICALL Java_gesher_Arm64Jni_nativeNest(JNIEnv* env, jclass cls, jint n) {
  if (n <= 0) return 0;
  jmethodID nest = (*env)->GetStaticMethodID(env, cls, "nest", "(I)I");
  return 1 + (*env)->CallStaticIntMethod(env, cls, nest, n - 1);
}

// --- a guest thread attached to the VM -----------------------------------------------

static JavaVM* g_vm;
static __thread int tl_marker = 1;

static void* attached_thread(void* arg) {
  (void)arg;
  int ok = tl_marker == 1;
  tl_marker = 2;
  JNIEnv* env = NULL;
  JavaVMAttachArgs args = {JNI_VERSION_1_6, "arm64-guest-thread", NULL};
  if ((*g_vm)->AttachCurrentThread(g_vm, &env, &args) != JNI_OK || env == NULL) {
    return (void*)(intptr_t)-1;
  }
  jmethodID from = (*env)->GetStaticMethodID(env, g_class, "fromThread", "(I)I");
  jint r = from ? (*env)->CallStaticIntMethod(env, g_class, from, 20) : -2;
  JNIEnv* again = NULL;
  ok = ok && (*g_vm)->GetEnv(g_vm, (void**)&again, JNI_VERSION_1_6) == JNI_OK && again == env;
  ok = ok && (*g_vm)->DetachCurrentThread(g_vm) == JNI_OK;
  return (void*)(intptr_t)(ok ? r : -3);
}

JNIEXPORT jint JNICALL Java_gesher_Arm64Jni_attachedThread(JNIEnv* env, jclass cls) {
  (void)cls;
  (*env)->GetJavaVM(env, &g_vm);
  pthread_t t;
  if (pthread_create(&t, NULL, attached_thread, NULL) != 0) return -10;
  void* r = NULL;
  pthread_join(t, &r);
  return tl_marker == 1 ? (jint)(intptr_t)r : -4;
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
