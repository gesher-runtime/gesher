// An x86-64 Android .so with JNI natives, loaded by dalvikvm through
// System.load (tests/native/java/gesher/JniTest.java): ART's host
// libnativeloader path dlopen()s it, gesher-base hands it to the ELF loader,
// and the JNI bridge (compat/bionic/jni_bridge.cc) connects it to ART.
//
// It covers natives found by name (Java_*) and registered in JNI_OnLoad
// (RegisterNatives), arguments in registers and on the stack, and calls back
// into ART through the JNIEnv and JavaVM tables: classes, methods (variadic,
// va_list and jvalue forms, static, virtual and non-virtual), fields, objects,
// strings, arrays, exceptions, a jmethodID from FromReflectedMethod, and a
// thread of its own that attaches to the VM. Built for API 29 with the stack
// protector, with an initial-exec thread-local, so threads ART created must get
// their TLS block (ADR 0014) before they run its code.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "jni_argument_callbacks.h"
#include "jni_getenv_checks.h"

// ASharedMemory (libandroid, in an app process), as libcutils' memfd regions:
// created sized and sealed F_SEAL_GROW | F_SEAL_SHRINK; setProt(PROT_READ) adds
// F_SEAL_FUTURE_WRITE for good. Chromium tells shared memory by those seals.
// Returns 0 when all hold, else the number of the first failure.
JNIEXPORT jint JNICALL Java_gesher_JniTest_sharedMemory(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  void* android = dlopen("libandroid.so", RTLD_NOW);
  if (android == NULL) return 1;
  int (*create)(const char*, size_t) = (int (*)(const char*, size_t))dlsym(android, "ASharedMemory_create");
  int (*set_prot)(int, int) = (int (*)(int, int))dlsym(android, "ASharedMemory_setProt");
  size_t (*get_size)(int) = (size_t (*)(int))dlsym(android, "ASharedMemory_getSize");
  if (create == NULL || set_prot == NULL || get_size == NULL) return 2;
  const size_t size = 4096;
  int fd = create("jni-region", size);
  if (fd < 0) return 3;
  int failure = 0;
  if (get_size(fd) != size) failure = 4;
  else if (fcntl(fd, F_GET_SEALS) != (F_SEAL_GROW | F_SEAL_SHRINK)) failure = 5;
  else if (set_prot(fd, PROT_READ | PROT_WRITE) != 0 || write(fd, "rw", 2) != 2) failure = 6;
  else if (set_prot(fd, PROT_READ) != 0 || (fcntl(fd, F_GET_SEALS) & F_SEAL_FUTURE_WRITE) == 0) failure = 7;
  else if (write(fd, "x", 1) != -1 || errno != EPERM) failure = 8;
  else if (set_prot(fd, PROT_READ | PROT_WRITE) != -1 || errno != EINVAL) failure = 9;
  else if (mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) != MAP_FAILED) failure = 10;
  close(fd);
  return failure;
}

static JavaVM* g_vm;
static jclass g_class;  // gesher/JniTest, a global reference
static int failures;

// Deliberately fatal, and called only by the isolated FatalContext fixture.
// The ELF read needs a raw PC; the host strlen read also needs a PE module
// and offset. A volatile pointer/function prevents folding away either call.
JNIEXPORT jlong JNICALL Java_gesher_JniTest_fatalNullRead(JNIEnv* env, jclass cls, jboolean host) {
  (void)env;
  (void)cls;
  volatile uintptr_t address = 0x308;
  if (host) {
    size_t (*volatile length)(const char*) = strlen;
    return (jlong)length((const char*)address);
  }
  return *(volatile unsigned char*)address;
}

static void check(const char* what, int ok) {
  printf("    native: %-52s %s\n", what, ok ? "ok" : "FAIL");
  fflush(stdout);
  if (!ok) ++failures;
}

// --- natives found by name ---------------------------------------------------

JNIEXPORT jint JNICALL Java_gesher_JniTest_add(JNIEnv* env, jclass cls, jint a, jint b) {
  (void)env;
  (void)cls;
  return a + b;
}

