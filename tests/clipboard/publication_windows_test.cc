/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Actual product implementation and Win32/OLE regression. The two compile-time
// wrappers below add an explicitly isolated competing-writer/error boundary;
// all normal operations still call the actual Windows API. No test hook ships
// in the product library, and no public HWND, message or token grants authority.
#undef NOGDI
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <thread>
#include <set>
HRESULT PublicationTestSetClipboard(IDataObject* object);
HRESULT PublicationTestFlushClipboard();
#define OleSetClipboard PublicationTestSetClipboard
#define OleFlushClipboard PublicationTestFlushClipboard
#include "../../compat/windows/clipboard_publication.cc"
#undef OleSetClipboard
#undef OleFlushClipboard
#include "../../compat/windows/clipboard_bridge.h"
#include "wic_decode.h"

namespace clipboard_test {
std::atomic<int> checks{0}, failures{0};
unsigned case_number = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAIL CLIPBOARD_PUBLICATION case=%u %s error=%lu\n", case_number, what,
                static_cast<unsigned long>(GetLastError()));
  }
}
bool PumpWait(HANDLE event, DWORD timeout) { return gesher::windows::WaitPumping(event, timeout); }
bool Open(HWND owner, DWORD* error = nullptr) {
  for (int attempt = 0; attempt != 20; ++attempt) {
    if (error) SetLastError(ERROR_SUCCESS);
    const BOOL opened = OpenClipboard(owner);
    if (error) *error = GetLastError();
    if (opened) return true;
    Sleep(10);
  }
  return false;
}
UINT HtmlFormat() { return RegisterClipboardFormatW(L"HTML Format"); }
std::vector<uint8_t> Bytes(const void* value, size_t size) {
  const auto* p = static_cast<const uint8_t*>(value);
  return {p, p + size};
}
std::vector<uint8_t> WideBytes(const std::wstring& value) {
  return Bytes(value.c_str(), (value.size() + 1) * sizeof(wchar_t));
}
std::vector<uint8_t> NarrowBytes(const std::string& value) { return Bytes(value.c_str(), value.size() + 1); }
const wchar_t* OwnText() { return L"Android OLE publication \x05D0\x05D1"; }
const wchar_t* OtherText() { return L"external different content"; }
const char* OwnHtml() { return "<b>Android OLE publication</b>"; }
const char* OtherHtml() { return "<i>external different HTML</i>"; }
std::u16string Text() { return std::u16string(reinterpret_cast<const char16_t*>(OwnText())); }
std::u16string Markup() { return u"<b>Android OLE publication</b>"; }
std::string Html(const std::string& text) {
  const char* fmt = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
                    "StartFragment:%010zu\r\nEndFragment:%010zu\r\n";
  const std::string before = "<html><body>\r\n<!--StartFragment-->";
  const std::string after = "<!--EndFragment-->\r\n</body>\r\n</html>";
  char line[256];
  const size_t n = static_cast<size_t>(std::snprintf(line, sizeof(line), fmt,
                                                    size_t{0}, size_t{0}, size_t{0}, size_t{0}));
  const size_t start = n + before.size(), end = start + text.size();
  std::snprintf(line, sizeof(line), fmt, n, end + after.size(), start, end);
  return std::string(line) + before + text + after;
}
std::vector<uint8_t> Dib() {
  BITMAPINFOHEADER h{};
  h.biSize = sizeof(h); h.biWidth = 2; h.biHeight = 1; h.biPlanes = 1;
  h.biBitCount = 32; h.biCompression = BI_RGB; h.biSizeImage = 8;
  std::vector<uint8_t> bytes(sizeof(h) + 8);
  std::memcpy(bytes.data(), &h, sizeof(h));
  const uint8_t pixels[] = {0x31,0x52,0x73,0,0x97,0xb9,0xdb,0};
  std::memcpy(bytes.data() + sizeof(h), pixels, sizeof(pixels));
  return bytes;
}
std::vector<uint8_t> Raw(UINT format) {
  if (!IsClipboardFormatAvailable(format) || !Open(nullptr)) return {};
  HANDLE value = GetClipboardData(format);
  std::vector<uint8_t> out;
  const SIZE_T size = value ? GlobalSize(value) : 0;
  const void* data = size ? GlobalLock(value) : nullptr;
  if (data) { out = Bytes(data, size); GlobalUnlock(value); }
  Check(CloseClipboard() != 0, "actual reader closes its clipboard access");
  return out;
}
struct Snapshot {
  std::vector<uint8_t> text, html, image;
  UINT image_format = 0;
  DWORD sequence = 0;
};
Snapshot Read() {
  Snapshot out;
  out.text = Raw(CF_UNICODETEXT); out.html = Raw(HtmlFormat());
  out.image_format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
      : (IsClipboardFormatAvailable(CF_DIB) ? CF_DIB : 0);
  if (out.image_format) out.image = Raw(out.image_format);
  out.sequence = GetClipboardSequenceNumber();
  return out;
}
bool Same(const Snapshot& a, const Snapshot& b) {
  return a.text == b.text && a.html == b.html && a.image == b.image
      && a.image_format == b.image_format;
}
void ClearFixture() {
  Check(Open(nullptr), "isolated fixture opens clipboard for cleanup");
  Check(EmptyClipboard() != 0, "isolated fixture removes its own prior test data");
  Check(CloseClipboard() != 0, "isolated cleanup closes clipboard");
}
void ReleaseFormat(UINT format, HANDLE value) {
  if (!value) return;
  STGMEDIUM medium{};
  if (format == CF_BITMAP || format == CF_PALETTE) {
    medium.tymed = TYMED_GDI; medium.hBitmap = static_cast<HBITMAP>(value);
  } else if (format == CF_ENHMETAFILE) {
    medium.tymed = TYMED_ENHMF; medium.hEnhMetaFile = static_cast<HENHMETAFILE>(value);
  } else if (format == CF_METAFILEPICT) {
    medium.tymed = TYMED_MFPICT; medium.hMetaFilePict = static_cast<HMETAFILEPICT>(value);
  } else {
    medium.tymed = TYMED_HGLOBAL; medium.hGlobal = static_cast<HGLOBAL>(value);
  }
  ReleaseStgMedium(&medium);
}
struct OwnedFormat {
  UINT format = 0;
  HANDLE value = nullptr;
  OwnedFormat(UINT f, HANDLE v) : format(f), value(v) {}
  OwnedFormat(const OwnedFormat&) = delete;
  OwnedFormat& operator=(const OwnedFormat&) = delete;
  OwnedFormat(OwnedFormat&& other) noexcept : format(other.format), value(other.value) { other.value = nullptr; }
  OwnedFormat& operator=(OwnedFormat&& other) noexcept {
    if (this != &other) {
      ReleaseFormat(format, value); format = other.format; value = other.value; other.value = nullptr;
    }
    return *this;
  }
  ~OwnedFormat() { ReleaseFormat(format, value); }
};
HANDLE MakeGlobal(const std::vector<uint8_t>& bytes) {
  HGLOBAL value = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
  if (!value) return nullptr;
  void* data = GlobalLock(value);
  if (!data) { GlobalFree(value); return nullptr; }
  std::memcpy(data, bytes.data(), bytes.size());
  GlobalUnlock(value);
  return value;
}
struct Observation {
  DWORD pid = 0, error = 0;
  LONG ready = 0, opened = 0, emptied = 0, published = 0, closed = 0;
  LONG read_opened = 0, read_closed = 0;
  LONG formats_seen = 0, formats_copied = 0, copy_failed = 0, formats_set = 0;
  unsigned long long owner_before = 0, owner_after = 0;
  UINT format_ids[128]{};
  DWORD format_errors[128]{};
  LONG format_duplicated[128]{};
};
void FirstError(Observation* result) {
  if (!result->error) result->error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
}
bool CopyFormats(std::vector<OwnedFormat>* formats, Observation* result) {
  if (!Open(nullptr)) { FirstError(result); return false; }
  result->read_opened = 1;
  bool ok = true;
  UINT format = 0;
  for (;;) {
    SetLastError(ERROR_SUCCESS);
    format = EnumClipboardFormats(format);
    if (!format) {
      if (GetLastError() != ERROR_SUCCESS) { FirstError(result); ok = false; }
      break;
    }
    ++result->formats_seen;
    if (result->formats_seen > 128) { SetLastError(ERROR_INVALID_DATA); FirstError(result); ok = false; break; }
    SetLastError(ERROR_SUCCESS);
    HANDLE original = GetClipboardData(format);
    const DWORD read_error = GetLastError();
    SetLastError(ERROR_SUCCESS);
    HANDLE copy = original ? OleDuplicateData(original, static_cast<CLIPFORMAT>(format), 0) : nullptr;
    const DWORD duplicate_error = GetLastError();
    const auto index = static_cast<size_t>(result->formats_seen - 1);
    result->format_ids[index] = format;
    result->format_errors[index] = original ? duplicate_error : read_error;
    result->format_duplicated[index] = copy != nullptr;
    wchar_t name[256]{};
    GetClipboardFormatNameW(format, name, 256);
    std::printf("OLE_REPLAY_FORMAT format=%u available=%d duplicate=%d error=%lu\n",
                format, original != nullptr, copy != nullptr,
                static_cast<unsigned long>(result->format_errors[index]));
    if (!copy) {
      ++result->copy_failed; SetLastError(result->format_errors[index]);
      FirstError(result); ok = false;
    }
    else { formats->emplace_back(format, copy); ++result->formats_copied; }
  }
  result->read_closed = CloseClipboard() != 0;
  if (!result->read_closed) { FirstError(result); ok = false; }
  return ok;
}
bool ReplaceBytes(std::vector<OwnedFormat>* formats, UINT format, const std::vector<uint8_t>& bytes) {
  HANDLE value = MakeGlobal(bytes);
  if (!value) return false;
  for (auto& current : *formats) {
    if (current.format == format) {
      ReleaseFormat(current.format, current.value); current.value = value; return true;
    }
  }
  formats->emplace_back(format, value);
  return true;
}
bool MutatePreferredImage(std::vector<OwnedFormat>* formats, int target) {
  UINT preferred = 0;
  for (const auto& value : *formats) {
    if (value.format == CF_DIBV5) preferred = CF_DIBV5;
    else if (!preferred && value.format == CF_DIB) preferred = CF_DIB;
  }
  for (auto& value : *formats) {
    if (value.format != preferred) continue;
    SIZE_T size = GlobalSize(value.value);
    auto* data = static_cast<uint8_t*>(GlobalLock(value.value));
    if (!data || size < sizeof(BITMAPINFOHEADER)) { if (data) GlobalUnlock(value.value); return false; }
    BITMAPINFOHEADER header{};
    std::memcpy(&header, data, sizeof(header));
    size_t offset = header.biSize;
    if (header.biSize == sizeof(header) && header.biCompression == BI_BITFIELDS) offset += 12;
    bool ok = header.biSize >= sizeof(header) && offset <= size && size - offset >= 8;
    if (ok && target == 3) { data[offset] = 0xaf; data[offset + 1] = 0x01; data[offset + 2] = 0xfe; }
    if (ok && target == 5) {
      LONG resolution = 424242; std::memcpy(data + 24, &resolution, sizeof(resolution));
    }
    if (ok && target == 6) {
      ok = header.biSize >= sizeof(BITMAPV5HEADER);
      if (ok) { DWORD alpha = 0xff000000u; std::memcpy(data + 52, &alpha, sizeof(alpha)); }
    }
    if (ok && target == 7) data[offset + 4] ^= 0x7f;
    if (ok && target == 8) {
      ok = header.biSize >= sizeof(BITMAPV5HEADER);
      if (ok) { DWORD intent = LCS_GM_GRAPHICS; std::memcpy(data + 108, &intent, sizeof(intent)); }
    }
    GlobalUnlock(value.value);
    return ok;
  }
  return false;
}

