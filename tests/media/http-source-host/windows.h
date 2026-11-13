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
