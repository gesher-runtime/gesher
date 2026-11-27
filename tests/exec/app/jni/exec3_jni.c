// Run the existing nine native process checks in the ART app process itself.
// Feature selection must precede jni.h, which includes bionic system headers.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1  // pipe2 and pwritev in the included exec2 corpus
#endif
#include <jni.h>

#define main Exec2Main
#include "../../exec2_test.c"
#undef main

// Diagnose the actual APK's environment checks without changing the shared
// corpus or its result codes. Log errors only; do not dump the environment.
static int Exec4SpawnDiagnostic(pid_t* pid, const char* path,
    const posix_spawn_file_actions_t* actions, const posix_spawnattr_t* attributes,
    char* const arguments[], char* const environment[]) {
  const int error = posix_spawn(pid, path, actions, attributes, arguments, environment);
  if (error)
    fprintf(stderr, "exec4: FAIL environment posix_spawn error=%d mode=%s\n", error,
            arguments && arguments[1] ? arguments[1] : "none");
  return error;
}
static int Exec4ActionsInitDiagnostic(posix_spawn_file_actions_t* actions) {
  const int error = posix_spawn_file_actions_init(actions);
  if (error) fprintf(stderr, "exec4: FAIL environment actions-init error=%d\n", error);
  return error;
}
static int Exec4ActionsDupDiagnostic(posix_spawn_file_actions_t* actions, int from, int to) {
  const int error = posix_spawn_file_actions_adddup2(actions, from, to);
  if (error) fprintf(stderr, "exec4: FAIL environment actions-dup2 error=%d\n", error);
  return error;
}
static int Exec4ActionsCloseDiagnostic(posix_spawn_file_actions_t* actions, int fd) {
  const int error = posix_spawn_file_actions_addclose(actions, fd);
  if (error) fprintf(stderr, "exec4: FAIL environment actions-close error=%d\n", error);
  return error;
}
static pid_t Exec4WaitDiagnostic(pid_t pid, int* status, int options) {
  const pid_t result = waitpid(pid, status, options);
  if (result < 0)
    fprintf(stderr, "exec4: FAIL environment waitpid error=%d\n", errno);
  else if (result > 0 && status && (!WIFEXITED(*status) || WEXITSTATUS(*status) != 0))
    fprintf(stderr, "exec4: FAIL environment child-status=%d\n", *status);
  return result;
}
#define posix_spawn Exec4SpawnDiagnostic
#define posix_spawn_file_actions_init Exec4ActionsInitDiagnostic
#define posix_spawn_file_actions_adddup2 Exec4ActionsDupDiagnostic
#define posix_spawn_file_actions_addclose Exec4ActionsCloseDiagnostic
#define waitpid Exec4WaitDiagnostic
#include "../../exec4_environment.c"
#undef posix_spawn
#undef posix_spawn_file_actions_init
#undef posix_spawn_file_actions_adddup2
#undef posix_spawn_file_actions_addclose
#undef waitpid

JNIEXPORT jint JNICALL Java_gesher_ExecActivity_nativeEnvironment(JNIEnv* env, jclass type, jstring child) {
  (void)type;
  const char* path = (*env)->GetStringUTFChars(env, child, NULL);
  if (path == NULL) return 100;
  int result = Exec4Environment(path, "java-host-setenv");
  if (result != 0)
    fprintf(stderr, "exec4: FAIL environment phase=initial child-result=%d\n", result);
  else {
    result = Exec4EnvironmentMutations(path, NULL);
    if (result != 0)
      fprintf(stderr, "exec4: FAIL environment phase=mutations result=%d\n", result);
  }
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