// A library that hooks JNI (Opera's does): it copies the JNIEnv's table, replaces
// an entry with its own, which calls the original, and stores the copy in the
// JNIEnv. The copy's other entries are called with the same JNIEnv and must still
// reach ART; the hook must reach the original. Returns 0 when all hold, else the
// number of the first failure.
static const struct JNINativeInterface* g_original_table;
static int g_hooked_calls;
static jsize JNICALL HookedGetStringLength(JNIEnv* env, jstring s) {
  ++g_hooked_calls;
  return g_original_table->GetStringLength(env, s);
}
JNIEXPORT jint JNICALL Java_gesher_JniTest_hookedTable(JNIEnv* env, jclass cls, jstring s) {
  (void)cls;
  static struct JNINativeInterface copy;
  g_original_table = *env;
  copy = **env;
  copy.GetStringLength = HookedGetStringLength;
  *env = &copy;
  int failure = 0;
  if ((*env)->GetStringLength(env, s) != 6) failure = 1;          // the hook, then ART
  else if (g_hooked_calls != 1) failure = 2;
  else if ((*env)->GetStringUTFLength(env, s) != 6) failure = 3;  // a copied entry
  else if ((*env)->GetVersion(env) < JNI_VERSION_1_6) failure = 4;
  else if ((*env)->FindClass(env, "java/lang/String") == NULL) failure = 5;
  *env = g_original_table;
  if (failure == 0 && (*env)->GetStringLength(env, s) != 6) failure = 6;  // restored
  if (failure == 0 && g_hooked_calls != 1) failure = 7;
  return failure;
}

JNIEXPORT jstring JNICALL Java_gesher_JniTest_greet(JNIEnv* env, jclass cls, jstring who) {
  (void)cls;
  const char* s = (*env)->GetStringUTFChars(env, who, NULL);
  char buf[128];
  snprintf(buf, sizeof(buf), "Hello, %s!", s);
  (*env)->ReleaseStringUTFChars(env, who, s);
  return (*env)->NewStringUTF(env, buf);
}

// Six integer and eight floating-point arguments in registers, the rest on the
// stack (System V), through the bridge's entry stub.
JNIEXPORT jdouble JNICALL Java_gesher_JniTest_mix(JNIEnv* env, jclass cls, jint a, jdouble b,
                                                  jlong c, jfloat d, jint e, jint f, jint g,
                                                  jint h, jdouble i, jdouble j, jdouble k,
                                                  jdouble l, jdouble m, jdouble n, jdouble o) {
  (void)env;
  (void)cls;
  return a + b + (double)c + d + e * 10.0 + f * 100.0 + g * 1000.0 + h * 10000.0 + i + j * 2 +
         k * 4 + l * 8 + m * 16 + n * 32 + o * 64;
}

JNIEXPORT void JNICALL Java_gesher_JniTest_throwFromNative(JNIEnv* env, jclass cls) {
  (void)cls;
  jclass ex = (*env)->FindClass(env, "java/lang/UnsupportedOperationException");
  (*env)->ThrowNew(env, ex, "thrown by native code");
}

// --- initial-exec TLS on threads ART made --------------------------------------

static __thread int tls_counter __attribute__((tls_model("initial-exec"))) = 5;

JNIEXPORT jint JNICALL Java_gesher_JniTest_tlsBump(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  return ++tls_counter;
}

// --- calls back into ART ---------------------------------------------------------

