// Run the existing nine native process checks in the ART app process itself.
#include <jni.h>

#define main Exec2Main
#include "../../exec2_test.c"
#undef main
#include "../../exec4_environment.c"

JNIEXPORT jint JNICALL Java_gesher_ExecActivity_nativeEnvironment(JNIEnv* env, jclass type, jstring child) {
  (void)type;
  const char* path = (*env)->GetStringUTFChars(env, child, NULL);
  if (path == NULL) return 100;
  int result = Exec4Environment(path, "java-host-setenv");
  if (result == 0) result = Exec4EnvironmentMutations(path, NULL);
  (*env)->ReleaseStringUTFChars(env, child, path);
  return result;
}

JNIEXPORT jint JNICALL Java_gesher_ExecActivity_nativeProcesses(JNIEnv* env, jclass type, jstring child) {
  (void)type;
  const char* path = (*env)->GetStringUTFChars(env, child, NULL);
  if (path == NULL) return 100;
  char* arguments[] = {(char*)"libexec3jni.so", (char*)path, NULL};
  g_pass = 0;
  g_fail = 0;
  int result = Exec2Main(2, arguments);
  (*env)->ReleaseStringUTFChars(env, child, path);
  return result;
}
