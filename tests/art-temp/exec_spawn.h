// Host test boundary for the actual Windows entry adapter. The tests model
// Win32 replies; real Windows filesystem operations are covered by the DEX test.
#pragma once
#include <cstdint>
#include <string>

using DWORD = std::uint32_t;
constexpr DWORD ERROR_SUCCESS = 0;
constexpr DWORD ERROR_ACCESS_DENIED = 5;
constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;
constexpr DWORD MAX_PATH = 260;
constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xffffffff;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10;

void SetLastError(DWORD error);
DWORD GetLastError();
DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* output, DWORD size);
DWORD GetTempPathW(DWORD size, wchar_t* output);
DWORD GetFileAttributesW(const wchar_t* path);
int SetEnvironmentVariableW(const wchar_t* name, const wchar_t* value);
int _putenv_s(const char* name, const char* value);

namespace gesher::exec {
std::wstring Widen(const std::string& value);
std::string Narrow(const std::wstring& value);
}