JNIEXPORT jint JNICALL Java_gesher_JniTest_callbacks(JNIEnv* env, jclass cls, jobject self) {
  failures = 0;
  check("GetVersion >= 1.6", (*env)->GetVersion(env) >= JNI_VERSION_1_6);

  jclass found = (*env)->FindClass(env, "gesher/JniTest");
  check("FindClass", found != NULL && (*env)->IsSameObject(env, found, cls));

  jmethodID twice = (*env)->GetStaticMethodID(env, cls, "twice", "(I)I");
  check("GetStaticMethodID", twice != NULL);
  check("CallStaticIntMethod (variadic)", (*env)->CallStaticIntMethod(env, cls, twice, 21) == 42);
  jvalue arg;
  arg.i = 50;
  check("CallStaticIntMethodA", (*env)->CallStaticIntMethodA(env, cls, twice, &arg) == 100);
  check("18-argument callbacks (variadic, V and A)", jni_argument_wide_callback(env, cls));

  // Mixed arguments: some in registers, some on the stack of the variadic call.
  jmethodID describe = (*env)->GetStaticMethodID(
      env, cls, "describe", "(IJDFZCSBLjava/lang/String;[I)Ljava/lang/String;");
  jintArray arr = (*env)->NewIntArray(env, 3);
  jint vals[3] = {7, 8, 9};
  (*env)->SetIntArrayRegion(env, arr, 0, 3, vals);
  jstring str = (*env)->NewStringUTF(env, "str");
  jstring desc = (jstring)(*env)->CallStaticObjectMethod(
      env, cls, describe, (jint)-3, (jlong)0x123456789LL, 2.5, (jfloat)0.75f, (jboolean)JNI_TRUE,
      (jchar)'Q', (jshort)-12, (jbyte)100, str, arr);
  const char* d = desc ? (*env)->GetStringUTFChars(env, desc, NULL) : NULL;
  int desc_ok = d != NULL && strcmp(d, "-3 4886718345 2.5 0.75 true Q -12 100 str [7, 8, 9]") == 0;
  if (!desc_ok) printf("    native: describe returned '%s'\n", d ? d : "(null)");
  check("CallStaticObjectMethod with 10 mixed arguments", desc_ok);
  if (d) (*env)->ReleaseStringUTFChars(env, desc, d);

  // Instance methods: virtual and non-virtual, variadic and va_list forms.
  jmethodID imix = (*env)->GetMethodID(env, cls, "instanceMix", "(IJDFLjava/lang/String;)J");
  jlong expect = 1 + 2 + 3 + 4 + 3 + 1000;
  check("CallLongMethod",
        (*env)->CallLongMethod(env, self, imix, (jint)1, (jlong)2, 3.0, (jfloat)4.0f, str) ==
            expect);
  check("CallNonvirtualLongMethod",
        (*env)->CallNonvirtualLongMethod(env, self, cls, imix, (jint)1, (jlong)2, 3.0,
                                         (jfloat)4.0f, str) == expect);

  // Fields.
  jfieldID base = (*env)->GetFieldID(env, cls, "base", "J");
  check("GetLongField", (*env)->GetLongField(env, self, base) == 1000);
  jfieldID counter = (*env)->GetFieldID(env, cls, "counter", "I");
  (*env)->SetIntField(env, self, counter, 77);
  check("SetIntField / GetIntField", (*env)->GetIntField(env, self, counter) == 77);
  jfieldID sfield = (*env)->GetStaticFieldID(env, cls, "sfield", "Ljava/lang/String;");
  jstring sv = (jstring)(*env)->GetStaticObjectField(env, cls, sfield);
  check("GetStaticObjectField", sv != NULL && (*env)->GetStringUTFLength(env, sv) == 12);

  // Objects.
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject made = (*env)->NewObject(env, cls, ctor, (jint)9);
  check("NewObject (variadic)", made != NULL && (*env)->GetIntField(env, made, counter) == 9);
  check("IsInstanceOf", (*env)->IsInstanceOf(env, made, cls));

  // Arrays.
  jint* elems = (*env)->GetIntArrayElements(env, arr, NULL);
  check("GetIntArrayElements", elems != NULL && elems[0] + elems[1] + elems[2] == 24);
  if (elems) (*env)->ReleaseIntArrayElements(env, arr, elems, JNI_ABORT);
  check("GetArrayLength", (*env)->GetArrayLength(env, arr) == 3);

  // A Java exception, seen and cleared by native code.
  jmethodID thrower = (*env)->GetStaticMethodID(env, cls, "thrower", "()V");
  (*env)->CallStaticVoidMethod(env, cls, thrower);
  jthrowable ex = (*env)->ExceptionOccurred(env);
  check("ExceptionCheck after a Java throw", (*env)->ExceptionCheck(env));
  (*env)->ExceptionClear(env);
  jclass ise = (*env)->FindClass(env, "java/lang/IllegalStateException");
  check("the exception is an IllegalStateException", ex && (*env)->IsInstanceOf(env, ex, ise));
  check("ExceptionClear", !(*env)->ExceptionCheck(env));

  // The JavaVM, and the JNIEnv it gives back.
  JavaVM* vm = NULL;
  check("GetJavaVM", (*env)->GetJavaVM(env, &vm) == JNI_OK && vm == g_vm);
  JNIEnv* again = NULL;
  check("GetEnv gives this JNIEnv",
        (*vm)->GetEnv(vm, (void**)&again, JNI_VERSION_1_6) == JNI_OK && again == env);
  check("GetEnv version and interface boundary", jni_getenv_contracts(vm, env));

  // Local frames.
  check("PushLocalFrame", (*env)->PushLocalFrame(env, 16) == 0);
  jobject kept = (*env)->PopLocalFrame(env, (*env)->NewStringUTF(env, "kept"));
  check("PopLocalFrame keeps its result", kept != NULL);
  return failures;
}

