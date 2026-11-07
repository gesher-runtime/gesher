/*
 * Copyright (C) 2026 The Gesher Authors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A server-local duplex pipe whose ordinary Windows HANDLEs survive Binder's
 * existing fd transfer. No socket fd or process-local socket table is involved.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <jni.h>

static BOOL server_process = FALSE;

static BOOL Equal(const WCHAR* a, const WCHAR* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

static void Fail(JNIEnv* env, const char* type, const char* message) {
  jclass cls = (*env)->FindClass(env, type);
  if (cls != NULL) (*env)->ThrowNew(env, cls, message);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  (void)vm; (void)reserved;
  int count = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
  if (args != NULL) {
    // Same process-role guard as account_crypto: only the actual ART entry class,
    // never an app extra, mutable environment variable or claimed package name.
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

static BOOL UserAcl(PACL* acl) {
  HANDLE token = NULL;
  TOKEN_USER* user = NULL;
  DWORD size = 0;
  BOOL ok = FALSE;
  *acl = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
  if (GetTokenInformation(token, TokenUser, NULL, 0, &size)
      || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 65536) goto done;
  user = (TOKEN_USER*)HeapAlloc(GetProcessHeap(), 0, size);
  if (user == NULL || !GetTokenInformation(token, TokenUser, user, size, &size)
      || !IsValidSid(user->User.Sid)) goto done;
  EXPLICIT_ACCESSW access = {0};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = (LPWSTR)user->User.Sid;
  ok = SetEntriesInAclW(1, &access, NULL, acl) == ERROR_SUCCESS;
done:
  if (user != NULL) HeapFree(GetProcessHeap(), 0, user);
  CloseHandle(token);
  return ok;
}

static BOOL NonceAcross(HANDLE from, HANDLE to, const BYTE nonce[32]) {
  BYTE received[32] = {0};
  DWORD wrote = 0, available = 0, read = 0;
  BOOL ok = WriteFile(from, nonce, 32, &wrote, NULL) && wrote == 32
      && PeekNamedPipe(to, NULL, 0, NULL, &available, NULL) && available >= 32
      && ReadFile(to, received, 32, &read, NULL) && read == 32;
  BYTE different = 0;
  for (DWORD i = 0; i < 32; ++i) different |= (BYTE)(received[i] ^ nonce[i]);
  SecureZeroMemory(received, sizeof(received));
  return ok && different == 0;
}

/* Creates both endpoints privately in this process. No pipe name/nonce/handle is
 * published until both kernel endpoint PIDs and the round-trip nonce are checked.
 * CreateFile precedes ConnectNamedPipe, so the connect cannot await an outsider.
 * The tiny nonce writes fit in the fresh 64 KiB buffers; Peek avoids an empty read.
 */