int Writer(int argc, wchar_t** argv) {
  if (argc != 8) return 2;
  HRESULT initialized = OleInitialize(nullptr);
  std::printf("OLE_CHILD_INITIALIZE hr=%08lx\n", static_cast<unsigned long>(initialized));
  if (FAILED(initialized)) return 2;
  const std::wstring prefix = argv[2];
  HWND borrowed = reinterpret_cast<HWND>(static_cast<uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));
  const bool use_borrowed = _wtoi(argv[4]) != 0;
  const bool empty = _wtoi(argv[5]) != 0;
  const bool replay = _wtoi(argv[6]) != 0;
  const int target = _wtoi(argv[7]); // 0=replay, 1=text, 2=HTML, 3=pixel, 4=same text, 5..8=image metadata.
  HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, (prefix + L"-ready").c_str());
  HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, (prefix + L"-stop").c_str());
  HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (prefix + L"-map").c_str());
  auto* result = mapping ? static_cast<Observation*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Observation))) : nullptr;
  if (!ready || !stop || !result) {
    if (result) UnmapViewOfFile(result);
    if (mapping) CloseHandle(mapping);
    if (ready) CloseHandle(ready);
    if (stop) CloseHandle(stop);
    OleUninitialize();
    return 2;
  }
  HWND own = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"OLE identity external writer",
      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  result->pid = GetCurrentProcessId();
  result->owner_before = reinterpret_cast<uintptr_t>(GetClipboardOwner());
  bool ok = own != nullptr;
  std::vector<OwnedFormat> copies;
  const bool image_target = target == 3 || (target >= 5 && target <= 8);
  if (ok && (replay || image_target || target == 11)) ok = CopyFormats(&copies, result);
  if (ok && target == 1) ok = ReplaceBytes(&copies, CF_UNICODETEXT, WideBytes(OtherText()));
  if (ok && target == 2) ok = ReplaceBytes(&copies, HtmlFormat(), NarrowBytes(Html(OtherHtml())));
  if (ok && image_target) ok = MutatePreferredImage(&copies, target);
  if (ok && target == 4) ok = ReplaceBytes(&copies, CF_UNICODETEXT, WideBytes(OwnText()));
  if (ok && target == 9) {
    copies.erase(std::remove_if(copies.begin(), copies.end(),
        [](const OwnedFormat& value) { return value.format == CF_DIBV5; }), copies.end());
  }
  if (ok && target == 10) ok = ReplaceBytes(&copies, CF_DIBV5, std::vector<uint8_t>{0});
  if (ok && !replay && image_target) {
    UINT keep = 0;
    for (const auto& value : copies)
      if (value.format == CF_DIBV5 || (!keep && value.format == CF_DIB)) keep = value.format;
    copies.erase(std::remove_if(copies.begin(), copies.end(),
        [keep](const OwnedFormat& value) { return value.format != keep; }), copies.end());
  }
  if (ok && target != 11) {
    result->opened = Open(use_borrowed ? borrowed : own);
    ok = result->opened != 0;
    if (!ok) FirstError(result);
  }
  if (ok && target != 11) {
    if (empty) {
      result->emptied = EmptyClipboard() != 0;
      ok = result->emptied != 0;
      if (!ok) FirstError(result);
    }
    if (ok) {
      for (auto& value : copies) {
        if (!SetClipboardData(value.format, value.value)) { FirstError(result); ok = false; break; }
        value.value = nullptr; // Windows owns it after a successful SetClipboardData.
        ++result->formats_set;
      }
      result->published = ok && !copies.empty();
    }
    result->closed = CloseClipboard() != 0;
    if (!result->closed) { FirstError(result); ok = false; }
  }
  result->owner_after = reinterpret_cast<uintptr_t>(GetClipboardOwner());
  result->ready = 1;
  SetEvent(ready);
  const bool stopped = PumpWait(stop, 20000);
  if (own) DestroyWindow(own);
  UnmapViewOfFile(result);
  CloseHandle(mapping); CloseHandle(ready); CloseHandle(stop);
  copies.clear();
  OleUninitialize();
  return ok && stopped ? 0 : 2;
}


