// Gesher media: http and https sources over WinHTTP (data_source.h).
//
// Reads are served from 256 KB chunks fetched with range requests (a few
// kept, least recently used first out). A server that ignores ranges is read
// incrementally into a private, delete-on-close disk spool. WinHTTP uses the system's proxy
// settings and validates certificates against the Windows store, as Gesher's
// TLS does (docs/architecture.md).

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <list>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

#include "../audio/log.h"
#include "data_source.h"

namespace gesher::media {
namespace {

constexpr size_t kChunk = 256 * 1024;
constexpr size_t kChunksKept = 16;
constexpr DWORD kTransfer = 64 * 1024;

std::wstring Wide(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  Handle() = default;
  explicit Handle(HINTERNET x) : h(x) {}
  ~Handle() { Reset(); }
  void Reset() {
    if (h != nullptr) WinHttpCloseHandle(h);
    h = nullptr;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

class HttpSource : public DataSource {
 public:
  HttpSource(std::string url, std::map<std::string, std::string> headers)
      : url_(std::move(url)), headers_(std::move(headers)) {}

  ~HttpSource() override {
    ReleaseWhole();  // request must close before its connection/session
    if (connect_ != nullptr) WinHttpCloseHandle(connect_);
    if (session_ != nullptr) WinHttpCloseHandle(session_);
  }

  bool Open(std::string* error) {
    std::wstring wurl = Wide(url_);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
      *error = "bad URL " + url_;
      return false;
    }
    secure_ = uc.nScheme == INTERNET_SCHEME_HTTPS;
    path_ = std::wstring(path, uc.dwUrlPathLength) + std::wstring(extra, uc.dwExtraInfoLength);
    session_ = WinHttpOpen(L"Gesher MediaPlayer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ == nullptr) {
      session_ = WinHttpOpen(L"Gesher MediaPlayer", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session_ == nullptr) {
      *error = "WinHttpOpen: " + std::to_string(GetLastError());
      return false;
    }
    WinHttpSetTimeouts(session_, 10000, 15000, 15000, 30000);
    connect_ = WinHttpConnect(session_, std::wstring(host, uc.dwHostNameLength).c_str(), uc.nPort, 0);
    if (connect_ == nullptr) {
      *error = "WinHttpConnect: " + std::to_string(GetLastError());
      return false;
    }
    return true;
  }

  // The first chunk tells the size, and whether ranges work: fetched by the
  // first read (in prepare, not setDataSource, as on Android).
  bool EnsureFirst() {
    if (first_done_) return first_ok_;
    std::lock_guard<std::mutex> once(first_mu_);
    if (first_done_) return first_ok_;
    std::vector<uint8_t> first;
    std::string error;
    first_ok_ = Fetch(0, &first, &error);
    if (first_ok_ && !whole_) {
      std::lock_guard<std::mutex> lock(mu_);
      Keep(0, std::move(first));
    } else if (!first_ok_) {
      MEDIA_LOGE("http: %s", error.c_str());
    }
    first_done_ = true;
    return first_ok_;
  }

  ssize_t ReadAt(int64_t offset, void* data, size_t size) override {
    if (offset < 0) return -EINVAL;
    if (cancelled_) return -EINTR;
    if (size == 0) return 0;
    if (!EnsureFirst()) return cancelled_ ? -EINTR : -EIO;
    size = std::min(size, static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
    if (whole_) return ReadWhole(offset, data, size);
    uint8_t* out = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
      if (cancelled_) return -EINTR;
      if (done > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - offset)) {
        return done > 0 ? static_cast<ssize_t>(done) : -EOVERFLOW;
      }
      int64_t pos = offset + static_cast<int64_t>(done);
      if (size_ >= 0 && pos >= size_) break;
      int64_t base = pos - pos % static_cast<int64_t>(kChunk);
      Chunk chunk;
      {
        std::lock_guard<std::mutex> lock(mu_);
        chunk = Find(base);
      }
      if (chunk == nullptr) {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!Fetch(base, &bytes, &error)) {
          MEDIA_LOGE("http: %s", error.c_str());
          return done > 0 ? static_cast<ssize_t>(done) : -EIO;
        }
        std::lock_guard<std::mutex> lock(mu_);
        chunk = Keep(base, std::move(bytes));
      }
      size_t in = static_cast<size_t>(pos - base);
      if (in >= chunk->size()) break;
      size_t n = std::min(size - done, chunk->size() - in);
      memcpy(out + done, chunk->data() + in, n);
      done += n;
    }
    return static_cast<ssize_t>(done);
  }

  int64_t Size() override {
    if (cancelled_ || !EnsureFirst()) return -1;
    if (whole_ && size_ < 0) {
      // Some demuxers need the total before probing the tail. For an unknown
      // Content-Length only this explicit size query drains the response.
      std::lock_guard<std::mutex> lock(whole_mu_);
      while (whole_request_.h != nullptr && !cancelled_) {
        if (!AppendWhole()) break;
      }
      if (cancelled_) { ReleaseWhole(); return -1; }
    }
    return size_;
  }
  std::string Uri() const override { return url_; }
  bool IsNetwork() const override { return true; }
  void Cancel() override {
    cancelled_ = true;
    // Closing an in-use synchronous WinHTTP request is unsafe. An active
    // reader observes cancellation after its current API call returns; an
    // idle request and its spool can be retired immediately.
    std::unique_lock<std::mutex> lock(whole_mu_, std::try_to_lock);
    if (lock.owns_lock()) ReleaseWhole();
  }

 private:
  // Readers retain immutable bytes after releasing mu_; an unrelated read
  // can then evict an LRU entry without invalidating the reader's copy.
  using Chunk = std::shared_ptr<const std::vector<uint8_t>>;

  // The cached chunk at `base`, most recently used first; mu_ held.
  Chunk Find(int64_t base) {
    for (auto it = chunks_.begin(); it != chunks_.end(); ++it) {
      if (it->first == base) {
        chunks_.splice(chunks_.begin(), chunks_, it);
        return chunks_.front().second;
      }
    }
    return nullptr;
  }

  Chunk Keep(int64_t base, std::vector<uint8_t> bytes) {
    // Concurrent misses can fetch the same range. Keep the first published
    // bytes and one cache entry, as a cache hit would have done.
    if (Chunk present = Find(base)) return present;
    chunks_.emplace_front(base, std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
    while (chunks_.size() > kChunksKept) chunks_.pop_back();
    return chunks_.front().second;
  }

  bool Fetch(int64_t base, std::vector<uint8_t>* out, std::string* error) {
    Handle req(WinHttpOpenRequest(connect_, L"GET", path_.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  secure_ ? WINHTTP_FLAG_SECURE : 0));
    if (req.h == nullptr) {
      *error = "WinHttpOpenRequest: " + std::to_string(GetLastError());
      return false;
    }
    std::wstring headers;
    for (const auto& [k, v] : headers_) headers += Wide(k) + L": " + Wide(v) + L"\r\n";
    if (!whole_) {
      const int64_t last = base + std::min(static_cast<int64_t>(kChunk) - 1,
                                          std::numeric_limits<int64_t>::max() - base);
      headers += L"Range: bytes=" + std::to_wstring(base) + L"-" +
                 std::to_wstring(last) + L"\r\n";
    }
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
      *error = url_ + ": request failed (" + std::to_string(GetLastError()) + ")";
      return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status == 416 && base > 0) {  // past the end
      out->clear();
      return true;
    }
    if (status != 200 && status != 206) {
      *error = url_ + ": HTTP " + std::to_string(status);
      return false;
    }
    if (base > 0 && status != 206) {
      // A server may ignore Range and return the complete resource. Its
      // first byte is not the requested offset; never cache it at that base.
      *error = url_ + ": server ignored nonzero byte range";
      return false;
    }
    if (status == 206) {
      wchar_t range[128];
      DWORD rlen = sizeof(range);
      if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range,
                              &rlen, WINHTTP_NO_HEADER_INDEX)) {
        const wchar_t* slash = wcschr(range, L'/');
        if (slash != nullptr && slash[1] != L'*') size_ = _wtoi64(slash + 1);
      }
    } else {
      // The initial 200 response is the complete resource, not a range. Keep
      // its one request alive and consume it only when a caller needs bytes.
      std::lock_guard<std::mutex> lock(whole_mu_);
      if (cancelled_) { *error = "cancelled"; return false; }
      whole_ = true;
      wchar_t clen[32];
      DWORD clen_size = sizeof(clen);
      if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen,
                              &clen_size, WINHTTP_NO_HEADER_INDEX)) {
        int64_t length = 0;
        bool valid = clen[0] != L'\0';
        for (const wchar_t* c = clen; valid && *c != L'\0'; ++c) {
          if (*c < L'0' || *c > L'9' ||
              length > (std::numeric_limits<int64_t>::max() - (*c - L'0')) / 10) valid = false;
          else length = length * 10 + (*c - L'0');
        }
        if (valid) size_ = whole_length_ = length;
      }
      whole_request_.h = req.h;
      req.h = nullptr;
      if (whole_length_ == 0) whole_request_.Reset();
      out->clear();
      return true;
    }
    out->clear();
    for (;;) {
      if (cancelled_) {
        *error = "cancelled";
        return false;
      }
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(req.h, &avail)) {
        *error = url_ + ": read failed (" + std::to_string(GetLastError()) + ")";
        return false;
      }
      if (avail == 0) break;
      size_t at = out->size();
      // A successful range response must fit the requested chunk. Do not
      // let a malformed 206 restore the unbounded-allocation path.
      if (avail > kChunk - at) {
        *error = url_ + ": oversized byte range";
        return false;
      }
      out->resize(at + avail);
      DWORD got = 0;
      if (!WinHttpReadData(req.h, out->data() + at, avail, &got)) {
        *error = url_ + ": read failed (" + std::to_string(GetLastError()) + ")";
        return false;
      }
      out->resize(at + got);
      if (got == 0) break;
    }
    return true;
  }

  // whole_mu_ protects the request, file cursor and the committed byte count.
  // Synchronous API calls on a request never overlap, including its close.
  void ReleaseWhole() {
    whole_request_.Reset();
    if (whole_file_ != INVALID_HANDLE_VALUE) CloseHandle(whole_file_);
    whole_file_ = INVALID_HANDLE_VALUE;
    std::vector<uint8_t>().swap(whole_buffer_);
  }

  bool FailWhole(const char* operation) {
    MEDIA_LOGE("http: %s (%lu)", operation, static_cast<unsigned long>(GetLastError()));
    whole_failed_ = true;
    whole_request_.Reset();
    return false;
  }

  bool OpenSpool() {
    if (whole_file_ != INVALID_HANDLE_VALUE) return true;
    wchar_t directory[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH + 1, directory);
    if (length == 0 || length > MAX_PATH) return FailWhole("temporary directory unavailable");
    static std::atomic<uint64_t> serial{0};
    for (int attempt = 0; attempt < 128; ++attempt) {
      const std::wstring path = std::wstring(directory, length) + L"gesher-media-" +
          std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
          std::to_wstring(serial.fetch_add(1)) + L".tmp";
      // CREATE_NEW claims the name atomically; never open/truncate someone
      // else's file. No inheritance, sharing or FILE_ATTRIBUTE_TEMPORARY:
      // ordinary lazy disk writeback remains available under memory pressure.
      whole_file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (whole_file_ != INVALID_HANDLE_VALUE) return true;
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    return FailWhole("create media spool");
  }

  bool SeekSpool(int64_t offset) {
    LARGE_INTEGER position;
    position.QuadPart = offset;
    return SetFilePointerEx(whole_file_, position, nullptr, FILE_BEGIN) != 0;
  }

  bool AppendWhole() {
    if (cancelled_) { ReleaseWhole(); return false; }
    if (whole_buffer_.empty()) whole_buffer_.resize(kTransfer);
    DWORD got = 0;
    if (!WinHttpReadData(whole_request_.h, whole_buffer_.data(), kTransfer, &got)) {
      return FailWhole("read whole HTTP body");
    }
    if (cancelled_) { ReleaseWhole(); return false; }
    if (got == 0) {
      whole_request_.Reset();
      if (whole_length_ >= 0 && whole_size_ != whole_length_) {
        return FailWhole("truncated whole HTTP body");
      }
      size_ = whole_size_;
      return true;
    }
    if (got > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - whole_size_) ||
        (whole_length_ >= 0 && static_cast<int64_t>(got) > whole_length_ - whole_size_)) {
      return FailWhole("whole HTTP body exceeds declared length");
    }
    if (!OpenSpool()) return false;
    if (!SeekSpool(whole_size_)) return FailWhole("seek media spool for write");
    DWORD written = 0;
    while (written < got) {
      DWORD count = 0;
      if (!WriteFile(whole_file_, whole_buffer_.data() + written, got - written, &count, nullptr) ||
          count == 0) return FailWhole("write media spool");
      written += count;
      if (cancelled_) { ReleaseWhole(); return false; }
    }
    whole_size_ += got;  // publish only a complete, successfully stored block
    if (whole_size_ == whole_length_) whole_request_.Reset();
    return true;
  }

  ssize_t ReadWhole(int64_t offset, void* data, size_t size) {
    std::lock_guard<std::mutex> lock(whole_mu_);
    if (cancelled_) { ReleaseWhole(); return -EINTR; }
    const int64_t known = size_;
    if (known >= 0 && offset >= known) return 0;
    const int64_t available = std::numeric_limits<int64_t>::max() - offset;
    int64_t end = offset + static_cast<int64_t>(std::min(size, static_cast<size_t>(available)));
    if (known >= 0) end = std::min(end, known);
    while (whole_size_ < end && whole_request_.h != nullptr) {
      if (!AppendWhole()) break;
    }
    if (cancelled_) { ReleaseWhole(); return -EINTR; }
    if (offset >= whole_size_) return whole_failed_ ? -EIO : 0;
    size = std::min(size, static_cast<size_t>(whole_size_ - offset));
    if (!SeekSpool(offset)) return -EIO;
    size_t done = 0;
    while (done < size) {
      DWORD got = 0;
      const DWORD count = static_cast<DWORD>(std::min(size - done, static_cast<size_t>(kTransfer)));
      if (!ReadFile(whole_file_, static_cast<uint8_t*>(data) + done, count, &got, nullptr) || got == 0) {
        return done > 0 ? static_cast<ssize_t>(done) : -EIO;
      }
      done += got;
      if (cancelled_) { ReleaseWhole(); return -EINTR; }
    }
    return static_cast<ssize_t>(done);
  }

  std::string url_;
  std::map<std::string, std::string> headers_;
  std::wstring path_;
  bool secure_ = false;
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  std::atomic<int64_t> size_{-1};
  std::atomic<bool> whole_{false};
  std::atomic<bool> cancelled_{false};
  std::mutex whole_mu_;
  Handle whole_request_;
  HANDLE whole_file_ = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> whole_buffer_;
  int64_t whole_size_ = 0;
  int64_t whole_length_ = -1;
  bool whole_failed_ = false;
  std::mutex first_mu_;
  std::atomic<bool> first_done_{false};
  bool first_ok_ = false;
  std::mutex mu_;
  std::list<std::pair<int64_t, Chunk>> chunks_;
};

}  // namespace

std::unique_ptr<DataSource> OpenHttpSource(const std::string& url,
                                           const std::map<std::string, std::string>& headers,
                                           std::string* error) {
  auto s = std::make_unique<HttpSource>(url, headers);
  if (!s->Open(error)) return nullptr;
  return s;
}

}  // namespace gesher::media
