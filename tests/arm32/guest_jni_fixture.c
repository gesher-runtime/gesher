// Real AAPCS32 JNI table calls from ARM and Thumb ELF32 code. The host test
// owns a fake ART JNIEnv; all guest pointers and jobject tokens stay 32-bit.
// This is a native component fixture, not an APK.
typedef unsigned int u32;
typedef struct { u32 functions; } JNIEnv32;
typedef u32 jobject32;
typedef u32 jclass32;
typedef u32 jstring32;

typedef int (*GetVersion)(JNIEnv32*);
typedef int (*ExceptionCheck)(JNIEnv32*);
typedef void (*ExceptionClear)(JNIEnv32*);
typedef jclass32 (*FindClass)(JNIEnv32*, const char*);
typedef jobject32 (*NewLocalRef)(JNIEnv32*, jobject32);
typedef jobject32 (*NewGlobalRef)(JNIEnv32*, jobject32);
typedef void (*DeleteRef)(JNIEnv32*, jobject32);
typedef int (*IsSameObject)(JNIEnv32*, jobject32, jobject32);
typedef jstring32 (*NewStringUTF)(JNIEnv32*, const char*);
typedef int (*GetStringUTFLength)(JNIEnv32*, jstring32);
typedef int (*GetStringLength)(JNIEnv32*, jstring32);
typedef const char* (*GetStringUTFChars)(JNIEnv32*, jstring32, unsigned char*);
typedef void (*ReleaseStringUTFChars)(JNIEnv32*, jstring32, const char*);
typedef jclass32 (*GetObjectClass)(JNIEnv32*, jobject32);

static const u32* table(JNIEnv32* env) {
  return (const u32*)(unsigned long)env->functions;
}
#define API(env, slot, kind) ((kind)(unsigned long)table(env)[slot])

int jni32_version(JNIEnv32* env) {
  return API(env, 4, GetVersion)(env);
}

int jni32_contract(JNIEnv32* env) {
  if (API(env, 4, GetVersion)(env) != 0x10006) return -1;
  if (API(env, 228, ExceptionCheck)(env)) return -2;
  jclass32 klass = API(env, 6, FindClass)(env, "java/lang/String");
  if (!klass) return -3;
  jstring32 str = API(env, 167, NewStringUTF)(env, "hello");
  if (!str) return -4;
  if (API(env, 168, GetStringUTFLength)(env, str) != 5) return -5;
  if (API(env, 164, GetStringLength)(env, str) != 5) return -6;
  unsigned char is_copy = 0;
  const char* utf = API(env, 169, GetStringUTFChars)(env, str, &is_copy);
  if (!utf || !is_copy || utf[0] != 'h' || utf[4] != 'o' || utf[5] != 0)
    return -7;
  API(env, 170, ReleaseStringUTFChars)(env, str, utf);
  jobject32 local = API(env, 25, NewLocalRef)(env, str);
  jobject32 global = API(env, 21, NewGlobalRef)(env, local);
  if (!local || !global ||
      !API(env, 24, IsSameObject)(env, str, global)) return -8;
  jclass32 actual_class = API(env, 31, GetObjectClass)(env, str);
  if (!actual_class || !API(env, 24, IsSameObject)(env, klass, actual_class))
    return -9;
  API(env, 22, DeleteRef)(env, global);
  API(env, 23, DeleteRef)(env, local);
  API(env, 17, ExceptionClear)(env);
  return 42;
}

// A stale opaque guest token must not resolve back to a host jobject.
int jni32_stale(JNIEnv32* env) {
  jstring32 str = API(env, 167, NewStringUTF)(env, "stale");
  if (!str) return -1;
  API(env, 23, DeleteRef)(env, str);
  return API(env, 164, GetStringLength)(env, str);
}

// The second call passes a forged JNIEnv*, which the bridge must reject.
int jni32_wrong_env(JNIEnv32* env) {
  GetVersion call = API(env, 4, GetVersion);
  return call((JNIEnv32*)((char*)env + 4));
}

// A genuinely unimplemented JNI function table slot stays zero, not a host
// address truncated to 32 bits. Jumping there is an explicit guest fault.
int jni32_unsupported(JNIEnv32* env) {
  typedef int (*BadCall)(JNIEnv32*);
  return API(env, 5, BadCall)(env);
}

// Named native entry points reached from a 64-bit ART caller after exact
// descriptor-based marshaling into AAPCS32.
int Java_gesher_Test_add(JNIEnv32* env, jobject32 receiver, int a, int b) {
  if (!receiver || API(env, 4, GetVersion)(env) != 0x10006) return -1;
  return a + b + 1;
}
int Java_gesher_Test_length(JNIEnv32* env, jobject32 receiver, jstring32 string) {
  if (!receiver || !string) return -1;
  return API(env, 168, GetStringUTFLength)(env, string);
}
jobject32 Java_gesher_Test_echo(JNIEnv32* env, jobject32 receiver, jobject32 string) {
  if (!receiver || !string) return 0;
  return API(env, 25, NewLocalRef)(env, string);
}
// @CriticalNative has no JNIEnv or managed receiver; AAPCS32 passes its
// 64-bit first argument in r0:r1, followed by the signed int in r2.
long long Java_gesher_Test_critical(long long value, int adjustment) {
  return value + adjustment;
}
