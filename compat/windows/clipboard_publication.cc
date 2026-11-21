/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#include "clipboard_publication.h"
#include "clipboard_dib_layout.h"
#include <ole2.h>
#include <objidl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace gesher::windows {
namespace {
constexpr DWORD kOperationTimeoutMs = 10000;

UINT HtmlFormat() {
  static const UINT format = RegisterClipboardFormatW(L"HTML Format");
  return format;
}

struct FormatBytes {
  CLIPFORMAT format;
  std::vector<uint8_t> bytes;
};

// Freeze the complete allocation extent before publishing. OLE consumers own
// copies of these bytes, never an alias of the application's immutable input.
bool AddFormat(std::vector<FormatBytes>* formats, CLIPFORMAT format,
               std::vector<uint8_t> bytes) {
  if (!format || bytes.empty()) return false;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes.size());
  if (!memory) return false;
  const SIZE_T extent = GlobalSize(memory);
  GlobalFree(memory);
  if (extent < bytes.size()) return false;
  bytes.resize(extent, 0);
  formats->push_back({format, std::move(bytes)});
  return true;
}

std::vector<uint8_t> Bytes(const void* value, size_t size) {
  const auto* first = static_cast<const uint8_t*>(value);
  return std::vector<uint8_t>(first, first + size);
}