struct ForeignCase { const char* name; bool borrowed, empty, replay; int target; };
std::atomic<const ForeignCase*> active_foreign{nullptr};
std::atomic<const ForeignCase*> active_flush_foreign{nullptr};
std::atomic<IDataObject*> last_set_object{nullptr};
std::atomic<bool> fail_set{false}, hold_next_set{false};
std::atomic<unsigned> actual_set_calls{0};
HANDLE held_entered = nullptr, held_release = nullptr;
Snapshot before_foreign, after_foreign;

// An ordinary concurrent native consumer, like the service's Windows-events
// thread. It only opens/reads/closes the clipboard; it never replaces data.
struct NativeReader {
  HANDLE opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::thread thread;
  const char* phase;
  bool rendering = false;
  bool acquired = false, read = false, closed = false;
  DWORD open_thread = 0, close_thread = 0;
  DWORD open_error = 0, data_error = 0, lock_error = 0, close_error = 0;
  explicit NativeReader(const char* name) : phase(name) {
    Check(opened && release && done, "native reader owns its synchronization events");
    if (!opened || !release || !done) ExitProcess(3);
  }
  void Start(bool render) {
    rendering = render;
    thread = std::thread([this, render] {
      open_thread = GetCurrentThreadId();
      acquired = Open(nullptr, &open_error);
      SetEvent(opened);
      if (acquired) {
        if (render) {
          // OleSetClipboard has just accepted a fresh delayed-rendered object.
          // GetClipboardData needs its owning STA to dispatch WM_RENDERFORMAT.
          SetLastError(ERROR_SUCCESS);
          HANDLE value = GetClipboardData(CF_UNICODETEXT);
          data_error = GetLastError();
          SetLastError(ERROR_SUCCESS);
          const void* bytes = value ? GlobalLock(value) : nullptr;
          lock_error = GetLastError();
          const auto expected = WideBytes(OwnText());
          read = bytes && GlobalSize(value) >= expected.size()
              && std::memcmp(bytes, expected.data(), expected.size()) == 0;
          if (bytes) GlobalUnlock(value);
        } else {
          Check(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0,
                "held native reader is released after the actual competing open");
        }
        close_thread = GetCurrentThreadId();
        SetLastError(ERROR_SUCCESS);
        closed = CloseClipboard() != 0;
        close_error = GetLastError();
      }
      SetEvent(done);
    });
  }
  void Finish() {
    if (!thread.joinable()) { Check(false, "actual native reader started"); return; }
    const bool ended = PumpWait(done, 5000);
    Check(ended, "native reader and delayed rendering finish within their bound");
    if (!ended) ExitProcess(3);
    thread.join();
    // Report the worker's immediate API errors, not Finish's thread-local error.
    std::printf("CLIPBOARD_NATIVE_READER phase=%s render=%d acquired=%d read=%d closed=%d "
        "open_thread=%lu close_thread=%lu open_error=%lu data_error=%lu lock_error=%lu close_error=%lu\n",
        phase, rendering, acquired, read, closed,
        static_cast<unsigned long>(open_thread), static_cast<unsigned long>(close_thread),
        static_cast<unsigned long>(open_error), static_cast<unsigned long>(data_error),
        static_cast<unsigned long>(lock_error), static_cast<unsigned long>(close_error));
    Check(acquired && closed, "native reader acquired and released actual clipboard access");
  }
  ~NativeReader() {
    if (thread.joinable()) { SetEvent(release); Finish(); }
    CloseHandle(opened); CloseHandle(release); CloseHandle(done);
  }
};
std::atomic<NativeReader*> held_reader{nullptr}, completion_reader{nullptr};
std::atomic<unsigned> contended_set_calls{0};
std::atomic<std::atomic<bool>*> cancel_after_contended_open{nullptr};

