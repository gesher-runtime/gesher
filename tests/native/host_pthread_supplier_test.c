/* A separate dynamic supplier client, without Gesher's POSIX overlay. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <bcrypt.h>

_Static_assert(sizeof(pthread_t) == 8, "Pinned public pthread identity width changed");

static int LoadedSupplier(const char *expected_sha) {
  wchar_t expected[32768], loaded[32768];
  HMODULE module = GetModuleHandleW(L"libwinpthread-1.dll");
  DWORD exe_length = GetModuleFileNameW(NULL, expected, 32768);
  DWORD dll_length = module ? GetModuleFileNameW(module, loaded, 32768) : 0;
  if (!exe_length || exe_length >= 32768 || !dll_length || dll_length >= 32768) return 3;
  wchar_t *leaf = wcsrchr(expected, L'\\');
  if (!leaf || (size_t)(leaf - expected) + 21 >= 32768) return 3;
  wcscpy(leaf + 1, L"libwinpthread-1.dll");
  if (_wcsicmp(expected, loaded) != 0) return 3;
  HANDLE file = CreateFileW(loaded, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  LARGE_INTEGER size;
  if (file == INVALID_HANDLE_VALUE) return 3;
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
    CloseHandle(file); return 3;
  }
  unsigned char *bytes = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
  DWORD read = 0;
  unsigned char digest[32];
  int ok = bytes && ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL) &&
      read == (DWORD)size.QuadPart &&
      BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, bytes, read, digest, 32) == 0;
  CloseHandle(file);
  if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
  if (!ok) return 3;
  char actual_sha[65];
  for (int i = 0; i != 32; ++i) sprintf(actual_sha + i * 2, "%02x", digest[i]);
  if (strlen(expected_sha) != 64 || _stricmp(actual_sha, expected_sha) != 0) return 3;
  printf("PASS host-authority loaded-supplier path-and-sha256=%s\n", actual_sha);
  return 0;
}

static pthread_key_t exit_key;
static volatile LONG destructor_count;
struct exit_target { HANDLE ready, proceed; pthread_t id; };
static void ExitDestructor(void *value) {
  if (value) InterlockedIncrement(&destructor_count);
}
static DWORD WINAPI ExternalExit(void *arg) {
  struct exit_target *t = arg;
  t->id = pthread_self();
  if (pthread_setspecific(exit_key, t)) abort();
  SetEvent(t->ready);
  if (WaitForSingleObject(t->proceed, 5000) != WAIT_OBJECT_0) abort();
  pthread_exit(NULL);
  return 0;
}
static int DynamicExitControls(void) {
  DWORD baseline, final;
  if (!GetProcessHandleCount(GetCurrentProcess(), &baseline) ||
      pthread_key_create(&exit_key, ExitDestructor)) return 1;
  for (int i = 0; i < 256; ++i) {
    struct exit_target t = { CreateEventW(NULL, TRUE, FALSE, NULL),
                             CreateEventW(NULL, TRUE, FALSE, NULL), 0 };
    HANDLE thread = CreateThread(NULL, 0, ExternalExit, &t, 0, NULL);
    if (!t.ready || !t.proceed || !thread || WaitForSingleObject(t.ready, 5000) != WAIT_OBJECT_0) return 1;
    if ((i & 1) == 0 && pthread_detach(t.id)) return 1;
    SetEvent(t.proceed);
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0) return 1;
    if ((i & 1) && pthread_join(t.id, NULL)) return 1;
    if (destructor_count != i + 1) return 1;
    CloseHandle(thread); CloseHandle(t.ready); CloseHandle(t.proceed);
  }
  if (pthread_key_delete(exit_key) || !GetProcessHandleCount(GetCurrentProcess(), &final) || final > baseline + 8) return 1;
  puts("PASS host-authority dynamic external-exit detach join destructor reuse handle-bound");
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2 || LoadedSupplier(argv[1])) return 3;
  void *handle = NULL;
  unsigned long long generation = 0;
  FILETIME created, exited, kernel, user;
  int result = gesher_winpthread_dup_cpu_handle_v1(pthread_self(), &handle, &generation);
  if (result || !generation || !handle || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT ||
      !GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 1;
  CloseHandle(handle);
  handle = (void *)(uintptr_t)123;
  generation = 456;
  result = gesher_winpthread_dup_cpu_handle_v1((pthread_t)-1, &handle, &generation);
  if (result != ESRCH || handle != (void *)(uintptr_t)123 || generation != 456) return 2;
  if (DynamicExitControls()) return 4;
  puts("PASS host-authority dynamic-export identity ownership invalid-output");
  return 0;
}