bool HtmlBytes(const std::u16string& html, std::vector<uint8_t>* bytes) {
  if (html.size() > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
  const auto* chars = reinterpret_cast<const wchar_t*>(html.data());
  const int length = WideCharToMultiByte(CP_UTF8, 0, chars, static_cast<int>(html.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (length <= 0) return false;
  std::string fragment(static_cast<size_t>(length), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, chars, static_cast<int>(html.size()),
                          fragment.data(), length, nullptr, nullptr) != length) return false;
  const char* format = "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\n"
                       "StartFragment:%010zu\r\nEndFragment:%010zu\r\n";
  const std::string before = "<html><body>\r\n<!--StartFragment-->";
  const std::string after = "<!--EndFragment-->\r\n</body>\r\n</html>";
  char header[256];
  const int count = std::snprintf(header, sizeof(header), format,
                                size_t{0}, size_t{0}, size_t{0}, size_t{0});
  if (count <= 0 || static_cast<size_t>(count) >= sizeof(header)) return false;
  const size_t start = static_cast<size_t>(count) + before.size();
  const size_t end = start + fragment.size();
  if (std::snprintf(header, sizeof(header), format, static_cast<size_t>(count),
                    end + after.size(), start, end) != count) return false;
  const std::string wrapped = std::string(header) + before + fragment + after;
  *bytes = Bytes(wrapped.c_str(), wrapped.size() + 1);
  return true;
}

bool MakeFormats(const std::u16string& text, bool has_text, const std::u16string& html,
                 const uint8_t* image, size_t image_size, std::vector<FormatBytes>* out) {
  if (has_text) {
    if (text.size() >= (std::numeric_limits<size_t>::max)() / sizeof(char16_t)) return false;
    if (!AddFormat(out, CF_UNICODETEXT, Bytes(text.c_str(), (text.size() + 1) * sizeof(char16_t))))
      return false;
  }
  if (!html.empty()) {
    std::vector<uint8_t> bytes;
    if (!HtmlBytes(html, &bytes) || !AddFormat(out, static_cast<CLIPFORMAT>(HtmlFormat()),
                                             std::move(bytes))) return false;
  }
  if (image_size) {
    if (!image) return false;
    // The existing native boundary accepts a packed DIB or a BMP file whose
    // 14-byte file header precedes the DIB. Keep the original DIB and masks.
    const size_t offset = image_size > 14 && image[0] == 'B' && image[1] == 'M' ? 14 : 0;
    auto original = Bytes(image + offset, image_size - offset);
    std::vector<uint8_t> preferred;
    if (!clipboard_layout::ToPackedV5(original, &preferred)) return false;
    if (!AddFormat(out, CF_DIB, std::move(original))
        || !AddFormat(out, CF_DIBV5, std::move(preferred))) return false;
  }
  return true;
}

class FormatEnumerator final : public IEnumFORMATETC {
 public:
  explicit FormatEnumerator(std::vector<FORMATETC> formats, size_t position = 0)
      : formats_(std::move(formats)), position_(position) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IEnumFORMATETC) return E_NOINTERFACE;
    *out = static_cast<IEnumFORMATETC*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = InterlockedDecrement(&references_);
    if (!left) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* values, ULONG* fetched) override {
    if (fetched) *fetched = 0;
    if (!values || (!fetched && count != 1)) return E_POINTER;
    ULONG n = 0;
    while (n < count && position_ < formats_.size()) values[n++] = formats_[position_++];
    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    const size_t n = (std::min)(static_cast<size_t>(count), formats_.size() - position_);
    position_ += n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override { position_ = 0; return S_OK; }
  HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    try { *out = new (std::nothrow) FormatEnumerator(formats_, position_); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    return *out ? S_OK : E_OUTOFMEMORY;
  }
 private:
  LONG references_ = 1;
  std::vector<FORMATETC> formats_;
  size_t position_;
};

class Publication final : public IDataObject {
 public:
  explicit Publication(std::vector<FormatBytes> formats) : formats_(std::move(formats)) {}
  const std::vector<FormatBytes>& formats() const { return formats_; }
  const FormatBytes* Find(CLIPFORMAT format) const {
    for (const auto& value : formats_) if (value.format == format) return &value;
    return nullptr;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDataObject) return E_NOINTERFACE;
    *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&references_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = InterlockedDecrement(&references_);
    if (!left) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* request) override {
    if (!request) return E_POINTER;
    if (request->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (request->lindex != -1) return DV_E_LINDEX;
    if (request->ptd) return DV_E_DVTARGETDEVICE;
    if (!(request->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    return Find(request->cfFormat) ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* request, STGMEDIUM* medium) override {
    if (!medium) return E_POINTER;
    std::memset(medium, 0, sizeof(*medium));
    const HRESULT result = QueryGetData(request);
    if (result != S_OK) return result;
    const auto& bytes = Find(request->cfFormat)->bytes;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes.size());
    if (!memory) return STG_E_MEDIUMFULL;
    if (GlobalSize(memory) != bytes.size()) { GlobalFree(memory); return STG_E_MEDIUMFULL; }
    void* target = GlobalLock(memory);
    if (!target) { GlobalFree(memory); return STG_E_MEDIUMFULL; }
    std::memcpy(target, bytes.data(), bytes.size());
    GlobalUnlock(memory);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
    if (!in || !out) return E_POINTER;
    *out = *in; out->ptd = nullptr; return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    try {
      std::vector<FORMATETC> values;
      for (const auto& format : formats_)
        values.push_back({format.format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
      *out = new (std::nothrow) FormatEnumerator(std::move(values));
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD* connection) override {
    if (connection) *connection = 0;
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override {
    if (out) *out = nullptr;
    return OLE_E_ADVISENOTSUPPORTED;
  }
 private:
  LONG references_ = 1;
  const std::vector<FormatBytes> formats_;
};

bool WaitPumping(HANDLE event, DWORD timeout);

bool OpenForRead(HWND read_window) {
  if (!read_window) return false;
  for (int attempt = 0; attempt != 20; ++attempt) {
    if (OpenClipboard(read_window)) return true;
    // A reader can own the clipboard while requesting this STA's delayed
    // formats. Let it finish and close before retrying the completion proof.
    if (attempt != 19 && !WaitPumping(nullptr, 10)) return false;
  }
  return false;
}

bool SameNativeBytes(const FormatBytes& expected) {
  HANDLE value = GetClipboardData(expected.format);
  if (!value || GlobalSize(value) != expected.bytes.size()) return false;
  const void* bytes = GlobalLock(value);
  if (!bytes) return false;
  const bool same = std::memcmp(bytes, expected.bytes.data(), expected.bytes.size()) == 0;
  GlobalUnlock(value);
  return same;
}

// This is a one-call completion proof, not an equality-based adoption path.
// Full replacement invalidates the private OLE identity, even for copied public
// data. In-place changes are caught by complete buffers and format precedence.
uint32_t CompletionSequence(Publication* object, HWND read_window) {
  if (!object || OleIsCurrentClipboard(object) != S_OK) return 0;
  const DWORD before = GetClipboardSequenceNumber();
  if (!before || !OpenForRead(read_window)) return 0;
  const bool wants_text = object->Find(CF_UNICODETEXT) != nullptr;
  const bool has_unicode = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
  const bool has_text = has_unicode || IsClipboardFormatAvailable(CF_TEXT) != 0;
  const bool wants_html = object->Find(static_cast<CLIPFORMAT>(HtmlFormat())) != nullptr;
  const bool wants_image = object->Find(CF_DIBV5) != nullptr;
  const UINT image_format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
      : (IsClipboardFormatAvailable(CF_DIB) ? CF_DIB : 0);
  bool same = (wants_text ? has_unicode : !has_text)
      && (IsClipboardFormatAvailable(HtmlFormat()) != 0) == wants_html
      && (wants_image ? image_format == CF_DIBV5 : image_format == 0);
  for (const auto& format : object->formats()) {
    if (!SameNativeBytes(format)) same = false;
  }
  const DWORD locked = GetClipboardSequenceNumber();
  const bool closed = CloseClipboard() != 0;
  const DWORD after = GetClipboardSequenceNumber();
  return same && closed && before == locked && locked == after
      && OleIsCurrentClipboard(object) == S_OK ? after : 0;
}

uint32_t EmptyCompletionSequence(HWND read_window) {
  const DWORD before = GetClipboardSequenceNumber();
  if (!before || !OpenForRead(read_window)) return 0;
  SetLastError(ERROR_SUCCESS);
  const bool empty = CountClipboardFormats() == 0 && GetLastError() == ERROR_SUCCESS;
  const DWORD locked = GetClipboardSequenceNumber();
  const bool closed = CloseClipboard() != 0;
  const DWORD after = GetClipboardSequenceNumber();
  return empty && closed && before == locked && locked == after ? after : 0;
}

// A caller may own a native window. Pump while waiting so OLE's real clipboard
// owner messages cannot deadlock against that caller. No private pointer is ever
// accepted through a public window message; requests use an owned queue/event.
bool WaitPumping(HANDLE event, DWORD timeout) {
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0) return true;
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= timeout) return event == nullptr;
    // A null event is a bounded delay that still services this apartment.
    const DWORD count = event ? 1 : 0;
    const DWORD ready = MsgWaitForMultipleObjectsEx(count, event ? &event : nullptr,
        timeout - static_cast<DWORD>(elapsed), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (event && ready == WAIT_OBJECT_0) return true;
    if (ready == WAIT_TIMEOUT) return event == nullptr;
    if (ready != WAIT_OBJECT_0 + count) return false;
    MSG message;
    for (unsigned n = 0; n != 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
      if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
      TranslateMessage(&message); DispatchMessageW(&message);
    }
  }
}

HRESULT SetClipboardRetrying(IDataObject* object, const std::atomic<bool>& cancelled,
                             const std::atomic<bool>& stopping) {
  HRESULT result = CLIPBRD_E_CANT_OPEN;
  for (int attempt = 0; attempt != 20; ++attempt) {
    if (cancelled.load(std::memory_order_acquire)
        || stopping.load(std::memory_order_acquire)) return E_ABORT;
    result = OleSetClipboard(object);
    // Only an unsuccessful open is safe to retry: no publication happened.
    // Never replay a completed write or a partial/finalization error.
    if (result != CLIPBRD_E_CANT_OPEN || attempt == 19
        || !WaitPumping(nullptr, 10)) break;
  }
  return result;
}

class Publisher final {
 public:
  static std::shared_ptr<Publisher> Create() {
    auto value = std::shared_ptr<Publisher>(new (std::nothrow) Publisher);
    if (!value || !value->work_ || !value->stop_ || !value->ready_) return nullptr;
    auto* start = new (std::nothrow) std::shared_ptr<Publisher>(value);
    if (!start) return nullptr;
    value->thread_ = CreateThread(nullptr, 0, Run, start, 0, nullptr);
    if (!value->thread_) { delete start; return nullptr; }
    return value;
  }
  ~Publisher() {
    if (thread_) CloseHandle(thread_);
    if (ready_) CloseHandle(ready_);
    if (stop_) CloseHandle(stop_);
    if (work_) CloseHandle(work_);
  }
  bool Ready() {
    return WaitPumping(ready_, kOperationTimeoutMs)
        && SUCCEEDED(initialized_.load(std::memory_order_acquire));
  }
  bool Done() const { return thread_ && WaitForSingleObject(thread_, 0) == WAIT_OBJECT_0; }
  bool Stopping() const { return stopping_.load(std::memory_order_acquire); }
  void Stop() {
    stopping_.store(true, std::memory_order_release);
    SetEvent(stop_);
  }
  bool WaitStopped() { return WaitPumping(thread_, kOperationTimeoutMs); }
  uint32_t Submit(std::vector<FormatBytes> formats) {
    auto request = std::make_shared<Request>(std::move(formats));
    if (!request->done) return 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Stopping() || Done()) return 0;
      requests_.push_back(request);
      if (!SetEvent(work_)) { requests_.pop_back(); return 0; }
    }
    if (!WaitPumping(request->done, kOperationTimeoutMs)) {
      request->cancelled.store(true, std::memory_order_release);
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = std::find(requests_.begin(), requests_.end(), request);
      if (found != requests_.end()) requests_.erase(found);
      if (requests_.empty()) ResetEvent(work_);
      return 0;
    }
    return request->complete.load(std::memory_order_acquire) ? request->sequence : 0;
  }
 private:
  Publisher()
      : work_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        ready_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  struct Request {
    explicit Request(std::vector<FormatBytes> value)
        : formats(std::move(value)), done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Request() { if (done) CloseHandle(done); }
    std::vector<FormatBytes> formats;
    HANDLE done;
    uint32_t sequence = 0;
    std::atomic<bool> cancelled{false}, complete{false};
  };
  static void Complete(const std::shared_ptr<Request>& request) {
    request->complete.store(true, std::memory_order_release);
    SetEvent(request->done);
  }
  static DWORD WINAPI Run(void* parameter) {
    // The worker owns this shared state until it actually exits. A timed-out
    // caller cannot close its events, free a request or destroy its COM object.
    std::unique_ptr<std::shared_ptr<Publisher>> start(
        static_cast<std::shared_ptr<Publisher>*>(parameter));
    const auto self = *start;
    start.reset();
    HRESULT initialized = OleInitialize(nullptr);
    HWND read_window = nullptr;
    if (SUCCEEDED(initialized)) {
      // A distinct open association prevents our completion read from sharing
      // a NULL-window consumer's open state in this process. This window has no
      // request protocol or authority; OLE identity still proves publication.
      read_window = CreateWindowExW(0, L"STATIC", nullptr, 0, 0, 0, 0, 0,
          HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
      if (!read_window) { OleUninitialize(); initialized = E_FAIL; }
    }
    self->initialized_.store(initialized, std::memory_order_release);
    SetEvent(self->ready_);
    if (FAILED(initialized)) return 1;
    Publication* current = nullptr;
    HANDLE events[] = {self->stop_, self->work_};
    bool running = true;
    while (running) {
      const DWORD ready = MsgWaitForMultipleObjects(2, events, FALSE, INFINITE, QS_ALLINPUT);
      if (ready == WAIT_OBJECT_0) break;
      if (ready == WAIT_OBJECT_0 + 1) {
        std::shared_ptr<Request> request;
        {
          std::lock_guard<std::mutex> lock(self->mutex_);
          if (!self->requests_.empty()) {
            request = self->requests_.front(); self->requests_.pop_front();
          }
          if (self->requests_.empty()) ResetEvent(self->work_);
        }
        if (request) {
          if (!self->Stopping() && !request->cancelled.load(std::memory_order_acquire)) {
            if (request->formats.empty()) {
              if (SetClipboardRetrying(nullptr, request->cancelled, self->stopping_) == S_OK) {
                if (current) { current->Release(); current = nullptr; }
                request->sequence = EmptyCompletionSequence(read_window);
              }
            } else {
              auto* next = new (std::nothrow) Publication(std::move(request->formats));
              if (next && SetClipboardRetrying(next, request->cancelled, self->stopping_) == S_OK) {
                if (current) current->Release();
                current = next;
                request->sequence = CompletionSequence(current, read_window);
              } else if (next) next->Release();
            }
          }
          Complete(request);
        }
      } else if (ready != WAIT_OBJECT_0 + 2) running = false;
      // A busy request queue must still service OLE's apartment messages.
      // Otherwise a native reader can hold the clipboard while waiting for
      // GetData on this STA, and every queued writer would time out behind it.
      if (running) {
        MSG message;
        for (unsigned n = 0; n != 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n) {
          if (message.message == WM_QUIT) { running = false; break; }
          TranslateMessage(&message); DispatchMessageW(&message);
        }
      }
    }
    // Never re-publish a stale object over an external replacement or in-place
    // mutation. Flush only the exact still-current, unchanged native buffers.
    if (current) {
      if (CompletionSequence(current, read_window)) OleFlushClipboard();
      current->Release();
    }
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      for (const auto& request : self->requests_) Complete(request);
      self->requests_.clear();
    }
    DestroyWindow(read_window);
    OleUninitialize();
    return 0;
  }
  std::mutex mutex_;
  std::deque<std::shared_ptr<Request>> requests_;
  std::atomic<bool> stopping_{false};
  HANDLE work_ = nullptr, stop_ = nullptr, ready_ = nullptr, thread_ = nullptr;
  std::atomic<HRESULT> initialized_{E_PENDING};
};

struct Publishers {
  std::mutex mutex;
  std::shared_ptr<Publisher> current;
  size_t clients = 0, operations = 0;
};
Publishers& State() {
  static Publishers state;
  return state;
}

void RetireIdle() {
  auto& state = State();
  std::shared_ptr<Publisher> retiring;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.clients && !state.operations && state.current) {
      retiring = state.current; retiring->Stop();
    }
  }
  if (retiring) {
    retiring->WaitStopped();
    std::lock_guard<std::mutex> lock(state.mutex);
    // A stuck retiring worker remains the sole generation. New users receive
    // an honest failure until it exits, rather than accumulating leaked STAs.
    if (state.current == retiring && retiring->Done()) state.current.reset();
  }
}