void ForeignWrite(const ForeignCase& test, IDataObject* object) {
  before_foreign = Read();
  Check(OleIsCurrentClipboard(object) == S_OK, "accepted product object is current before the competing process");
  GUID nonce{};
  Check(CoCreateGuid(&nonce) == S_OK, "fresh owned child synchronization identity");
  wchar_t guid[40]{}; StringFromGUID2(nonce, guid, 40);
  const std::wstring prefix = std::wstring(L"Local\\Gesher-Clipboard-Publication-") + guid;
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, (prefix + L"-ready").c_str());
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, (prefix + L"-stop").c_str());
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
      sizeof(Observation), (prefix + L"-map").c_str());
  auto* result = mapping ? static_cast<Observation*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Observation))) : nullptr;
  Check(ready && stop && mapping && result, "child handles and result mapping are owned and valid");
  PROCESS_INFORMATION process{};
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  wchar_t executable[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
  bool started = false;
  if (ready && stop && result && length && length < 32768) {
    *result = Observation{};
    const auto owner = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(GetClipboardOwner()));
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --writer \"" + prefix + L"\" "
        + std::to_wstring(owner) + L" " + std::to_wstring(test.borrowed) + L" "
        + std::to_wstring(test.empty) + L" " + std::to_wstring(test.replay) + L" " + std::to_wstring(test.target);
    started = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0,
                              nullptr, nullptr, &startup, &process) != 0;
  }
  Check(started, "real separate writer process starts");
  if (started) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    Check(GetProcessTimes(process.hProcess, &creation, &exit, &kernel, &user)
          && (creation.dwHighDateTime || creation.dwLowDateTime), "actual foreign process creation time");
    const bool observed = PumpWait(ready, 15000);
    Check(observed, "owned STA pumps until actual foreign operation completes");
    if (observed) {
      Check(result->pid == process.dwProcessId && result->pid != GetCurrentProcessId(), "observed writer has the created external PID");
      const bool read_only = test.target == 11;
      const bool wrote = read_only
          ? result->read_opened && result->read_closed && result->formats_seen > 0
              && result->formats_seen == result->formats_copied && result->copy_failed == 0
              && !result->published && result->error == 0
          : result->opened && (!test.empty || result->emptied)
              && result->published && result->closed && result->error == 0;
      Check(wrote, "foreign control completed every required actual Windows call");
      if (test.replay) Check(result->formats_seen > 0 && result->formats_seen == result->formats_copied
          && result->copy_failed == 0, "every available format was really copied, including OLE metadata");
      std::printf("CLIPBOARD_FOREIGN name=%s pid=%lu borrowed=%d empty=%d replay=%d target=%d "
          "opened=%ld published=%ld closed=%ld copied=%ld seen=%ld error=%lu identity=%08lx\n",
          test.name, static_cast<unsigned long>(result->pid), test.borrowed, test.empty, test.replay, test.target,
          static_cast<long>(result->opened), static_cast<long>(result->published), static_cast<long>(result->closed),
          static_cast<long>(result->formats_copied), static_cast<long>(result->formats_seen),
          static_cast<unsigned long>(result->error), static_cast<unsigned long>(OleIsCurrentClipboard(object)));
      after_foreign = Read();
      if (test.target == 1) {
        std::u16string text;
        Check(gesher::windows::ReadClipboardText(&text)
              && text == std::u16string(reinterpret_cast<const char16_t*>(OtherText())), "different external text is actually readable");
      }
      if (test.target == 2) {
        std::u16string html;
        Check(gesher::windows::ReadClipboardHtml(&html) && html == u"<i>external different HTML</i>", "different external HTML is actually readable");
      }
      if (test.target == 3 || (test.target >= 5 && test.target <= 8))
        Check(after_foreign.image_format == CF_DIBV5 && !after_foreign.image.empty()
              && after_foreign.image != before_foreign.image, "actual preferred image bytes changed");
      if (test.target >= 5 && test.target <= 8) {
        const size_t offset = sizeof(BITMAPV5HEADER);
        Check(after_foreign.image.size() >= offset + 8 && before_foreign.image.size() >= offset + 8
              && std::memcmp(after_foreign.image.data() + offset, before_foreign.image.data() + offset, 4) == 0,
              "metadata and second-pixel control preserves the original first pixel");
      }
      if (test.target == 10) Check(after_foreign.image_format == CF_DIBV5
          && after_foreign.image.size() < sizeof(BITMAPINFOHEADER), "invalid preferred V5 is not replaced by a convenient fallback");
      if (test.target == 0 || test.target == 11) Check(Same(before_foreign, after_foreign), "full same-projection replay/read control preserves every preferred byte");
      if (read_only) Check(before_foreign.sequence == after_foreign.sequence,
                           "actual read-only external consumer leaves the sequence unchanged");
    }
    SetEvent(stop);
    const bool ended = PumpWait(process.hProcess, 5000);
    Check(ended, "exact owned foreign process retires");
    if (!ended) { TerminateProcess(process.hProcess, 3); WaitForSingleObject(process.hProcess, 5000); }
    DWORD code = STILL_ACTIVE; GetExitCodeProcess(process.hProcess, &code);
    Check(code == 0, "foreign process reports complete cleanup");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
  if (result) UnmapViewOfFile(result);
  if (mapping) CloseHandle(mapping);
  if (ready) CloseHandle(ready);
  if (stop) CloseHandle(stop);
}

