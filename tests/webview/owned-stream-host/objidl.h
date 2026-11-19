// Test-only COM ABI seam for the portable production-source stream fixture.
#pragma once
#include <cstdint>

#define STDMETHODCALLTYPE
using HRESULT = int32_t;
using ULONG = uint32_t;
using DWORD = uint32_t;
using UINT = uint32_t;
using BYTE = uint8_t;
using ULONGLONG = uint64_t;
using LPCWSTR = const wchar_t*;
struct LARGE_INTEGER { int64_t QuadPart; };
struct ULARGE_INTEGER { uint64_t QuadPart; };
struct IID { uint32_t value; };
using REFIID = const IID&;
inline bool IsEqualIID(REFIID a, REFIID b) { return a.value == b.value; }
inline constexpr IID IID_IUnknown{1}, IID_ISequentialStream{2}, IID_IStream{3},
    IID_IAgileObject{4}, IID_IMarshal{5};
inline constexpr HRESULT S_OK = 0, S_FALSE = 1;
inline constexpr HRESULT E_POINTER = static_cast<HRESULT>(0x80004003u);
inline constexpr HRESULT E_NOINTERFACE = static_cast<HRESULT>(0x80004002u);
inline constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000eu);
inline constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);
inline constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057u);
inline constexpr HRESULT STG_E_INVALIDFUNCTION = static_cast<HRESULT>(0x80030001u);
inline constexpr HRESULT STG_E_ACCESSDENIED = static_cast<HRESULT>(0x80030005u);
inline constexpr HRESULT STG_E_INSUFFICIENTMEMORY = static_cast<HRESULT>(0x80030008u);
inline constexpr HRESULT STG_E_INVALIDPOINTER = static_cast<HRESULT>(0x80030009u);
inline constexpr HRESULT STG_E_WRITEFAULT = static_cast<HRESULT>(0x8003001du);
inline constexpr HRESULT STG_E_MEDIUMFULL = static_cast<HRESULT>(0x80030070u);
inline constexpr HRESULT STG_E_INVALIDFLAG = static_cast<HRESULT>(0x800300ffu);
inline bool SUCCEEDED(HRESULT hr) { return hr >= 0; }
inline bool FAILED(HRESULT hr) { return hr < 0; }
inline constexpr DWORD STREAM_SEEK_SET = 0, STREAM_SEEK_CUR = 1, STREAM_SEEK_END = 2;
inline constexpr DWORD STGTY_STREAM = 2, STGM_READ = 0;
inline constexpr DWORD STATFLAG_DEFAULT = 0, STATFLAG_NONAME = 1, STATFLAG_NOOPEN = 2;
struct STATSTG {
  wchar_t* pwcsName;
  DWORD type;
  ULARGE_INTEGER cbSize;
  uint64_t mtime, ctime, atime;
  DWORD grfMode, grfLocksSupported;
  IID clsid;
  DWORD grfStateBits, reserved;
};
struct IUnknown {
  virtual HRESULT QueryInterface(REFIID, void**) = 0;
  virtual ULONG AddRef() = 0;
  virtual ULONG Release() = 0;
};
struct IAgileObject : IUnknown {};
struct ISequentialStream : IUnknown {
  virtual HRESULT Read(void*, ULONG, ULONG*) = 0;
  virtual HRESULT Write(const void*, ULONG, ULONG*) = 0;
};
struct IStream : ISequentialStream {
  virtual HRESULT Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) = 0;
  virtual HRESULT SetSize(ULARGE_INTEGER) = 0;
  virtual HRESULT CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) = 0;
  virtual HRESULT Commit(DWORD) = 0;
  virtual HRESULT Revert() = 0;
  virtual HRESULT LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) = 0;
  virtual HRESULT UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) = 0;
  virtual HRESULT Stat(STATSTG*, DWORD) = 0;
  virtual HRESULT Clone(IStream**) = 0;
};