// A jmethodID that did not come from GetMethodID: the bridge finds its
// parameter types by reflection for the variadic call.
JNIEXPORT jint JNICALL Java_gesher_JniTest_callReflected(JNIEnv* env, jclass cls, jobject method) {
  jmethodID mid = (*env)->FromReflectedMethod(env, method);
  return (*env)->CallStaticIntMethod(env, cls, mid, (jint)5, 0.5, (jlong)7);
}

// --- a thread of the library's own ---------------------------------------------

static void* worker(void* out) {
  if (!jni_getenv_detached(g_vm)) {
    *(int*)out = -3;
    return NULL;
  }
  JNIEnv* env = NULL;
  if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) {
    *(int*)out = -1;
    return NULL;
  }
  jmethodID twice = (*env)->GetStaticMethodID(env, g_class, "twice", "(I)I");
  *(int*)out = (*env)->CallStaticIntMethod(env, g_class, twice, 50) + tls_counter;
  if ((*g_vm)->DetachCurrentThread(g_vm) != JNI_OK || !jni_getenv_detached(g_vm)) {
    *(int*)out = -4;
  } else {
    puts("GetEnv attach/detach: all passed");
  }
  return NULL;
}

JNIEXPORT jint JNICALL Java_gesher_JniTest_threadAttach(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  int result = 0;
  pthread_t t;
  if (pthread_create(&t, NULL, worker, &result) != 0) return -2;
  pthread_join(t, NULL);
  return result;  // 100 + the new thread's initial tls_counter (5)
}

// A thread that ends still attached, as an arm64 library's daemon threads do
// (Waze): ART warns and keeps it listed, as on Android, where its key
// destructor is not called a second time (patches/art/0770); it aborted the
// process ("Native thread exited without calling DetachCurrentThread").
static void* attached_worker(void* out) {
  JNIEnv* env = NULL;
  if ((*g_vm)->AttachCurrentThreadAsDaemon(g_vm, &env, NULL) != JNI_OK) {
    *(int*)out = -1;
    return NULL;
  }
  jmethodID twice = (*env)->GetStaticMethodID(env, g_class, "twice", "(I)I");
  *(int*)out = (*env)->CallStaticIntMethod(env, g_class, twice, 7);
  return NULL;  // no DetachCurrentThread
}

JNIEXPORT jint JNICALL Java_gesher_JniTest_threadExitAttached(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  int result = 0;
  pthread_t t;
  if (pthread_create(&t, NULL, attached_worker, &result) != 0) return -2;
  pthread_join(t, NULL);
  return result;  // 14
}

// --- registered in JNI_OnLoad --------------------------------------------------

static jint registered_twice(JNIEnv* env, jclass cls, jint x) {
  (void)env;
  (void)cls;
  return x * 2;
}

static jlong instance_sum(JNIEnv* env, jobject self, jlong a) {
  jfieldID base = (*env)->GetFieldID(env, g_class, "base", "J");
  return a + (*env)->GetLongField(env, self, base);
}

static int g_onload_ran;

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)reserved;
  g_vm = vm;
  JNIEnv* env = NULL;
  if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = (*env)->FindClass(env, "gesher/JniTest");
  if (cls == NULL) return JNI_ERR;
  g_class = (jclass)(*env)->NewGlobalRef(env, cls);
  static const JNINativeMethod methods[] = {
      {"registeredTwice", "(I)I", (void*)registered_twice},
      {"instanceSum", "(J)J", (void*)instance_sum},
  };
  if ((*env)->RegisterNatives(env, cls, methods, 2) != JNI_OK) return JNI_ERR;
  g_onload_ran = 1;
  return JNI_VERSION_1_6;
}

JNIEXPORT jboolean JNICALL Java_gesher_JniTest_onLoadRan(JNIEnv* env, jclass cls) {
  (void)env;
  (void)cls;
  return g_onload_ran ? JNI_TRUE : JNI_FALSE;
}