bool PublishKind(int kind, uint32_t* sequence, bool empty_text = false) {
  const auto image = Dib();
  return kind == 2
      ? gesher::windows::WriteRichClipboard(empty_text ? u"" : Text(), Markup(), image.data(), image.size(), sequence)
      : gesher::windows::PublishClipboard(empty_text ? u"" : Text(), true, kind == 1 ? Markup() : u"", nullptr, 0, sequence);
}
void OwnCase(int kind, bool empty_text = false) {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "live service acquires a native publisher lease");
  uint32_t sequence = 0;
  Check(PublishKind(kind, &sequence, empty_text) && sequence != 0, "actual own publication has a verified nonzero receipt");
  auto own = Read();
  Check(own.sequence == sequence && !own.text.empty(), "read-only native consumer retains the exact completion generation");
  std::u16string text;
  Check(gesher::windows::ReadClipboardText(&text) && text == (empty_text ? u"" : Text()), "ordinary getter returns the exact text including empty text");
  if (kind >= 1) {
    std::u16string html;
    Check(gesher::windows::ReadClipboardHtml(&html) && html == Markup(), "ordinary getter returns the requested HTML fragment");
  }
  if (kind == 2) {
    std::vector<uint8_t> bmp;
    Check(gesher::windows::ReadClipboardImage(&bmp) && bmp.size() > 14
        && std::vector<uint8_t>(bmp.begin() + 14, bmp.end()) == own.image, "ordinary image reader returns the full preferred representation");
  }
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "last service release actually retires its native STA");
  Check(Same(own, Read()), "shutdown flush preserves complete native data after object retirement");
}
void Replacement(const ForeignCase& test) {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "race fixture acquires a real native lease");
  before_foreign = {}; after_foreign = {};
  active_foreign.store(&test, std::memory_order_release);
  uint32_t receipt = 0;
  const bool published = PublishKind(2, &receipt);
  active_foreign.store(nullptr, std::memory_order_release);
  const bool unchanged = test.target == 4 || test.target == 11;
  if (unchanged) Check(published && receipt != 0, "unchanged private object and complete data keep legitimate own completion");
  else Check(!published && receipt == 0, "different-content or full replay replacement cannot inherit Android publication authority");
  const Snapshot actual = Read();
  Check(Same(actual, after_foreign), "receipt validation did not overwrite the external clipboard");
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "foreign replacement path retires the last native lease");
  Check(Same(actual, Read()), "retirement does not re-publish stale Android data over the external result");
}
void FlushReplacement(const ForeignCase& test) {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "flush race fixture acquires a real native lease");
  uint32_t receipt = 0;
  Check(PublishKind(2, &receipt) && receipt != 0, "flush race begins with a real private publication");
  before_foreign = {}; after_foreign = {};
  active_flush_foreign.store(&test, std::memory_order_release);
  gesher::windows::ReleaseClipboardPublisher();
  active_flush_foreign.store(nullptr, std::memory_order_release);
  Check(!gesher::windows::State().current, "flush race retires its actual worker and request resources");
  Check(Same(after_foreign, Read()), "a foreign change after the final identity check survives actual shutdown flush");
}
void EmptyAndLeases() {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher() && gesher::windows::RetainClipboardPublisher(), "two live services have independent native leases");
  uint32_t sequence = 0;
  Check(PublishKind(2, &sequence), "first service publishes rich data");
  auto current = gesher::windows::State().current;
  gesher::windows::ReleaseClipboardPublisher();
  Check(current && !current->Stopping() && !current->Done(), "closing one service does not stop another service's STA");
  Check(PublishKind(0, &sequence, true) && sequence != 0, "second live service can publish an actual empty text clip");
  Check(IsClipboardFormatAvailable(CF_UNICODETEXT) && !Raw(CF_UNICODETEXT).empty(), "empty text remains a present native format");
  Check(gesher::windows::ClearClipboardPublication(&sequence) && sequence != 0, "true clear completes against the actual empty clipboard");
  Check(CountClipboardFormats() == 0 && !IsClipboardFormatAvailable(CF_UNICODETEXT)
      && !IsClipboardFormatAvailable(HtmlFormat()) && !gesher::windows::HasClipboardImage(), "clear removes every native format including text");
  gesher::windows::ReleaseClipboardPublisher();
  Check(current->Done() && !gesher::windows::State().current, "final lease closes and joins the actual worker");
  current.reset();
  Check(CountClipboardFormats() == 0, "retirement never restores content after a true clear");
  // Direct writes outside a service have a bounded operation lease as well.
  Check(PublishKind(1, &sequence) && !gesher::windows::State().current, "direct writer publishes and retires without a leaked service lease");
}
void Concurrent() {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "concurrent writers share a live publisher");
  std::array<std::thread, 6> threads;
  std::array<std::array<uint32_t, 8>, 6> receipts{};
  std::atomic<unsigned> ended{0};
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(done != nullptr, "concurrent completion event is owned");
  for (size_t worker = 0; worker < threads.size(); ++worker) {
    threads[worker] = std::thread([&, worker] {
      for (size_t call = 0; call < receipts[worker].size(); ++call) {
        const bool ok = PublishKind(static_cast<int>((worker + call) % 3), &receipts[worker][call]);
        Check(ok && receipts[worker][call] != 0, "concurrent plain/HTML/rich call completes without stolen ownership or CloseClipboard failure");
      }
      if (++ended == threads.size()) SetEvent(done);
    });
  }
  const bool complete = PumpWait(done, 60000);
  Check(complete, "all actual concurrent calls retire within their native bounds");
  if (!complete) ExitProcess(3); // External test watchdog also bounds this fixture process.
  for (auto& thread : threads) thread.join();
  CloseHandle(done);
  std::set<uint32_t> distinct;
  for (const auto& worker : receipts) for (uint32_t receipt : worker) distinct.insert(receipt);
  Check(distinct.size() == 48, "each accepted concurrent write has its own actual Windows generation");
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "all concurrent requests and their worker retire");
}
void NativeReadContention() {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "native reader fixture retains a publisher lease");
  uint32_t sequence = 0;
  Check(PublishKind(1, &sequence), "native reader starts from an actual publication");
  for (bool clear : {false, true}) {
    NativeReader reader(clear ? "clear-contention" : "write-contention");
    reader.Start(false);
    Check(PumpWait(reader.opened, 5000), "native consumer holds the actual clipboard before publication");
    const unsigned before = contended_set_calls.load();
    held_reader.store(&reader, std::memory_order_release);
    const bool published = clear ? gesher::windows::ClearClipboardPublication(&sequence)
        : PublishKind(1, &sequence);
    held_reader.store(nullptr, std::memory_order_release);
    SetEvent(reader.release);
    reader.Finish();
    Check(contended_set_calls.load() > before, "real OleSetClipboard reported the consumer's temporary open");
    Check(published && sequence != 0, "write and clear retry a temporary native reader without losing their completion proof");
    if (clear) Check(CountClipboardFormats() == 0, "contended clear still removes every actual format");
  }
  {
    NativeReader reader("delayed-render");
    completion_reader.store(&reader, std::memory_order_release);
    const bool published = PublishKind(1, &sequence);
    completion_reader.store(nullptr, std::memory_order_release);
    reader.Finish();
    Check(reader.read, "concurrent native consumer receives the exact delayed-rendered text");
    Check(published && sequence != 0,
          "completion proof services a native consumer waiting for this STA's delayed formats");
  }
  for (bool stop : {false, true}) {
    const auto before_clipboard = Read();
    std::atomic<bool> cancelled{false}, stopping{false};
    NativeReader reader(stop ? "stop-retry" : "cancel-retry");
    reader.Start(false);
    Check(PumpWait(reader.opened, 5000), "native reader holds clipboard before a cancelled retry");
    held_reader.store(&reader, std::memory_order_release);
    cancel_after_contended_open.store(stop ? &stopping : &cancelled, std::memory_order_release);
    const unsigned before_calls = actual_set_calls.load();
    // Exercise the production retry boundary with the same cancellation flags
    // its request/worker supplies, changing one only after the real open fails.
    const HRESULT result = gesher::windows::SetClipboardRetrying(nullptr, cancelled, stopping);
    cancel_after_contended_open.store(nullptr, std::memory_order_release);
    held_reader.store(nullptr, std::memory_order_release);
    SetEvent(reader.release);
    reader.Finish();
    Check(result == E_ABORT && actual_set_calls.load() == before_calls + 1,
          "cancellation and shutdown prevent a fresh publication attempt after a contended open");
    Check(Same(before_clipboard, Read()), "cancelled retry preserves the actual previous publication");
  }
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "native reader contention retires its publisher and requests");
}
void TimeoutRequests() {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "timeout fixture retains a live service lease");
  held_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  held_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(held_entered && held_release && done, "timeout control owns its synchronization events");
  if (!held_entered || !held_release || !done) ExitProcess(3);
  std::atomic<unsigned> ended{0};
  std::array<bool, 2> success{};
  std::array<uint32_t, 2> receipts{};
  const unsigned before = actual_set_calls.load();
  hold_next_set.store(true, std::memory_order_release);
  std::thread first([&] {
    success[0] = PublishKind(0, &receipts[0]);
    if (++ended == 2) SetEvent(done);
  });
  Check(PumpWait(held_entered, 5000), "actual worker entered a held OLE completion boundary");
  std::thread queued([&] {
    success[1] = PublishKind(1, &receipts[1]);
    if (++ended == 2) SetEvent(done);
  });
  const bool timed_out = PumpWait(done, 15000);
  Check(timed_out, "both calls honor the unchanged production ten-second completion bound");
  SetEvent(held_release);
  if (!timed_out) ExitProcess(3);
  first.join(); queued.join();
  Check(!success[0] && !success[1] && receipts[0] == 0 && receipts[1] == 0,
        "in-flight and queued timeouts never return fabricated publication receipts");
  Check(actual_set_calls.load() == before + 1, "cancelled queued request did not publish after its caller timed out");
  uint32_t receipt = 0;
  Check(PublishKind(2, &receipt) && receipt != 0, "live worker safely finishes its owned request and accepts the next real write");
  Check(actual_set_calls.load() == before + 2, "no timed-out queued write was replayed over the new publication");
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "timed-out request data and native worker retire without dangling caller resources");
  CloseHandle(done); CloseHandle(held_entered); CloseHandle(held_release);
  held_entered = nullptr; held_release = nullptr;
}
void ErrorAndResources() {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "error controls have a live publisher");
  uint32_t sequence = 0;
  Check(PublishKind(2, &sequence), "error control begins with a real owned publication");
  const auto prior = Read();
  fail_set.store(true, std::memory_order_release);
  Check(!PublishKind(0, &sequence) && sequence == 0, "explicit OLE failure does not invent native success");
  Check(Same(prior, Read()), "failed publication preserves the actual previous Windows data");
  const uint8_t bad[] = {1,2,3};
  Check(!gesher::windows::PublishClipboard(Text(), true, Markup(), bad, sizeof(bad), &sequence)
      && sequence == 0, "truncated image input fails honestly without partial publication");
  Check(Same(prior, Read()), "invalid image cannot silently replace rich data with text alone");
  gesher::windows::ReleaseClipboardPublisher();
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "record native handle baseline after warm-up");
  SetLastError(ERROR_SUCCESS);
  const DWORD user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  Check(user_before != 0 || GetLastError() == ERROR_SUCCESS,
        "record native USER-object baseline after warm-up");
  for (int pass = 0; pass != 16; ++pass) {
    Check(gesher::windows::RetainClipboardPublisher(), "re-entry takes a fresh service lease");
    Check(PublishKind(pass % 3, &sequence), "re-entered service publishes through a live STA");
    Check(gesher::windows::ClearClipboardPublication(&sequence), "re-entry clear preserves format retirement");
    gesher::windows::ReleaseClipboardPublisher();
    Check(!gesher::windows::State().current, "re-entry releases every private worker generation");
  }
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) != 0 && after <= before + 1,
        "repeated service lifetimes release native thread, request and event handles");
  SetLastError(ERROR_SUCCESS);
  const DWORD user_after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
  Check((user_after != 0 || GetLastError() == ERROR_SUCCESS) && user_after == user_before,
        "repeated service lifetimes release every private read window");
  std::printf("CLIPBOARD_RESOURCES before=%lu after=%lu user_before=%lu user_after=%lu\n",
      static_cast<unsigned long>(before), static_cast<unsigned long>(after),
      static_cast<unsigned long>(user_before), static_cast<unsigned long>(user_after));
}
struct ImageInput { const char* name; std::vector<uint8_t> bytes; };
std::vector<uint8_t> ImageFixture(DWORD header_size, WORD bits, DWORD compression,
                                bool top_down = false, bool profile = false) {
  const bool core = header_size == sizeof(BITMAPCOREHEADER);
  const DWORD colors = bits <= 8 ? (DWORD{1} << bits) : 0;
  const size_t masks = header_size == 40 && (compression == BI_BITFIELDS || compression == 6)
      ? (compression == 6 ? 16u : 12u) : 0;
  const size_t palette = static_cast<size_t>(colors) * (core ? 3u : 4u);
  const size_t row = ((size_t{2} * bits + 31u) / 32u) * 4u;
  const std::vector<uint8_t> rle8 = {2, 1, 0, 0, 2, 2, 0, 0, 0, 1};
  const std::vector<uint8_t> rle4 = {2, 0x12, 0, 0, 2, 0x23, 0, 0, 0, 1};
  const size_t pixels = compression == BI_RLE8 ? rle8.size()
      : (compression == BI_RLE4 ? rle4.size() : row * 2u);
  const size_t pixel_offset = static_cast<size_t>(header_size) + masks + palette;
  const size_t profile_size = profile ? 132u : 0u;
  std::vector<uint8_t> data(pixel_offset + pixels + profile_size, 0);
  BITMAPV5HEADER h{};
  h.bV5Size = header_size;
  h.bV5Width = 2; h.bV5Height = top_down ? -2 : 2;
  h.bV5Planes = 1; h.bV5BitCount = bits; h.bV5Compression = compression;
  h.bV5SizeImage = static_cast<DWORD>(pixels);
  h.bV5XPelsPerMeter = 3780; h.bV5YPelsPerMeter = 7559;
  h.bV5ClrUsed = colors; h.bV5ClrImportant = colors;
  if (compression == BI_BITFIELDS || compression == 6) {
    h.bV5RedMask = bits == 16 ? 0xf800u : 0x00ff0000u;
    h.bV5GreenMask = bits == 16 ? 0x07e0u : 0x0000ff00u;
    h.bV5BlueMask = bits == 16 ? 0x001fu : 0x000000ffu;
    if (header_size >= 56 || compression == 6) h.bV5AlphaMask = 0xff000000u;
  }
  h.bV5CSType = profile ? PROFILE_EMBEDDED : LCS_sRGB;
  h.bV5GammaRed = 0x00018000u;
  h.bV5GammaGreen = 0x00020000u;
  h.bV5GammaBlue = 0x00028000u;
  h.bV5Intent = LCS_GM_IMAGES;
  if (profile) {
    h.bV5ProfileData = static_cast<DWORD>(pixel_offset + pixels);
    h.bV5ProfileSize = static_cast<DWORD>(profile_size);
  }
  if (core) {
    BITMAPCOREHEADER c{};
    c.bcSize = sizeof(c); c.bcWidth = 2; c.bcHeight = 2;
    c.bcPlanes = 1; c.bcBitCount = bits;
    std::memcpy(data.data(), &c, sizeof(c));
  } else {
    std::memcpy(data.data(), &h, header_size);
  }
  if (masks) std::memcpy(data.data() + header_size, &h.bV5RedMask, masks);
  for (DWORD index = 0; index < colors; ++index) {
    const size_t offset = static_cast<size_t>(header_size) + masks
        + static_cast<size_t>(index) * (core ? 3u : 4u);
    data[offset] = static_cast<uint8_t>(index * 31u);
    data[offset + 1] = static_cast<uint8_t>(index * 53u);
    data[offset + 2] = static_cast<uint8_t>(index * 79u);
  }
  if (compression == BI_RLE8) {
    std::copy(rle8.begin(), rle8.end(), data.begin() + static_cast<ptrdiff_t>(pixel_offset));
  } else if (compression == BI_RLE4) {
    std::copy(rle4.begin(), rle4.end(), data.begin() + static_cast<ptrdiff_t>(pixel_offset));
  } else {
    for (size_t index = 0; index < pixels; ++index)
      data[pixel_offset + index] = static_cast<uint8_t>((index * 37u + 17u) & 0xffu);
  }
  if (profile) {
    // A complete, bounded embedded-profile payload used for byte/layout
    // preservation. This probe does not assert a color-engine interpretation.
    uint8_t* p = data.data() + h.bV5ProfileData;
    p[3] = 132; p[8] = 4; p[9] = 0x30;
    std::memcpy(p + 12, "mntrRGB XYZ ", 12);
    std::memcpy(p + 36, "acsp", 4);
    std::memcpy(p + 40, "MSFT", 4);
    p[68] = 0; p[69] = 0; p[70] = 0xf6; p[71] = 0xd6;
    p[72] = 0; p[73] = 1; p[74] = 0; p[75] = 0;
    p[76] = 0; p[77] = 0; p[78] = 0xd3; p[79] = 0x2d;
  }
  return data;
}
std::vector<uint8_t> FixtureBmp(const std::vector<uint8_t>& dib) {
  if (dib.size() < 12) return {};
  DWORD size = 0;
  std::memcpy(&size, dib.data(), sizeof(size));
  size_t pixel_offset = 0;
  if (size == 12) {
    BITMAPCOREHEADER h{};
    std::memcpy(&h, dib.data(), sizeof(h));
    pixel_offset = 12u + (h.bcBitCount <= 8 ? (size_t{1} << h.bcBitCount) * 3u : 0u);
  } else {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) return {};
    BITMAPINFOHEADER h{};
    std::memcpy(&h, dib.data(), sizeof(h));
    const size_t colors = h.biClrUsed ? h.biClrUsed
        : (h.biBitCount <= 8 ? size_t{1} << h.biBitCount : 0u);
    const size_t masks = size == 40 && h.biCompression == BI_BITFIELDS ? 12u
        : (size == 40 && h.biCompression == 6 ? 16u : 0u);
    pixel_offset = static_cast<size_t>(size) + masks + colors * 4u;
  }
  if (pixel_offset >= dib.size() || dib.size() > 64u * 1024u * 1024u) return {};
  static_assert(sizeof(BITMAPFILEHEADER) == 14);
  BITMAPFILEHEADER file{};
  file.bfType = 0x4d42;
  file.bfSize = static_cast<DWORD>(14u + dib.size());
  file.bfOffBits = static_cast<DWORD>(14u + pixel_offset);
  std::vector<uint8_t> result(14u + dib.size());
  std::memcpy(result.data(), &file, sizeof(file));
  std::memcpy(result.data() + 14u, dib.data(), dib.size());
  return result;
}

