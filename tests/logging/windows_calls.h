// Forced into production units only. All calls delegate to the real Windows
// API; the fixture counts them and injects one explicit short/failed write.
#pragma once
#include <windows.h>
DWORD WINAPI LoggingTestWait(HANDLE, DWORD);
HANDLE WINAPI LoggingTestOpen(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI LoggingTestSize(HANDLE, LARGE_INTEGER*);
BOOL WINAPI LoggingTestWrite(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
BOOL WINAPI LoggingTestClose(HANDLE);
#define WaitForSingleObject LoggingTestWait
#define CreateFileW LoggingTestOpen
#define GetFileSizeEx LoggingTestSize
#define WriteFile LoggingTestWrite
#define CloseHandle LoggingTestClose
