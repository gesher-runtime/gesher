// Test-only Win32 boundary for executing the complete production logger on Linux.
// Property storage is a separately declared host boundary; Windows uses the
// actual mapped property area and actual kernel files/mutexes instead.
#pragma once
#include <cstdint>
#include <cstddef>
#include <shared_mutex>
using DWORD = uint32_t;
using BOOL = int;
using HANDLE = void*;
using LPVOID = void*;
using LPCVOID = const void*;
using LPCWSTR = const wchar_t*;
using LPDWORD = DWORD*;
using LPSECURITY_ATTRIBUTES = void*;
#define WINAPI
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))
constexpr BOOL FALSE = 0, TRUE = 1;
constexpr DWORD INFINITE = 0xffffffff, WAIT_OBJECT_0 = 0, WAIT_ABANDONED = 0x80;
constexpr DWORD GENERIC_READ = 1, GENERIC_WRITE = 2, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
constexpr DWORD OPEN_ALWAYS = 4, FILE_ATTRIBUTE_NORMAL = 0x80, FILE_BEGIN = 0;
struct SRWLOCK { std::shared_mutex lock; };
#define SRWLOCK_INIT {}
inline void AcquireSRWLockShared(SRWLOCK* value) { value->lock.lock_shared(); }
inline void ReleaseSRWLockShared(SRWLOCK* value) { value->lock.unlock_shared(); }
inline void AcquireSRWLockExclusive(SRWLOCK* value) { value->lock.lock(); }
inline void ReleaseSRWLockExclusive(SRWLOCK* value) { value->lock.unlock(); }
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
struct LARGE_INTEGER { int64_t QuadPart; };
struct SYSTEMTIME { uint16_t wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
struct OVERLAPPED { uintptr_t Internal = 0, InternalHigh = 0; DWORD Offset = 0, OffsetHigh = 0; HANDLE hEvent = nullptr; };
DWORD GetEnvironmentVariableW(LPCWSTR, wchar_t*, DWORD);
BOOL SetEnvironmentVariableW(LPCWSTR, LPCWSTR);
DWORD GetFullPathNameW(LPCWSTR, DWORD, wchar_t*, wchar_t**);
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
DWORD WaitForSingleObject(HANDLE, DWORD);
BOOL ReleaseMutex(HANDLE);
BOOL CloseHandle(HANDLE);
DWORD GetLastError();
HANDLE CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, DWORD);
BOOL SetEndOfFile(HANDLE);
BOOL WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, OVERLAPPED*);
BOOL ReadFile(HANDLE, LPVOID, DWORD, LPDWORD, OVERLAPPED*);
BOOL DeleteFileW(LPCWSTR);
BOOL MoveFileW(LPCWSTR, LPCWSTR);
DWORD GetCurrentProcessId();
DWORD GetCurrentThreadId();
void GetSystemTimePreciseAsFileTime(FILETIME*);
BOOL FileTimeToLocalFileTime(const FILETIME*, FILETIME*);
BOOL FileTimeToSystemTime(const FILETIME*, SYSTEMTIME*);
int _stricmp(const char*, const char*);
