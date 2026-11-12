// Compile the entire production HttpSource against an explicit WinHTTP boundary.
// No real Windows networking is claimed by this host fixture.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "windows.h"
#include "winhttp.h"
#include "compat/media/player/data_source.h"

namespace {
constexpr int64_t kFixtureChunk = 256 * 1024;
constexpr int64_t kFixtureLength = 24 * kFixtureChunk + 13;
std::atomic<int> checks{0}, failures{0}, handles{0}, requests{0};
std::atomic<int64_t> last_range_end{-1};
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL HTTP source: %s\n", reason); }
  return ok;
}
uint8_t ByteAt(int64_t offset) { return static_cast<uint8_t>(offset % 251); }
}

struct HttpContractHandle {
  std::wstring path;
  DWORD status = 0;
  std::wstring range;
  std::vector<uint8_t> bytes;
  size_t cursor = 0;
  HttpContractHandle() { ++handles; }
  ~HttpContractHandle() { --handles; }
};

DWORD GetLastError() { return 1234; }
int MultiByteToWideChar(unsigned, DWORD, const char* input, int, wchar_t* out, int capacity) {
  const int count = static_cast<int>(std::strlen(input)) + 1;
  if (out == nullptr) return count;
  if (capacity < count) return 0;
  for (int i = 0; i < count; ++i) out[i] = static_cast<unsigned char>(input[i]);
  return count;
}
BOOL WinHttpCrackUrl(const wchar_t* input, DWORD, DWORD, URL_COMPONENTS* out) {
  std::wstring url(input);
  const auto scheme = url.find(L"://");
  if (scheme == std::wstring::npos) return false;
  const auto start = scheme + 3, slash = url.find(L'/', start);
  const std::wstring host = url.substr(start, slash - start);
  const std::wstring path = slash == std::wstring::npos ? L"/" : url.substr(slash);
  if (host.size() >= out->dwHostNameLength || path.size() >= out->dwUrlPathLength) return false;
  std::copy(host.begin(), host.end(), out->lpszHostName);
  std::copy(path.begin(), path.end(), out->lpszUrlPath);
  out->dwHostNameLength = static_cast<DWORD>(host.size());
  out->dwUrlPathLength = static_cast<DWORD>(path.size());
  out->dwExtraInfoLength = 0;
  out->nScheme = url.substr(0, scheme) == L"https" ? INTERNET_SCHEME_HTTPS : INTERNET_SCHEME_HTTP;
  return true;
}
HINTERNET WinHttpOpen(const wchar_t*, DWORD, const wchar_t*, const wchar_t*, DWORD) {
  return new HttpContractHandle;
}
BOOL WinHttpCloseHandle(HINTERNET handle) { delete handle; return true; }
BOOL WinHttpSetTimeouts(HINTERNET, int, int, int, int) { return true; }
HINTERNET WinHttpConnect(HINTERNET, const wchar_t*, INTERNET_PORT, DWORD) {
  return new HttpContractHandle;
}
HINTERNET WinHttpOpenRequest(HINTERNET, const wchar_t*, const wchar_t* path, const wchar_t*,
                           const wchar_t*, const wchar_t* const*, DWORD) {
  auto* request = new HttpContractHandle;
  request->path = path;
  return request;
}
BOOL WinHttpSendRequest(HINTERNET req, const wchar_t* raw, DWORD, void*, DWORD, DWORD, uintptr_t) {
  ++requests;
  std::wstring headers = raw == nullptr ? L"" : raw;
  int64_t base = 0;
  const auto range = headers.find(L"Range: bytes=");
  if (range != std::wstring::npos) {
    wchar_t* end = nullptr;
    base = std::wcstoll(headers.c_str() + range + 13, &end, 10);
    Check(end != nullptr && *end == L'-', "range syntax");
    last_range_end = std::wcstoll(end + 1, nullptr, 10);
  }
  if (req->path == L"/error") { req->status = 500; return true; }
  if (req->path == L"/whole" || (req->path == L"/ignore" && base > 0)) {
    req->status = 200;
    const std::string all = "whole-file-source";
    req->bytes.assign(all.begin(), all.end());
    return true;
  }
  if (base >= kFixtureLength) { req->status = 416; return true; }
  req->status = 206;
  const auto count = std::min(kFixtureChunk, kFixtureLength - base);
  req->bytes.resize(static_cast<size_t>(count));
  for (size_t i = 0; i < req->bytes.size(); ++i) req->bytes[i] = ByteAt(base + static_cast<int64_t>(i));
  req->range = L"bytes " + std::to_wstring(base) + L"-" + std::to_wstring(base + count - 1)
      + L"/" + (req->path == L"/unknown" ? L"*" : std::to_wstring(kFixtureLength));
  return true;
}
BOOL WinHttpReceiveResponse(HINTERNET, void*) { return true; }
BOOL WinHttpQueryHeaders(HINTERNET req, DWORD query, const wchar_t*, void* output,
                         DWORD* bytes, DWORD*) {
  if (query == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
    if (*bytes < sizeof(DWORD)) return false;
    *static_cast<DWORD*>(output) = req->status;
    return true;
  }
  std::wstring text;
  if (query == WINHTTP_QUERY_CONTENT_RANGE && !req->range.empty()) text = req->range;
  else if (query == WINHTTP_QUERY_CONTENT_LENGTH) text = std::to_wstring(req->bytes.size());
  else return false;
  const size_t length = (text.size() + 1) * sizeof(wchar_t);
  if (*bytes < length) return false;
  std::memcpy(output, text.c_str(), length);
  *bytes = static_cast<DWORD>(length);
  return true;
}
BOOL WinHttpQueryDataAvailable(HINTERNET req, DWORD* count) {
  *count = static_cast<DWORD>(req->bytes.size() - req->cursor);
  return true;
}
BOOL WinHttpReadData(HINTERNET req, void* out, DWORD count, DWORD* got) {
  *got = static_cast<DWORD>(std::min(static_cast<size_t>(count), req->bytes.size() - req->cursor));
  std::memcpy(out, req->bytes.data() + req->cursor, *got);
  req->cursor += *got;
  return true;
}

