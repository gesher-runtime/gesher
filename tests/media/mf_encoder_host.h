// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Declared host adapter for Win32/MF COM calls. Not a codec or Windows runtime.
#pragma once
#include <cstddef>
#include <cstdint>

#define WINAPI
#define STDMETHODCALLTYPE
using BYTE = uint8_t;
using DWORD = uint32_t;
using ULONG = uint32_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using LONGLONG = int64_t;
using HRESULT = int32_t;
using HMODULE = void*;
using FARPROC = void (*)();
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr HRESULT E_NOINTERFACE = -2;
constexpr HRESULT MF_E_NOTACCEPTING = -3;
constexpr HRESULT MF_E_TRANSFORM_NEED_MORE_INPUT = -4;
constexpr HRESULT MF_E_TRANSFORM_STREAM_CHANGE = -5;
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline bool FAILED(HRESULT hr) { return hr < 0; }
struct GUID { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; };
using IID = GUID;
constexpr DWORD COINIT_MULTITHREADED = 0;
constexpr DWORD MFT_ENUM_FLAG_SYNCMFT = 1;
constexpr DWORD MFT_ENUM_FLAG_LOCALMFT = 2;
constexpr DWORD MFT_ENUM_FLAG_SORTANDFILTER = 4;
constexpr DWORD MFT_OUTPUT_STREAM_PROVIDES_SAMPLES = 1;
constexpr DWORD MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES = 2;
enum MFT_MESSAGE_TYPE {
  MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,
  MFT_MESSAGE_NOTIFY_START_OF_STREAM,
  MFT_MESSAGE_NOTIFY_END_OF_STREAM,
  MFT_MESSAGE_COMMAND_DRAIN,
};
constexpr uint16_t VT_UI4 = 19;
struct VARIANT { uint16_t vt; uint32_t ulVal; };
struct IUnknown {
  virtual HRESULT QueryInterface(const IID&, void**) = 0;
  virtual ULONG AddRef() = 0;
  virtual ULONG Release() = 0;
 protected:
  ~IUnknown() = default;
};
struct IMFMediaType : IUnknown {
  virtual HRESULT SetGUID(const GUID&, const GUID&) = 0;
  virtual HRESULT SetUINT32(const GUID&, UINT32) = 0;
  virtual HRESULT SetUINT64(const GUID&, UINT64) = 0;
};
struct IMFMediaBuffer : IUnknown {
  virtual HRESULT Lock(BYTE**, DWORD*, DWORD*) = 0;
  virtual HRESULT Unlock() = 0;
  virtual HRESULT SetCurrentLength(DWORD) = 0;
};
struct IMFSample : IUnknown {
  virtual HRESULT AddBuffer(IMFMediaBuffer*) = 0;
  virtual HRESULT SetSampleTime(LONGLONG) = 0;
  virtual HRESULT SetSampleDuration(LONGLONG) = 0;
  virtual HRESULT GetSampleTime(LONGLONG*) = 0;
  virtual HRESULT GetUINT32(const GUID&, UINT32*) = 0;
  virtual HRESULT ConvertToContiguousBuffer(IMFMediaBuffer**) = 0;
};
struct MFT_REGISTER_TYPE_INFO { GUID guidMajorType, guidSubtype; };
struct MFT_OUTPUT_STREAM_INFO { DWORD dwFlags, cbSize; };
struct MFT_OUTPUT_DATA_BUFFER { DWORD dwStreamID; IMFSample* pSample; DWORD dwStatus; IUnknown* pEvents; };
struct IMFTransform : IUnknown {
  virtual HRESULT SetOutputType(DWORD, IMFMediaType*, DWORD) = 0;
  virtual HRESULT SetInputType(DWORD, IMFMediaType*, DWORD) = 0;
  virtual HRESULT ProcessMessage(MFT_MESSAGE_TYPE, uintptr_t) = 0;
  virtual HRESULT GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO*) = 0;
  virtual HRESULT ProcessOutput(DWORD, DWORD, MFT_OUTPUT_DATA_BUFFER*, DWORD*) = 0;
  virtual HRESULT ProcessInput(DWORD, IMFSample*, DWORD) = 0;
};
struct IMFActivate : IUnknown {
  virtual HRESULT ActivateObject(const IID&, void**) = 0;
};
HMODULE LoadLibraryW(const wchar_t*);
FARPROC GetProcAddress(HMODULE, const char*);
HRESULT CoInitializeEx(void*, DWORD);
void CoTaskMemFree(void*);
