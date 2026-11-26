// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Declared host adapter for Win32/MF COM video decoder calls. Not a codec or Windows runtime.
#pragma once
#include <cstddef>
#include <cstdint>

#define WINAPI
#define STDMETHODCALLTYPE
using BYTE = uint8_t;
using UINT8 = uint8_t;
using SHORT = int16_t;
using WORD = uint16_t;
using INT32 = int32_t;
using LONG = int32_t;
using BOOL = int32_t;
using DWORD = uint32_t;
using ULONG = uint32_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
using LONGLONG = int64_t;
using HRESULT = int32_t;
using HMODULE = void*;
using FARPROC = void (*)();
constexpr BOOL TRUE = 1;
constexpr BOOL FALSE = 0;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
constexpr HRESULT E_NOINTERFACE = -2;
constexpr HRESULT MF_E_NOTACCEPTING = -3;
constexpr HRESULT MF_E_TRANSFORM_NEED_MORE_INPUT = -4;
constexpr HRESULT MF_E_TRANSFORM_STREAM_CHANGE = -5;
constexpr HRESULT MF_E_TRANSFORM_TYPE_NOT_SET = -6;
constexpr HRESULT MF_E_NO_MORE_TYPES = -7;
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline bool FAILED(HRESULT hr) { return hr < 0; }
struct GUID { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; };
using IID = GUID;
inline bool IsEqualGUID(const GUID& a, const GUID& b) {
  return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
         a.Data4[0] == b.Data4[0] && a.Data4[1] == b.Data4[1] &&
         a.Data4[2] == b.Data4[2] && a.Data4[3] == b.Data4[3] &&
         a.Data4[4] == b.Data4[4] && a.Data4[5] == b.Data4[5] &&
         a.Data4[6] == b.Data4[6] && a.Data4[7] == b.Data4[7];
}
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
  MFT_MESSAGE_COMMAND_FLUSH,
  MFT_MESSAGE_COMMAND_DRAIN,
};
struct MFOffset { WORD fract; SHORT value; };
struct SIZE { LONG cx, cy; };
struct MFVideoArea { MFOffset OffsetX, OffsetY; SIZE Area; };
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
  virtual HRESULT SetBlob(const GUID&, const UINT8*, UINT32) = 0;
  virtual HRESULT GetGUID(const GUID&, GUID*) = 0;
  virtual HRESULT GetUINT32(const GUID&, UINT32*) = 0;
  virtual HRESULT GetUINT64(const GUID&, UINT64*) = 0;
  virtual HRESULT GetBlob(const GUID&, UINT8*, UINT32, UINT32*) = 0;
};
struct IMFMediaBuffer : IUnknown {
  virtual HRESULT Lock(BYTE**, DWORD*, DWORD*) = 0;
  virtual HRESULT Unlock() = 0;
  virtual HRESULT SetCurrentLength(DWORD) = 0;
};
struct IMF2DBuffer : IUnknown {
  virtual HRESULT Lock2D(BYTE**, LONG*) = 0;
  virtual HRESULT Unlock2D() = 0;
};
struct IMFSample : IUnknown {
  virtual HRESULT AddBuffer(IMFMediaBuffer*) = 0;
  virtual HRESULT GetBufferCount(DWORD*) = 0;
  virtual HRESULT GetBufferByIndex(DWORD, IMFMediaBuffer**) = 0;
  virtual HRESULT DeleteAllItems() = 0;
  virtual HRESULT SetSampleTime(LONGLONG) = 0;
  virtual HRESULT SetSampleDuration(LONGLONG) = 0;
  virtual HRESULT GetSampleTime(LONGLONG*) = 0;
  virtual HRESULT SetUINT32(const GUID&, UINT32) = 0;
};
struct MFT_REGISTER_TYPE_INFO { GUID guidMajorType, guidSubtype; };
struct MFT_OUTPUT_STREAM_INFO { DWORD dwFlags, cbSize; };
struct MFT_OUTPUT_DATA_BUFFER { DWORD dwStreamID; IMFSample* pSample; DWORD dwStatus; IUnknown* pEvents; };
struct IMFTransform : IUnknown {
  virtual HRESULT GetOutputAvailableType(DWORD, DWORD, IMFMediaType**) = 0;
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

struct AMediaFormat;
void AMediaFormat_setBuffer(AMediaFormat* format, const char* name, const void* data, size_t size);
