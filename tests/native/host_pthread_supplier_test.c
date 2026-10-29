/* A separate dynamic supplier client, without Gesher's POSIX overlay. */
#include <pthread.h>
#include <windows.h>
#include <stdio.h>
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

typedef int (__cdecl *clock_id_fn)(pthread_t, clockid_t *);
typedef int (__cdecl *clock_time_fn)(clockid_t, struct timespec *);
static clock_id_fn public_clock_id;
static volatile LONG ready, stop;
static clockid_t worker_clock;
static DWORD WINAPI BusyWorker(void *unused) {
  (void)unused;
  if (public_clock_id(pthread_self(), &worker_clock) != 0) return 1;
  InterlockedExchange(&ready, 1);
  while (!InterlockedCompareExchange(&stop, 0, 0)) { }
  return 0;
}
static uint64_t ticks(FILETIME t) { return ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime; }
static uint64_t cpu(HANDLE h) {
  FILETIME c, e, k, u;
  if (!GetThreadTimes(h, &c, &e, &k, &u)) return UINT64_MAX;
  return ticks(k) + ticks(u);
}
static int PublicClockControl(void) {
  HMODULE dll = GetModuleHandleW(L"libwinpthread-1.dll");
  public_clock_id = (clock_id_fn)GetProcAddress(dll, "pthread_getcpuclockid");
  clock_time_fn gettime = (clock_time_fn)GetProcAddress(dll, "clock_gettime");
  clock_time_fn getres = (clock_time_fn)GetProcAddress(dll, "clock_getres");
  if (!public_clock_id || !gettime || !getres ||
      !GetProcAddress(dll, "clock_gettime32") || !GetProcAddress(dll, "clock_gettime64") ||
      !GetProcAddress(dll, "clock_getres32") || !GetProcAddress(dll, "clock_getres64")) return 10;
  HANDLE worker = CreateThread(NULL, 0, BusyWorker, NULL, 0, NULL);
  if (!worker) return 11;
  while (!InterlockedCompareExchange(&ready, 0, 0)) {
    if (WaitForSingleObject(worker, 1) == WAIT_OBJECT_0) return 12;
  }
  uint64_t before = cpu(worker);
  struct timespec sample, resolution;
  if (before == UINT64_MAX || gettime(worker_clock, &sample) || getres(worker_clock, &resolution)) return 13;
  uint64_t after = cpu(worker);
  uint64_t measured = (uint64_t)sample.tv_sec * 10000000 + (uint64_t)sample.tv_nsec / 100;
  if (after == UINT64_MAX || measured < before || measured > after ||
      resolution.tv_sec != 0 || resolution.tv_nsec != 100) return 14;
  InterlockedExchange(&stop, 1);
  if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0) return 15;
  DWORD exit_code;
  if (!GetExitCodeThread(worker, &exit_code) || exit_code != 0 || !CloseHandle(worker)) return 16;
  sample.tv_sec = 123; sample.tv_nsec = 456;
  if (gettime(worker_clock, &sample) != -1 || errno != EINVAL || sample.tv_sec != 123 || sample.tv_nsec != 456) return 17;
  puts("PASS public-clock DLL self-ID cross-thread GetThreadTimes bracket and retired ID");
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2 || LoadedSupplier(argv[1])) return 3;
  int public_result = PublicClockControl();
  if (public_result) return public_result;
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
  puts("PASS host-authority dynamic-export identity ownership invalid-output");
  return 0;
}
