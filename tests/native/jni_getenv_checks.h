// Checks through an actual guest JavaVM table in JniTest and Arm64Jni.
#pragma once

#include <jni.h>
#include <stdint.h>
#include <stdio.h>

static int jni_getenv_supported_versions(JavaVM* vm, JNIEnv* expected) {
  const jint versions[] = {JNI_VERSION_1_1, JNI_VERSION_1_2, JNI_VERSION_1_4, JNI_VERSION_1_6};
  int ok = 1;
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
    JNIEnv* env = NULL;
    jint result = (*vm)->GetEnv(vm, (void**)&env, versions[i]);
    if (result != JNI_OK || env != expected) {
      printf("    GetEnv 0x%08x: result=%d, same proxy=%d\n",
             (uint32_t)versions[i], result, env == expected);
      ok = 0;
    }
  }
  return ok;
}

static int jni_getenv_unsupported_interfaces(JavaVM* vm) {
  // JVMTI 1.0/1.1/1.2/9/11/21 from jvmti.h; ART's best-effort tooling ID
  // from openjdkjvmti/art_jvmti.h; unknown IDs; newer standard JNI families.
  // None has a guest ABI bridge. Never dereference a rejected raw pointer.
  const jint versions[] = {
      0x30010000, 0x30010100, 0x30010200, 0x30090000, 0x300b0000, 0x30150000,
      0x70010200, 0x10010006, 0x40010006, 0x00000000, 0x00020000, 0x00090000,
      0x00150000, 0x7fffffff, (jint)0x80010006u, (jint)0xffffffffu};
  int ok = 1;
  for (int repeat = 0; repeat < 4; ++repeat) {
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
      void* env = vm;  // A non-null value which GetEnv must clear.
      jint result = (*vm)->GetEnv(vm, &env, versions[i]);
      if (result != JNI_EVERSION || env != NULL) {
        printf("    GetEnv unsupported 0x%08x: result=%d, null=%d\n",
               (uint32_t)versions[i], result, env == NULL);
        ok = 0;
      }
    }
  }
  return ok;
}

static int jni_getenv_unknown_jni_versions(JavaVM* vm) {
  // Android's host VM rejects these individual versions inside the bridged
  // 0x0001xxxx family; the bridge must preserve its error and clear output.
  const jint versions[] = {0x00010000, 0x00010003, 0x00010008, 0x0001ffff};
  int ok = 1;
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i) {
    void* env = vm;
    jint result = (*vm)->GetEnv(vm, &env, versions[i]);
    if (result != JNI_EVERSION || env != NULL) {
      printf("    GetEnv unknown JNI 0x%08x: result=%d, null=%d\n",
             (uint32_t)versions[i], result, env == NULL);
      ok = 0;
    }
  }
  return ok;
}

static int jni_getenv_contracts(JavaVM* vm, JNIEnv* expected) {
  int ok = jni_getenv_unsupported_interfaces(vm);
  ok &= jni_getenv_unknown_jni_versions(vm);
  ok &= jni_getenv_supported_versions(vm, expected);
  if (ok) puts("GetEnv guest boundary: all passed");
  return ok;
}

static int jni_getenv_detached(JavaVM* vm) {
  void* env = vm;
  jint result = (*vm)->GetEnv(vm, &env, JNI_VERSION_1_6);
  if (result == JNI_EDETACHED && env == NULL) return 1;
  printf("    GetEnv detached: result=%d, null=%d\n", result, env == NULL);
  return 0;
}
