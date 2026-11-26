// Copyright (C) 2026 The Gesher Authors. SPDX-License-Identifier: Apache-2.0
// Host-only Win32/COM call boundary for compat/net/windows_cellular.cc. The ABI and
// layout are separately compiled against the real llvm-mingw headers by the build.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#define STDMETHODCALLTYPE
using DWORD = uint32_t; using ULONG = uint32_t; using LONG = int32_t; using UINT = unsigned;
using HRESULT = int32_t; using WCHAR = char16_t; using OLECHAR = WCHAR;
using LPCWSTR = const WCHAR*; using BSTR = WCHAR*; using LPVOID = void*;
using VARIANT_BOOL = short;
struct GUID { uint32_t Data1; uint16_t Data2, Data3; unsigned char Data4[8]; };
using IID = GUID; using CLSID = GUID; using REFIID = const IID&; using REFCLSID = const CLSID&;
inline bool operator==(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
struct IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
using LPUNKNOWN = IUnknown*;
struct SAFEARRAY;
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define HRESULT_FROM_WIN32(x) static_cast<HRESULT>((x) <= 0 ? (x) : (((x) & 0x0000FFFF) | (7 << 16) | 0x80000000))
constexpr HRESULT S_OK = 0, S_FALSE = 1;
constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005);
constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002);
constexpr HRESULT E_ACCESSDENIED = static_cast<HRESULT>(0x80070005);
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT E_UNEXPECTED = static_cast<HRESULT>(0x8000FFFF);
constexpr HRESULT REGDB_E_CLASSNOTREG = static_cast<HRESULT>(0x80040154);
constexpr HRESULT RPC_E_CHANGED_MODE = static_cast<HRESULT>(0x80010106);
constexpr DWORD ERROR_NOT_FOUND = 1168, ERROR_SERVICE_DISABLED = 1058, ERROR_SERVICE_DOES_NOT_EXIST = 1060;
constexpr DWORD COINIT_MULTITHREADED = 0, CLSCTX_ALL = 23;
HRESULT CoInitializeEx(LPVOID, DWORD);
void CoUninitialize();
HRESULT CoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
void SysFreeString(BSTR);
UINT SysStringLen(BSTR);
HRESULT SafeArrayGetLBound(SAFEARRAY*, UINT, LONG*);
HRESULT SafeArrayGetUBound(SAFEARRAY*, UINT, LONG*);
HRESULT SafeArrayGetElement(SAFEARRAY*, LONG*, void*);
HRESULT SafeArrayDestroy(SAFEARRAY*);