class Operation final {
 public:
  Operation() {
    auto& state = State();
    for (int attempt = 0; attempt != 2; ++attempt) {
      std::shared_ptr<Publisher> retiring;
      {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.current && state.current->Done()) state.current.reset();
        if (state.current && state.current->Stopping()) retiring = state.current;
        else {
          if (!state.current) state.current = Publisher::Create();
          if (!state.current) return;
          publisher = state.current;
          ++state.operations;
          return;
        }
      }
      if (!retiring->WaitStopped()) return;
    }
  }
  ~Operation() {
    if (!publisher) return;
    auto& state = State();
    { std::lock_guard<std::mutex> lock(state.mutex); --state.operations; }
    RetireIdle();
  }
  std::shared_ptr<Publisher> publisher;
};

bool Publish(std::vector<FormatBytes> formats, uint32_t* written_sequence) {
  Operation operation;
  if (!operation.publisher || !operation.publisher->Ready()) return false;
  const uint32_t sequence = operation.publisher->Submit(std::move(formats));
  if (written_sequence) *written_sequence = sequence;
  return sequence != 0;
}
}  // namespace

bool PublishClipboard(const std::u16string& text, bool has_text, const std::u16string& html,
                      const uint8_t* image, size_t image_size, uint32_t* written_sequence) {
  if (written_sequence) *written_sequence = 0;
  try {
    std::vector<FormatBytes> formats;
    return MakeFormats(text, has_text, html, image, image_size, &formats)
        && Publish(std::move(formats), written_sequence);
  } catch (const std::bad_alloc&) { return false; }
}

bool ClearClipboardPublication(uint32_t* written_sequence) {
  if (written_sequence) *written_sequence = 0;
  try { return Publish({}, written_sequence); }
  catch (const std::bad_alloc&) { return false; }
}

bool RetainClipboardPublisher() {
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  if (state.clients == (std::numeric_limits<size_t>::max)()) return false;
  ++state.clients;
  return true;
}

void ReleaseClipboardPublisher() {
  auto& state = State();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.clients) return;
    --state.clients;
  }
  RetireIdle();
}
}  // namespace gesher::windows