static BOOL CreatePair(int result[2]) {
  const WCHAR prefix[] = L"\\\\.\\pipe\\Gesher.InstallPipe.v1.";
  const WCHAR hex[] = L"0123456789abcdef";
  WCHAR name[sizeof(prefix) / sizeof(prefix[0]) + 64] = {0};
  BYTE random[32] = {0}, nonce[32] = {0};
  PACL acl = NULL;
  SECURITY_DESCRIPTOR descriptor;
  HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE;
  int server_fd = -1, client_fd = -1;
  BOOL ok = FALSE;
  result[0] = result[1] = -1;
  if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
      || !BCRYPT_SUCCESS(BCryptGenRandom(NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
      || !UserAcl(&acl)) goto done;
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION)
      || !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)
      || !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) goto done;
  SIZE_T prefix_length = sizeof(prefix) / sizeof(prefix[0]) - 1;
  for (SIZE_T i = 0; i < prefix_length; ++i) name[i] = prefix[i];
  for (SIZE_T i = 0; i < sizeof(random); ++i) {
    name[prefix_length + 2 * i] = hex[random[i] >> 4];
    name[prefix_length + 2 * i + 1] = hex[random[i] & 15];
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), &descriptor, FALSE};
  server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      1, 65536, 65536, 0, &attributes);
  if (server == INVALID_HANDLE_VALUE) goto done;
  client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
  if (client == INVALID_HANDLE_VALUE) goto done;
  if (!ConnectNamedPipe(server, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) goto done;
  ULONG client_pid = 0, server_pid = 0;
  if (!GetNamedPipeClientProcessId(server, &client_pid)
      || !GetNamedPipeServerProcessId(client, &server_pid)
      || client_pid != GetCurrentProcessId() || server_pid != GetCurrentProcessId()
      || !NonceAcross(client, server, nonce) || !NonceAcross(server, client, nonce)
      || !SetHandleInformation(server, HANDLE_FLAG_INHERIT, 0)
      || !SetHandleInformation(client, HANDLE_FLAG_INHERIT, 0)) goto done;
  server_fd = _open_osfhandle((intptr_t)server, _O_RDWR | _O_BINARY);
  if (server_fd < 0) goto done;
  server = INVALID_HANDLE_VALUE;  // the CRT owns it now
  client_fd = _open_osfhandle((intptr_t)client, _O_RDWR | _O_BINARY);
  if (client_fd < 0) goto done;
  client = INVALID_HANDLE_VALUE;
  result[0] = server_fd; result[1] = client_fd;
  server_fd = client_fd = -1;
  ok = TRUE;
done:
  if (server_fd >= 0) _close(server_fd);
  if (client_fd >= 0) _close(client_fd);
  if (server != INVALID_HANDLE_VALUE) CloseHandle(server);
  if (client != INVALID_HANDLE_VALUE) CloseHandle(client);
  if (acl != NULL) LocalFree(acl);
  SecureZeroMemory(random, sizeof(random));
  SecureZeroMemory(nonce, sizeof(nonce));
  SecureZeroMemory(name, sizeof(name));
  return ok;
}

JNIEXPORT jintArray GESHER_NATIVE
Java_com_gesher_server_InstallPipeTransport_nativeCreatePipe(JNIEnv* env, jclass cls) {
  (void)cls;
  if (!server_process) {
    Fail(env, "java/lang/SecurityException", "Installation transport is restricted to system_server");
    return NULL;
  }
  int descriptors[2];
  if (!CreatePair(descriptors)) {
    Fail(env, "java/io/IOException", "Cannot create protected installation transport");
    return NULL;
  }
  jintArray result = (*env)->NewIntArray(env, 2);
  if (result != NULL) {
    jint values[2] = {descriptors[0], descriptors[1]};
    (*env)->SetIntArrayRegion(env, result, 0, 2, values);
  }
  if (result == NULL || (*env)->ExceptionCheck(env)) {
    _close(descriptors[0]); _close(descriptors[1]); return NULL;
  }
  return result;
}

/* The CRT holds a descriptor lock during synchronous pipe reads/writes. Closing
 * that descriptor from another thread can therefore wait indefinitely. The Java
 * owner serializes this call against descriptor retirement and repeats it until
 * its writer exits, covering cancellation just before an I/O operation begins.
 * CancelIoEx does not wait for the CRT lock or for the canceled operation to end.
 */
JNIEXPORT jboolean GESHER_NATIVE
Java_com_gesher_server_InstallPipeTransport_nativeCancelIo(JNIEnv* env, jclass cls, jint fd) {
  (void)cls;
  if (!server_process) {
    Fail(env, "java/lang/SecurityException", "Installation transport is restricted to system_server");
    return JNI_FALSE;
  }
  if (fd < 0) return JNI_FALSE;
  intptr_t handle = _get_osfhandle(fd);
  if (handle == -1 || handle == -2) return JNI_FALSE;
  if (CancelIoEx((HANDLE)handle, NULL)) return JNI_TRUE;
  return GetLastError() == ERROR_NOT_FOUND ? JNI_TRUE : JNI_FALSE;
}
