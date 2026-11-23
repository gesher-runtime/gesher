/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * User-scoped DPAPI for the system server. This is at-rest protection and a
 * process-role guard, not a sandbox against arbitrary Windows-user execution.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <jni.h>
#include "../locale/ui_locale.h"

static BOOL server_process = FALSE;

static BOOL Equal(const WCHAR* a, const WCHAR* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static void Fail(JNIEnv* env, const char* type, const char* message) {
  jclass cls = (*env)->FindClass(env, type);
  if (cls != NULL) (*env)->ThrowNew(env, cls, message);
}

static BOOL RequireServer(JNIEnv* env) {
  if (server_process) return TRUE;
  Fail(env, "java/lang/SecurityException", "Account protection is restricted to system_server");
  return FALSE;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm; (void)reserved;
  int count = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (args != NULL) {
    // Only the actual main class, never an app's package/extra argument, authorizes
    // access. ART options precede the first non-option token (apart from -cp value).
    for (int i = 1; i < count; ++i) {
      if (Equal(args[i], L"-cp") || Equal(args[i], L"-classpath")) { ++i; continue; }
      if (args[i][0] == L'-') continue;
      server_process = Equal(args[i], L"com.gesher.server.SystemServer");
      break;
    }
    LocalFree(args);
  }
  return JNI_VERSION_1_6;
}

static jbyteArray Crypt(JNIEnv* env, jbyteArray input, BOOL protect) {
  if (!RequireServer(env)) return NULL;
  if (input == NULL) {
    Fail(env, "java/lang/IllegalArgumentException", "Missing account payload");
    return NULL;
  }
  jsize size = (*env)->GetArrayLength(env, input);
  if (size < 0 || size > 16 * 1024 * 1024) {
    Fail(env, "java/lang/IllegalArgumentException", "Account payload too large");
    return NULL;
  }
  BYTE* bytes = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size > 0 ? (SIZE_T)size : 1);
  if (bytes == NULL) {
    Fail(env, "java/lang/OutOfMemoryError", "Account protection allocation failed");
    return NULL;
  }
  (*env)->GetByteArrayRegion(env, input, 0, size, (jbyte*)bytes);
  if ((*env)->ExceptionCheck(env)) {
    SecureZeroMemory(bytes, (SIZE_T)size); HeapFree(GetProcessHeap(), 0, bytes);
    return NULL;
  }
  DATA_BLOB source = {(DWORD)size, bytes};
  DATA_BLOB output = {0, NULL};
  // Fixed application entropy scopes blobs to this format; DPAPI supplies the key.
  BYTE purpose[] = "Gesher account store v1";
  DATA_BLOB entropy = {(DWORD)(sizeof(purpose) - 1), purpose};
  BOOL ok = protect
      ? CryptProtectData(&source, L"Gesher accounts", &entropy, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &output)
      : CryptUnprotectData(&source, NULL, &entropy, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &output);
  SecureZeroMemory(bytes, (SIZE_T)size);
  HeapFree(GetProcessHeap(), 0, bytes);
  if (!ok) {
    Fail(env, "java/lang/SecurityException", "Windows account protection failed");
    return NULL;
  }
  jbyteArray result = (*env)->NewByteArray(env, (jsize)output.cbData);
  if (result != NULL) {
    (*env)->SetByteArrayRegion(env, result, 0, (jsize)output.cbData, (jbyte*)output.pbData);
  }
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  return result;
}

JNIEXPORT jbyteArray GESHER_NATIVE
Java_com_gesher_server_AccountSecretStore_nativeProtect(JNIEnv* env, jclass cls, jbyteArray input) {
  (void)cls; return Crypt(env, input, TRUE);
}

JNIEXPORT jbyteArray GESHER_NATIVE
Java_com_gesher_server_AccountSecretStore_nativeUnprotect(JNIEnv* env, jclass cls, jbyteArray input) {
  (void)cls; return Crypt(env, input, FALSE);
}

#include "permission_dialog.inc"
#include "exact_alarm_dialog.inc"

JNIEXPORT jboolean GESHER_NATIVE
Java_com_gesher_server_ExactAlarmAccessRequests_nativeAvailable(JNIEnv* env, jclass cls) {
  (void)env; (void)cls;
  return server_process ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint GESHER_NATIVE
Java_com_gesher_server_ExactAlarmAccessRequests_nativeEdit(JNIEnv* env, jclass cls,
        jobjectArray packages, jbooleanArray allowed) {
  (void)cls;
  if (!RequireServer(env) || packages == NULL || allowed == NULL) return -1;
  jsize count = (*env)->GetArrayLength(env, packages);
  if (count != (*env)->GetArrayLength(env, allowed) || count > 10000) return -1;
  WCHAR** names = (WCHAR**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
      ((SIZE_T)count + 1) * sizeof(WCHAR*));
  jboolean* values = (jboolean*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)count + 1);
  int result = -1;
  if (names == NULL || values == NULL) goto done;
  (*env)->GetBooleanArrayRegion(env, allowed, 0, count, values);
  if ((*env)->ExceptionCheck(env)) goto done;
  for (jsize i = 0; i < count; ++i) {
    jstring name = (jstring)(*env)->GetObjectArrayElement(env, packages, i);
    if (name == NULL) goto done;
    jsize length = (*env)->GetStringLength(env, name);
    if (length <= 0 || length > 255) { (*env)->DeleteLocalRef(env, name); goto done; }
    names[i] = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((SIZE_T)length + 1) * sizeof(WCHAR));
    if (names[i] == NULL) { (*env)->DeleteLocalRef(env, name); goto done; }
    (*env)->GetStringRegion(env, name, 0, length, (jchar*)names[i]);
    (*env)->DeleteLocalRef(env, name);
    if ((*env)->ExceptionCheck(env)) goto done;
  }
  result = ExactAlarmDialog(GesherUiCurrentLanguage(), (const WCHAR* const*)names, values, count);
