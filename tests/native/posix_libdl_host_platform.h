// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Explicit Win32 dispatch seam. No real PE loading or Windows execution.
#pragma once
#include <cstddef>
#include <string>
using DWORD = unsigned long;
using BOOL = int;
using HANDLE = void*;
using HMODULE = void*;
using LPCWSTR = const wchar_t*;
using FARPROC = int (*)();
constexpr DWORD LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x1000;
constexpr DWORD LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x100;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
constexpr DWORD MAX_PATH = 260;
inline int pe_loads, pe_symbols, pe_closes;
inline char main_module, pe_module;
inline int PeSymbol() { return 91; }
inline DWORD GetLastError() { return 2; }
inline HANDLE GetCurrentProcess() { return &main_module; }
inline HMODULE GetModuleHandleW(const wchar_t* path) { return path ? &pe_module : &main_module; }
inline HMODULE LoadLibraryExW(const wchar_t*, void*, DWORD) { ++pe_loads; return &pe_module; }
inline FARPROC GetProcAddress(HMODULE handle, const char*) {
  ++pe_symbols;
  return handle == &pe_module ? &PeSymbol : nullptr;
}
inline BOOL FreeLibrary(HMODULE) { ++pe_closes; return 1; }
inline BOOL EnumProcessModules(HANDLE, HMODULE*, DWORD, DWORD* needed) { *needed = 0; return 1; }
inline BOOL GetModuleHandleExW(DWORD, LPCWSTR, HMODULE*) { return 0; }
inline DWORD GetModuleFileNameW(HMODULE, wchar_t*, DWORD) { return 0; }
namespace gesher {
inline std::wstring Widen(const char* text) {
  const std::string value(text);
  return std::wstring(value.begin(), value.end());
}
inline std::string Narrow(const wchar_t* text) {
  const std::wstring value(text);
  return std::string(value.begin(), value.end());
}
}  // namespace gesher