// Only access changes for deterministic cache ownership inspection. All production
// method bodies and their standard-library members are compiled unchanged.
#define private public
#include GESHER_HTTP_SOURCE
#undef private

namespace {
using gesher::media::HttpSource;
std::unique_ptr<gesher::media::DataSource> Open(const char* endpoint) {
  std::string error;
  auto source = gesher::media::OpenHttpSource(std::string("http://contract.test/") + endpoint, {}, &error);
  Check(source != nullptr, "source opens through transport boundary");
  return source;
}
template <typename Retained>
void EvictionLifetime(HttpSource& source, Retained retained) {
  if constexpr (std::is_pointer_v<Retained>) {
    // The old source cannot establish ownership. Do not dereference an evicted
    // raw pointer or turn the old-source control into a race/crash experiment.
    Check(false, "cache reader holds an owning immutable chunk");
  } else {
    static_assert(std::is_const_v<typename Retained::element_type>);
    std::weak_ptr<const std::vector<uint8_t>> weak = retained;
    for (int i = 1; i <= 17; ++i) source.Keep(i * kFixtureChunk, {static_cast<uint8_t>(i)});
    Check(source.chunks_.size() == 16, "LRU remains bounded");
    Check(source.Find(0) == nullptr, "retained oldest chunk is evicted from cache");
    Check(!weak.expired() && retained->size() == 1 && retained->front() == 42,
          "reader still owns the exact evicted bytes");
    retained.reset();
    Check(weak.expired(), "evicted bytes retire after the last reader");
  }
}
}

int main() {
  {
    auto source = Open("range");
    Check(source->Size() == kFixtureLength, "206 resource length");
    uint8_t bytes[32] = {};
    Check(source->ReadAt(kFixtureChunk + 7, bytes, sizeof(bytes)) == sizeof(bytes), "nonzero 206 read");
    bool exact = true;
    for (size_t i = 0; i < sizeof(bytes); ++i) exact &= bytes[i] == ByteAt(kFixtureChunk + 7 + i);
    Check(exact, "206 bytes belong to requested position");
    Check(source->ReadAt(kFixtureLength - 3, bytes, sizeof(bytes)) == 3, "final partial range");
    Check(source->ReadAt(kFixtureLength, bytes, 1) == 0, "known resource EOF");
    Check(source->ReadAt(-1, bytes, 1) == -EINVAL, "negative HTTP position");
    source->Cancel();
    Check(source->ReadAt(0, bytes, 1) == -EINTR, "cancelled reads fail");
  }
  {
    auto source = Open("whole");
    uint8_t bytes[32] = {};
    Check(source->ReadAt(6, bytes, std::numeric_limits<size_t>::max()) == 11
              && std::memcmp(bytes, "file-source", 11) == 0, "initial 200 supports whole-file seek");
    Check(source->Size() == 17, "whole-file size");
  }
  {
    auto source = Open("ignore");
    uint8_t bytes[32] = {};
    Check(source->ReadAt(kFixtureChunk + 7, bytes, sizeof(bytes)) == -EIO,
          "200 cannot satisfy a nonzero range");
    Check(source->ReadAt(kFixtureChunk - 2, bytes, 4) == 2
              && bytes[0] == ByteAt(kFixtureChunk - 2) && bytes[1] == ByteAt(kFixtureChunk - 1),
          "range error preserves prior bytes from a partial read");
  }
  {
    auto source = Open("unknown");
    Check(source->Size() == -1, "star range preserves unknown size");
    uint8_t byte = 0;
    Check(source->ReadAt(kFixtureLength + kFixtureChunk, &byte, 1) == 0, "416 is range EOF");
    Check(source->ReadAt(INT64_MAX, &byte, 1) == 0 && last_range_end == INT64_MAX,
          "last representable range has no end overflow");
  }
  {
    auto source = Open("error");
    uint8_t byte = 0;
    Check(source->ReadAt(0, &byte, 1) == -EIO && source->Size() == -1, "initial HTTP failure");
  }
  {
    HttpSource source("", {});
    auto first = source.Keep(0, {42});
    auto again = source.Keep(0, {99});
    Check(first == again && source.chunks_.size() == 1 && first->front() == 42,
          "concurrent duplicate publication keeps one immutable chunk");
    again = nullptr;
    EvictionLifetime(source, std::move(first));
  }
  // Ordinary concurrent reads are safe to run only once owning chunk semantics
  // have been established; the old-source control above reports their absence.
  if (failures == 0) {
    auto source = Open("range");
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) readers.emplace_back([&, t] {
      for (int j = 0; j < 40; ++j) {
        const int64_t at = ((t * 7 + j) % 24) * kFixtureChunk + 9;
        uint8_t bytes[16] = {};
        bool valid = source->ReadAt(at, bytes, sizeof(bytes)) == sizeof(bytes);
        for (size_t i = 0; i < sizeof(bytes); ++i) valid &= bytes[i] == ByteAt(at + i);
        Check(valid, "concurrent read preserves requested bytes across LRU eviction");
      }
    });
    for (auto& reader : readers) reader.join();
  }
  Check(handles == 0, "all request, connection and session handles close");
  std::printf("HTTP source host contracts: %d checks, %d failures, %d requests\n",
              checks.load(), failures.load(), requests.load());
  return failures != 0;
}