done:
  if (names != NULL) {
    for (jsize i = 0; i < count; ++i) if (names[i] != NULL) HeapFree(GetProcessHeap(), 0, names[i]);
    HeapFree(GetProcessHeap(), 0, names);
  }
  if (values != NULL) HeapFree(GetProcessHeap(), 0, values);
  return result;
}

JNIEXPORT jint GESHER_NATIVE
Java_com_gesher_server_PermissionRequests_nativeConfirm(JNIEnv* env, jclass cls,
                                                       jstring label, jstring permissions) {
  (void)cls;
  if (!RequireServer(env)) return PERMISSION_DISMISSED;
  if (label == NULL || permissions == NULL) return PERMISSION_DISMISSED;
  jsize ln = (*env)->GetStringLength(env, label);
  jsize pn = (*env)->GetStringLength(env, permissions);
  if (ln > 1024 || pn > 8192) return PERMISSION_DISMISSED;
  enum GesherUiLanguage lang = GesherUiCurrentLanguage();
  SIZE_T length = (SIZE_T)ln + (SIZE_T)pn + 2;
  WCHAR* text = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, length * sizeof(WCHAR));
  if (text == NULL) return PERMISSION_DISMISSED;
  WCHAR* permission_text = text + ln + 1;
  (*env)->GetStringRegion(env, label, 0, ln, (jchar*)text);
  (*env)->GetStringRegion(env, permissions, 0, pn, (jchar*)permission_text);
  if ((*env)->ExceptionCheck(env)) { HeapFree(GetProcessHeap(), 0, text); return PERMISSION_DISMISSED; }
  int decision = PermissionDialog(lang, text, permission_text);
  HeapFree(GetProcessHeap(), 0, text);
  return decision;
}

JNIEXPORT jboolean GESHER_NATIVE
Java_com_gesher_server_AccountManagerService_nativeConfirmToken(JNIEnv* env, jclass cls,
        jstring package_name, jstring account_name, jstring token_type) {
  (void)cls;
  if (!RequireServer(env)) return JNI_FALSE;
  if (package_name == NULL || account_name == NULL || token_type == NULL) return JNI_FALSE;
  jsize lengths[3] = {(*env)->GetStringLength(env, package_name),
      (*env)->GetStringLength(env, account_name), (*env)->GetStringLength(env, token_type)};
  if (lengths[0] > 1024 || lengths[1] > 2048 || lengths[2] > 2048) return JNI_FALSE;
  enum GesherUiLanguage lang = GesherUiCurrentLanguage();
  const WCHAR* parts[3] = {GesherUiText(lang, "account_prefix"),
                          GesherUiText(lang, "account_name"), GesherUiText(lang, "account_token")};
  SIZE_T total = (SIZE_T)lengths[0] + (SIZE_T)lengths[1] + (SIZE_T)lengths[2]
      + wcslen(parts[0]) + wcslen(parts[1]) + wcslen(parts[2]) + 7;
  WCHAR* text = (WCHAR*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(WCHAR));
  if (text == NULL) return JNI_FALSE;
  jstring values[3] = {package_name, account_name, token_type};
  SIZE_T at = 0;
  for (int n = 0; n < 3; ++n) {
    const WCHAR* part = parts[n];
    while (*part != 0) text[at++] = *part++;
    text[at++] = 0x2068;
    (*env)->GetStringRegion(env, values[n], 0, lengths[n], (jchar*)(text + at));
    at += (SIZE_T)lengths[n];
    text[at++] = 0x2069;
  }
  if ((*env)->ExceptionCheck(env)) { HeapFree(GetProcessHeap(), 0, text); return JNI_FALSE; }
  // The shared host prompt keeps No as the default, including Esc and close.
  BOOL allowed = GesherHostPrompt(GesherUiText(lang, "account_title"), text,
      GesherUiText(lang, "permission_allow"), GesherUiText(lang, "permission_deny"),
      GesherUiRtl(lang), NULL);
  SecureZeroMemory(text, total * sizeof(WCHAR));
  HeapFree(GetProcessHeap(), 0, text);
  return allowed ? JNI_TRUE : JNI_FALSE;
}

#include "account_chooser.inc"