bool RecordedOldImageAvailable(const char* name) {
  // Exact old107 observations on both Windows 2022/2025 in 44727512461 a1.
  // This is a named regression matrix, not a general image acceptance policy.
  const char* const available[] = {"info-1", "info-4", "info-8", "info-16-rgb",
      "info-16-masks", "info-24", "info-24-top-down", "info-32-rgb",
      "info-32-masks", "info-rle8", "info-rle4"};
  for (const char* value : available) if (std::strcmp(name, value) == 0) return true;
  return false;
}


void ImageCase(const ImageInput& input) {
  ++case_number;
  Check(gesher::windows::RetainClipboardPublisher(), "image fixture leases the production publisher");
  uint32_t sequence = 0;
  Check(gesher::windows::WriteRichClipboard(Text(), Markup(), input.bytes.data(), input.bytes.size(), &sequence)
      && sequence != 0, "supported packed input has an exact production publication receipt");
  const auto original = Raw(CF_DIB);
  Check(original.size() >= input.bytes.size()
      && std::equal(input.bytes.begin(), input.bytes.end(), original.begin())
      && std::all_of(original.begin() + static_cast<ptrdiff_t>(input.bytes.size()), original.end(), [](uint8_t v){ return v == 0; }),
      "original DIB masks and bytes remain available with only zero allocation padding");
  std::vector<uint8_t> actual;
  Check(gesher::windows::ReadClipboardImage(&actual), "actual production image reader accepts the preferred layout");
  const auto source = gesher::clipboard_wic_test::Decode(FixtureBmp(input.bytes));
  const auto native = gesher::clipboard_wic_test::Decode(actual);
  gesher::clipboard_wic_test::Print(input.name, "source", source);
  gesher::clipboard_wic_test::Print(input.name, "product", native);
  const bool pixels = source.pixels_equal(native);
  const bool resolution = source.resolution_result == S_OK && native.resolution_result == S_OK
      && source.dpi_x == native.dpi_x && source.dpi_y == native.dpi_y;
  const bool contexts = source.color_result == native.color_result && source.colors == native.colors;
  if (RecordedOldImageAvailable(input.name)) {
    Check(pixels, "every previously supported input preserves all decoded pixels and alpha");
    Check(resolution, "every previously supported input preserves both actual DPI values");
    Check(contexts, "every previously supported input preserves actual color-context results and bytes");
  }
  std::printf("CLIPBOARD_IMAGE_SEMANTICS name=%s previously_supported=%d pixels=%d dpi=%d contexts=%d\n",
      input.name, RecordedOldImageAvailable(input.name), pixels, resolution, contexts);
  const auto preferred = Raw(CF_DIBV5);
  Check(preferred.size() >= sizeof(BITMAPV5HEADER), "preferred image contains its complete actual header");
  if (preferred.size() >= sizeof(BITMAPV5HEADER)) {
    BITMAPV5HEADER h{}; std::memcpy(&h, preferred.data(), sizeof(h));
    DWORD source_size = 0; std::memcpy(&source_size, input.bytes.data(), sizeof(source_size));
    if (source_size < 124) Check(h.bV5Intent == LCS_GM_IMAGES, "pre-V5 input keeps the observed valid Windows rendering intent");
    else Check(std::memcmp(input.bytes.data(), preferred.data(), sizeof(h)) == 0, "all explicit V5 metadata including alpha, profile offsets and intent are retained");
  }
  gesher::windows::ReleaseClipboardPublisher();
  Check(!gesher::windows::State().current, "image worker and immutable buffers retire with their service lease");
}
} // namespace clipboard_test

