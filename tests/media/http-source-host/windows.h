#pragma once
// Declared Win32 boundary for the whole-production HttpSource host fixture.
#include <cstddef>
#include <cstdint>
#include <cwchar>
using DWORD = uint32_t;
using BOOL = int;
using INTERNET_PORT = uint16_t;
struct HttpContractHandle;
using HINTERNET = HttpContractHandle*;
constexpr unsigned CP_UTF8 = 65001;
DWORD GetLastError();
int MultiByteToWideChar(unsigned, DWORD, const char*, int, wchar_t*, int);
inline int64_t _wtoi64(const wchar_t* text) { return std::wcstoll(text, nullptr, 10); }

struct HttpFileHandle;
using HANDLE = HttpFileHandle*;
#define INVALID_HANDLE_VALUE reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1))
constexpr DWORD MAX_PATH = 260;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr DWORD CREATE_NEW = 1, FILE_BEGIN = 0;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80, FILE_FLAG_DELETE_ON_CLOSE = 0x04000000;
constexpr DWORD ERROR_FILE_EXISTS = 80, ERROR_ALREADY_EXISTS = 183;
struct LARGE_INTEGER { int64_t QuadPart; };
DWORD GetTempPathW(DWORD, wchar_t*);
DWORD GetCurrentProcessId();
uint64_t GetTickCount64();
HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE);
BOOL CloseHandle(HANDLE);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, DWORD);
BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
