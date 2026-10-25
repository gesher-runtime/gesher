// Gesher media: http and https sources over WinHTTP (data_source.h).
//
// Reads are served from 256 KB chunks fetched with range requests (a few
// kept, least recently used first out); a server that ignores ranges sends
// the whole file once, kept in memory. WinHTTP uses the system's proxy
// settings and validates certificates against the Windows store, as Gesher's
// TLS does (docs/architecture.md).

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "../audio/log.h"
#include "data_source.h"

namespace gesher::media {
namespace {

constexpr size_t kChunk = 256 * 1024;
constexpr size_t kChunksKept = 16;

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
  ~Handle() {
    if (h != nullptr) WinHttpCloseHandle(h);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

class HttpSource : public DataSource {
 public:
  HttpSource(std::string url, std::map<std::string, std::string> headers)
      : url_(std::move(url)), headers_(std::move(headers)) {}

  ~HttpSource() override {
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
    if (first_ok_) {
      std::lock_guard<std::mutex> lock(mu_);
      Keep(0, std::move(first));
    } else {
      MEDIA_LOGE("http: %s", error.c_str());
    }
    first_done_ = true;
    return first_ok_;
  }

  ssize_t ReadAt(int64_t offset, void* data, size_t size) override {
    if (offset < 0) return -EINVAL;
    if (!EnsureFirst()) return -EIO;
    uint8_t* out = static_cast<uint8_t*>(data);
    size_t done = 0;
    while (done < size) {
      if (cancelled_) return -EINTR;
      int64_t pos = offset + static_cast<int64_t>(done);
      if (size_ >= 0 && pos >= size_) break;
      int64_t base = whole_ ? 0 : pos - pos % static_cast<int64_t>(kChunk);
      const std::vector<uint8_t>* chunk = nullptr;
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
    EnsureFirst();
    return size_;
  }
  std::string Uri() const override { return url_; }
  bool IsNetwork() const override { return true; }
  void Cancel() override { cancelled_ = true; }

 private:
  // The cached chunk at `base`, most recently used first; mu_ held.
  const std::vector<uint8_t>* Find(int64_t base) {
    for (auto it = chunks_.begin(); it != chunks_.end(); ++it) {
      if (it->first == base) {
        chunks_.splice(chunks_.begin(), chunks_, it);
        return &chunks_.front().second;
      }
    }
    return nullptr;
  }

  const std::vector<uint8_t>* Keep(int64_t base, std::vector<uint8_t> bytes) {
    chunks_.emplace_front(base, std::move(bytes));
    while (chunks_.size() > kChunksKept && !whole_) chunks_.pop_back();
    return &chunks_.front().second;
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
      headers += L"Range: bytes=" + std::to_wstring(base) + L"-" +
                 std::to_wstring(base + static_cast<int64_t>(kChunk) - 1) + L"\r\n";
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
    if (status == 206) {
      wchar_t range[128];
      DWORD rlen = sizeof(range);
      if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, range,
                              &rlen, WINHTTP_NO_HEADER_INDEX)) {
        const wchar_t* slash = wcschr(range, L'/');
        if (slash != nullptr && slash[1] != L'*') size_ = _wtoi64(slash + 1);
      }
    } else if (base == 0 || whole_) {
      // No ranges: the body is the whole file.
      whole_ = true;
      wchar_t clen[32];
      DWORD clen_size = sizeof(clen);
      if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clen,
                              &clen_size, WINHTTP_NO_HEADER_INDEX)) {
        size_ = _wtoi64(clen);
      }
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
      out->resize(at + avail);
      DWORD got = 0;
      if (!WinHttpReadData(req.h, out->data() + at, avail, &got)) {
        *error = url_ + ": read failed (" + std::to_string(GetLastError()) + ")";
        return false;
      }
      out->resize(at + got);
    }
    if (whole_) size_ = static_cast<int64_t>(out->size());
    return true;
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
  std::mutex first_mu_;
  std::atomic<bool> first_done_{false};
  bool first_ok_ = false;
  std::mutex mu_;
  std::list<std::pair<int64_t, std::vector<uint8_t>>> chunks_;
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