HRESULT PublicationTestSetClipboard(IDataObject* object) {
  if (clipboard_test::fail_set.exchange(false, std::memory_order_acq_rel)) return E_ACCESSDENIED;
  ++clipboard_test::actual_set_calls;
  const HRESULT result = ::OleSetClipboard(object);
  if (result == CLIPBRD_E_CANT_OPEN) {
    auto* cancelled = clipboard_test::cancel_after_contended_open.load(std::memory_order_acquire);
    if (cancelled) cancelled->store(true, std::memory_order_release);
    auto* reader = clipboard_test::held_reader.load(std::memory_order_acquire);
    if (reader) {
      ++clipboard_test::contended_set_calls;
      SetEvent(reader->release);
    }
  }
  if (result == S_OK) clipboard_test::last_set_object.store(object, std::memory_order_release);
  if (result == S_OK && object) {
    auto* reader = clipboard_test::completion_reader.exchange(nullptr, std::memory_order_acq_rel);
    if (reader) {
      reader->Start(true);
      // Deliberately do not pump yet: the production completion proof must
      // dispatch the consumer's delayed-render request while retrying its open.
      clipboard_test::Check(WaitForSingleObject(reader->opened, 5000) == WAIT_OBJECT_0,
                           "native consumer opens the fresh publication before its receipt");
    }
  }
  if (result == S_OK && object && clipboard_test::hold_next_set.exchange(false, std::memory_order_acq_rel)) {
    SetEvent(clipboard_test::held_entered);
    clipboard_test::Check(clipboard_test::PumpWait(clipboard_test::held_release, 20000),
                         "explicit held-completion boundary is released by its owner");
  }
  const auto* test = clipboard_test::active_foreign.load(std::memory_order_acquire);
  if (result == S_OK && object && test) clipboard_test::ForeignWrite(*test, object);
  return result;
}
HRESULT PublicationTestFlushClipboard() {
  const auto* test = clipboard_test::active_flush_foreign.load(std::memory_order_acquire);
  if (test) {
    auto* object = clipboard_test::last_set_object.load(std::memory_order_acquire);
    clipboard_test::Check(object != nullptr, "shutdown race starts from the actual accepted private object");
    if (object) clipboard_test::ForeignWrite(*test, object);
  }
  return ::OleFlushClipboard();
}
int wmain(int argc, wchar_t** argv) {
  using namespace clipboard_test;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc > 1 && std::wstring(argv[1]) == L"--writer") return Writer(argc, argv);
  const HRESULT init = OleInitialize(nullptr);
  Check(SUCCEEDED(init), "real caller OLE apartment initializes");
  if (FAILED(init)) return 1;
  OwnCase(0); OwnCase(1); OwnCase(2); OwnCase(0, true);
  const ForeignCase cases[] = {
      {"normal-different-text", false, true, false, 1},
      {"borrowed-replay-different-text", true, true, true, 1},
      {"normal-replay-same-projection", false, true, true, 0},
      {"borrowed-replay-same-projection", true, true, true, 0},
      {"normal-inplace-text", false, false, false, 1},
      {"borrowed-inplace-text", true, false, false, 1},
      {"borrowed-replay-different-html", true, true, true, 2},
      {"borrowed-replay-different-image", true, true, true, 3},
      {"normal-inplace-html", false, false, false, 2},
      {"borrowed-inplace-image", true, false, false, 3},
      {"borrowed-inplace-same-text-observation", true, false, false, 4},
      {"borrowed-inplace-image-resolution", true, false, false, 5},
      {"normal-inplace-image-alpha-mask", false, false, false, 6},
      {"borrowed-inplace-image-second-pixel", true, false, false, 7},
      {"normal-inplace-image-color-intent", false, false, false, 8},
      {"borrowed-replay-without-preferred-v5", true, true, true, 9},
      {"normal-inplace-invalid-preferred-v5", false, false, false, 10},
      {"normal-read-only-all-native-formats", false, false, true, 11},
  };
  const ImageInput images[] = {
      {"core-1", ImageFixture(12, 1, BI_RGB)},
      {"core-4", ImageFixture(12, 4, BI_RGB)},
      {"core-8", ImageFixture(12, 8, BI_RGB)},
      {"core-24", ImageFixture(12, 24, BI_RGB)},
      {"info-1", ImageFixture(40, 1, BI_RGB)},
      {"info-4", ImageFixture(40, 4, BI_RGB)},
      {"info-8", ImageFixture(40, 8, BI_RGB)},
      {"info-16-rgb", ImageFixture(40, 16, BI_RGB)},
      {"info-16-masks", ImageFixture(40, 16, BI_BITFIELDS)},
      {"info-24", ImageFixture(40, 24, BI_RGB)},
      {"info-24-top-down", ImageFixture(40, 24, BI_RGB, true)},
      {"info-32-rgb", ImageFixture(40, 32, BI_RGB)},
      {"info-32-masks", ImageFixture(40, 32, BI_BITFIELDS)},
      {"v2-32-masks", ImageFixture(52, 32, BI_BITFIELDS)},
      {"v3-32-alpha", ImageFixture(56, 32, BI_BITFIELDS)},
      {"v4-24-srgb-gamma", ImageFixture(108, 24, BI_RGB)},
      {"v4-32-alpha", ImageFixture(108, 32, BI_BITFIELDS)},
      {"v5-32-alpha", ImageFixture(124, 32, BI_BITFIELDS)},
      {"v5-24-top-down", ImageFixture(124, 24, BI_RGB, true)},
      {"v5-32-embedded-profile-bytes", ImageFixture(124, 32, BI_BITFIELDS, false, true)},
      {"info-rle8", ImageFixture(40, 8, BI_RLE8)},
      {"info-rle4", ImageFixture(40, 4, BI_RLE4)},
  };

  for (const auto& value : cases) Replacement(value);
  for (const auto& value : std::array<ForeignCase, 4>{{
      {"flush-normal-replacement", false, true, false, 1},
      {"flush-borrowed-inplace-text", true, false, false, 1},
      {"flush-normal-inplace-html", false, false, false, 2},
      {"flush-borrowed-inplace-image", true, false, false, 3}}}) FlushReplacement(value);
  for (const auto& input : images) ImageCase(input);
  EmptyAndLeases(); Concurrent(); NativeReadContention(); TimeoutRequests(); ErrorAndResources();
  ClearFixture();
  OleUninitialize();
  std::printf("Clipboard publication Windows completed: cases=%u checks=%d failures=%d\n", case_number, checks.load(), failures.load());
  return failures.load() ? 1 : 0;
}
