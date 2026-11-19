// Compile the entire production HttpSource against an explicit WinHTTP boundary.
// No real Windows networking is claimed by this host fixture.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <unistd.h>
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
std::atomic<int64_t> transport_bytes{0}, spool_bytes{0};
std::atomic<int> file_handles{0};
std::atomic<DWORD> max_http_read{0};
std::atomic<bool> fail_create{false}, fail_write{false}, fail_read{false}, fail_seek{false};
std::atomic<bool> zero_write{false}, zero_read{false}, short_file_io{false};
std::atomic<int64_t> fail_write_after{-1};
std::mutex blocked_mu;
std::condition_variable blocked_cv;
bool read_blocked = false, release_read = false;
std::atomic<bool> switch_to_whole{false};
std::mutex transition_mu;
std::condition_variable transition_cv;
int transition_arrived = 0;
bool transition_release_second = false;
std::atomic<int> transition_returned{0};
thread_local DWORD last_error = 1234;
constexpr int64_t kLargeLength = 128 * 1024 * 1024 + 13;
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
  int64_t generated_length = -1;
  int64_t declared_length = -1;
  int64_t fail_at = -1;
  bool unknown_length = false;
  DWORD read_limit = std::numeric_limits<DWORD>::max();
  size_t Length() const { return generated_length >= 0 ? static_cast<size_t>(generated_length) : bytes.size(); }
  HttpContractHandle() { ++handles; }
  ~HttpContractHandle() { --handles; }
};

DWORD GetLastError() { return last_error; }
struct HttpFileHandle { FILE* file; std::string path; };
DWORD GetTempPathW(DWORD size, wchar_t* out) {
  const std::string path = std::filesystem::temp_directory_path().string() + "/";
  if (size <= path.size()) return static_cast<DWORD>(path.size() + 1);
  for (size_t i = 0; i <= path.size(); ++i) out[i] = static_cast<unsigned char>(path[i]);
  return static_cast<DWORD>(path.size());
}
DWORD GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
uint64_t GetTickCount64() { return 123456789; }
HANDLE CreateFileW(const wchar_t* name, DWORD access, DWORD sharing, void* inherit,
                   DWORD disposition, DWORD flags, HANDLE) {
  Check(access == (GENERIC_READ | GENERIC_WRITE) && sharing == 0 && inherit == nullptr,
        "spool is private and non-inheritable");
  Check(disposition == CREATE_NEW && flags == (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE),
        "spool atomically creates an owned disk file with deletion on close");
  if (fail_create) return INVALID_HANDLE_VALUE;
  std::string path;
  while (*name != 0) path += static_cast<char>(*name++);
  FILE* f = std::fopen(path.c_str(), "w+bx");
  if (f == nullptr) { last_error = errno == EEXIST ? ERROR_FILE_EXISTS : 1234; return INVALID_HANDLE_VALUE; }
  ++file_handles;
  return new HttpFileHandle{f, path};
}
BOOL CloseHandle(HANDLE file) {
  Check(file != nullptr && file != INVALID_HANDLE_VALUE, "close valid spool handle");
  const std::string path = file->path;
  std::fclose(file->file);
  const int removed = std::remove(path.c_str());
  Check(removed == 0 && !std::filesystem::exists(path), "spool removed on close");
  delete file;
  --file_handles;
  return true;
}
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER at, LARGE_INTEGER*, DWORD origin) {
  Check(origin == FILE_BEGIN && at.QuadPart >= 0, "spool uses nonnegative absolute offsets");
  if (fail_seek) return false;
  return fseeko(file->file, static_cast<off_t>(at.QuadPart), SEEK_SET) == 0;
}
BOOL WriteFile(HANDLE file, const void* data, DWORD size, DWORD* written, void*) {
  *written = 0;
  if (fail_write) return false;
  if (zero_write) return true;
  if (short_file_io) size = std::min<DWORD>(size, 997);
  if (fail_write_after >= 0) {
    if (fail_write_after == 0) return false;
    size = static_cast<DWORD>(std::min<int64_t>(size, fail_write_after));
    fail_write_after -= size;
  }
  *written = static_cast<DWORD>(std::fwrite(data, 1, size, file->file));
  spool_bytes += *written;
  return std::ferror(file->file) == 0;
}
BOOL ReadFile(HANDLE file, void* data, DWORD size, DWORD* read, void*) {
  *read = 0;
  if (fail_read) return false;
  if (zero_read) return true;
  if (short_file_io) size = std::min<DWORD>(size, 991);
  *read = static_cast<DWORD>(std::fread(data, 1, size, file->file));
  return std::ferror(file->file) == 0;
}
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
  if (req->path == L"/stream-headers-blocked") {
    std::unique_lock<std::mutex> lock(blocked_mu);
    read_blocked = true;
    blocked_cv.notify_all();
    blocked_cv.wait(lock, [] { return release_read; });
  }
  std::wstring headers = raw == nullptr ? L"" : raw;
  int64_t base = 0;
  const auto range = headers.find(L"Range: bytes=");
  if (range != std::wstring::npos) {
    wchar_t* end = nullptr;
    base = std::wcstoll(headers.c_str() + range + 13, &end, 10);
    Check(end != nullptr && *end == L'-', "range syntax");
    last_range_end = std::wcstoll(end + 1, nullptr, 10);
  }
  if (req->path == L"/concurrent-transition" && switch_to_whole) {
    std::unique_lock<std::mutex> lock(transition_mu);
    const int ordinal = ++transition_arrived;
    transition_cv.notify_all();
    if (ordinal == 1) {
      Check(transition_cv.wait_for(lock, std::chrono::seconds(5), [] {return transition_arrived >= 2;}),
            "two ordinary cache misses reach response boundary");
    } else {
      Check(transition_cv.wait_for(lock, std::chrono::seconds(5), [] {return transition_release_second;}),
            "second response released after prefix observation");
    }
    req->status = 200;
    req->generated_length = kFixtureLength;
    return true;
  }
  if (req->path == L"/range-then-whole" && switch_to_whole) {
    req->status = 200;
    req->generated_length = kFixtureLength;
    return true;
  }
  if (req->path.starts_with(L"/stream")) {
    req->status = 200;
    req->generated_length = req->path == L"/stream-empty" ? 0 : 2 * kFixtureChunk + 13;
    if (req->path == L"/stream-large") req->generated_length = kLargeLength;
    req->unknown_length = req->path == L"/stream-unknown";
    if (req->path == L"/stream-short") { req->read_limit = 73; req->generated_length = 12345; }
    if (req->path == L"/stream-truncated") {
      req->generated_length = 1000;
      req->declared_length = 2000;
    }
    if (req->path == L"/stream-bad-length") req->declared_length = 10;
    if (req->path == L"/stream-error") req->fail_at = 64 * 1024;
    return true;
  }
  if (req->path == L"/oversized-range") {
    req->status = 206;
    req->generated_length = kFixtureChunk + 1;
    return true;
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
  else if (query == WINHTTP_QUERY_CONTENT_LENGTH && !req->unknown_length)
    text = std::to_wstring(req->declared_length >= 0 ? req->declared_length : static_cast<int64_t>(req->Length()));
  else return false;
  const size_t length = (text.size() + 1) * sizeof(wchar_t);
  if (*bytes < length) return false;
  std::memcpy(output, text.c_str(), length);
  *bytes = static_cast<DWORD>(length);
  return true;
}
BOOL WinHttpQueryDataAvailable(HINTERNET req, DWORD* count) {
  *count = static_cast<DWORD>(req->Length() - req->cursor);
  return true;
}
BOOL WinHttpReadData(HINTERNET req, void* out, DWORD count, DWORD* got) {
  if ((req->path == L"/stream-blocked" && req->cursor == 0) ||
      (req->path == L"/stream-blocked-tail" && req->cursor >= 65536)) {
    std::unique_lock<std::mutex> lock(blocked_mu);
    read_blocked = true;
    blocked_cv.notify_all();
    blocked_cv.wait(lock, [] { return release_read; });
  }
  if (req->fail_at >= 0 && static_cast<int64_t>(req->cursor) >= req->fail_at) return false;
  max_http_read.store(std::max(max_http_read.load(), count));
  *got = static_cast<DWORD>(std::min({static_cast<size_t>(count), req->Length() - req->cursor,
                                     static_cast<size_t>(req->read_limit)}));
  if (req->generated_length >= 0) {
    for (DWORD i = 0; i < *got; ++i) static_cast<uint8_t*>(out)[i] = ByteAt(req->cursor + i);
  } else if (*got != 0) std::memcpy(out, req->bytes.data() + req->cursor, *got);
  req->cursor += *got;
  transport_bytes += *got;
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

namespace {
template<class Source>
size_t PayloadCapacity(const Source& source) {
  size_t total = 0;
  for (const auto& item : source.chunks_) total += item.second->capacity();
  if constexpr (requires { source.whole_buffer_; }) total += source.whole_buffer_.capacity();
  return total;
}
void CheckBytes(gesher::media::DataSource& source, int64_t offset, size_t count, const char* reason) {
  std::vector<uint8_t> bytes(count + 32, 0xed);
  bool exact = source.ReadAt(offset, bytes.data() + 16, count) == static_cast<ssize_t>(count);
  for (size_t i = 0; i < count; ++i) exact &= bytes[i + 16] == ByteAt(offset + i);
  for (size_t i = 0; i < 16; ++i) exact &= bytes[i] == 0xed && bytes[count + 16 + i] == 0xed;
  Check(exact, reason);
}
void Measurements(bool gate) {
  transport_bytes = 0; spool_bytes = 0; max_http_read = 0;
  const int calls = requests;
  auto source = Open("stream-large");
  const auto begin = std::chrono::steady_clock::now();
  CheckBytes(*source, 17, 32, "large whole resource returns exact initial bytes and guards");
  const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - begin).count();
  const int64_t first_transfer = transport_bytes;
  const int64_t first_spooled = spool_bytes;
  const size_t first_payload = PayloadCapacity(static_cast<HttpSource&>(*source));
  if (gate) {
    Check(first_transfer <= 64 * 1024, "first read does not consume the complete 128 MiB body");
    Check(first_payload <= 64 * 1024, "whole body payload capacity is at most 64 KiB");
    Check(max_http_read <= 64 * 1024, "every whole-body transport read is bounded");
  }
  CheckBytes(*source, kLargeLength - 13, 13, "forward seek spools exact final bytes");
  const size_t full_payload = PayloadCapacity(static_cast<HttpSource&>(*source));
  const int64_t after_tail = transport_bytes;
  CheckBytes(*source, 123, 101, "backward seek reads exact persisted bytes");
  Check(transport_bytes == after_tail && requests == calls + 1, "backward seek never refetches a whole resource");
  Check(source->Size() == kLargeLength, "full resource retains exact length");
  if (gate) {
    Check(full_payload <= 64 * 1024, "full transfer does not grow the retained payload buffer");
    Check(handles == 2, "completed known-length request closes before source destruction");
  }
  std::printf("HTTP whole-body measurement: resource=%lld first_transfer=%lld first_spool=%lld "
              "first_payload_capacity=%zu full_payload_capacity=%zu first_read_us=%lld total_transfer=%lld\n",
              static_cast<long long>(kLargeLength), static_cast<long long>(first_transfer),
              static_cast<long long>(first_spooled), first_payload, full_payload,
              static_cast<long long>(micros), static_cast<long long>(after_tail));
}
void StreamingContracts() {
  {
    auto source = Open("stream-large");
    const int calls = requests;
    Check(source->ReadAt(0, nullptr, 0) == 0 && requests == calls,
          "zero-sized read performs no request or spool allocation");
    const int64_t before = transport_bytes;
    Check(source->Size() == kLargeLength && transport_bytes == before,
          "known length query does not drain the response");
    source->Cancel();
    Check(source->ReadAt(0, nullptr, 0) == -EINTR && source->Size() == -1,
          "cancelled source rejects reads and size query");
    Check(file_handles == 0 && handles == 2, "idle cancellation retires request without creating spool");
  }
  {
    auto source = Open("stream-unknown");
    const int64_t before = transport_bytes;
    CheckBytes(*source, 23, 103, "unknown-length 200 permits an early read");
    Check(transport_bytes - before <= 64 * 1024, "unknown length early read also stops promptly");
    Check(source->Size() == 2 * kFixtureChunk + 13, "unknown length query discovers exact EOF through spool");
    Check(handles == 2, "unknown-length EOF closes request");
    CheckBytes(*source, kFixtureChunk - 7, 41, "seek after unknown-length drain preserves bytes");
    uint8_t bytes[32] = {};
    Check(source->ReadAt(2 * kFixtureChunk + 10, bytes, sizeof(bytes)) == 3,
          "partial final unknown-length read");
    Check(source->ReadAt(INT64_MAX, bytes, 1) == 0, "highest offset is EOF after size discovery");
  }
  {
    auto source = Open("stream-empty");
    uint8_t byte = 99;
    Check(source->Size() == 0 && source->ReadAt(0, &byte, 1) == 0 && byte == 99,
          "empty body has zero size and leaves output unchanged");
    Check(handles == 2 && file_handles == 0, "empty body retains no request or spool");
  }
  {
    short_file_io = true;
    auto source = Open("stream-short");
    CheckBytes(*source, 17, 8000, "short network and disk reads/writes preserve every byte");
    short_file_io = false;
    CheckBytes(*source, 0, 12345, "short reads retain seekable whole source");
  }
  {
    auto source = Open("stream-truncated");
    uint8_t bytes[3000];
    std::memset(bytes, 0xed, sizeof(bytes));
    Check(source->ReadAt(0, bytes, 2000) == 1000, "premature EOF returns only received prefix");
    bool exact = true;
    for (size_t i = 0; i < 1000; ++i) exact &= bytes[i] == ByteAt(i);
    for (size_t i = 1000; i < sizeof(bytes); ++i) exact &= bytes[i] == 0xed;
    Check(exact, "truncated response never fabricates bytes past received prefix");
    Check(source->ReadAt(1000, bytes, 1) == -EIO, "premature EOF is sticky error rather than ordinary EOF");
    CheckBytes(*source, 11, 17, "verified prefix survives a later transport error");
  }
  {
    auto source = Open("stream-bad-length");
    uint8_t byte = 99;
    Check(source->ReadAt(0, &byte, 1) == -EIO && byte == 99,
          "transport body exceeding declared length is rejected");
  }
  {
    auto source = Open("stream-error");
    CheckBytes(*source, 0, 32, "initial prefix arrives before later network error");
    uint8_t byte = 99;
    Check(source->ReadAt(64 * 1024, &byte, 1) == -EIO && byte == 99,
          "later read failure is reported without fabricated data");
  }
  {
    auto source = Open("oversized-range");
    uint8_t byte = 99;
    Check(source->ReadAt(0, &byte, 1) == -EIO && byte == 99,
          "oversized 206 cannot allocate an unbounded range buffer");
  }
  for (int fault = 0; fault < 6; ++fault) {
    {
      auto source = Open("stream-large");
      fail_create = fault == 0; fail_write = fault == 1; fail_seek = fault == 2;
      zero_write = fault == 3; fail_write_after = fault == 4 ? 113 : -1;
      fail_read = fault == 5;
      uint8_t byte = 99;
      Check(source->ReadAt(0, &byte, 1) == -EIO && byte == 99, "spool failure reports error without success bytes");
      fail_create = fail_write = fail_seek = zero_write = fail_read = false;
      fail_write_after = -1;
      if (fault < 5) Check(source->ReadAt(0, &byte, 1) == -EIO,
                           "failed spool append does not publish partial disk bytes");
    }
    Check(file_handles == 0 && handles == 0, "faulted source retires file and network handles");
  }
  {
    auto source = Open("stream-large");
    CheckBytes(*source, 0, 31, "spool prepares bytes before read fault");
    zero_read = true;
    uint8_t byte = 99;
    Check(source->ReadAt(0, &byte, 1) == -EIO && byte == 99,
          "unexpected disk EOF is error, not a shortened HTTP resource");
    zero_read = false;
  }
  for (const char* endpoint : {"stream-blocked", "stream-headers-blocked"}) {
    auto source = Open(endpoint);
    { std::lock_guard<std::mutex> lock(blocked_mu); read_blocked = false; release_read = false; }
    ssize_t result = 0;
    std::thread reader([&] { uint8_t byte; result = source->ReadAt(0, &byte, 1); });
    {
      std::unique_lock<std::mutex> lock(blocked_mu);
      Check(blocked_cv.wait_for(lock, std::chrono::seconds(2), [] { return read_blocked; }),
            "controlled synchronous request reaches blocked boundary");
    }
    source->Cancel();
    Check(handles == 3, "cancel does not close a handle while synchronous WinHTTP uses it");
    { std::lock_guard<std::mutex> lock(blocked_mu); release_read = true; }
    blocked_cv.notify_all();
    reader.join();
    Check(result == -EINTR && handles == 2 && file_handles == 0,
          "returning synchronous operation observes cancel and retires request");
  }
  {
    auto source = Open("stream-large");
    CheckBytes(*source, 17, 61, "idle spool has an initial prefix");
    source->Cancel();
    Check(file_handles == 0 && handles == 2, "cancel promptly removes idle spool and request");
  }
  {
    auto source = Open("stream-concurrent");
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) readers.emplace_back([&, t] {
      for (int j = 0; j < 15; ++j) {
        const int64_t offset = (t * 9901 + j * 32771) % (2 * kFixtureChunk - 100);
        CheckBytes(*source, offset, 79, "concurrent whole-file readers retain exact random-access bytes");
      }
    });
    for (auto& reader : readers) reader.join();
  }
  Check(file_handles == 0 && handles == 0, "all streaming paths retire their resources");
}

void CachedByteContracts() {
  {
    auto source = Open("stream-blocked-tail");
    CheckBytes(*source, 0, 32, "cache snapshot prepares a committed prefix before blocked I/O");
    { std::lock_guard<std::mutex> lock(blocked_mu); read_blocked = false; release_read = false; }
    std::thread reader([&] { CheckBytes(*source, 65536, 1, "blocked tail eventually completes"); });
    {
      std::unique_lock<std::mutex> lock(blocked_mu);
      Check(blocked_cv.wait_for(lock, std::chrono::seconds(2), [] { return read_blocked; }),
            "next transfer blocks while a valid spool prefix exists");
    }
    int64_t cached = -99;
    bool queried = false;
    std::thread query([&] {
      cached = source->CachedBytes();
      { std::lock_guard<std::mutex> lock(blocked_mu); queried = true; }
      blocked_cv.notify_all();
    });
    {
      std::unique_lock<std::mutex> lock(blocked_mu);
      Check(blocked_cv.wait_for(lock, std::chrono::seconds(2), [&] { return queried; }),
            "cache snapshot returns before the blocked network read is released");
      release_read = true;
    }
    blocked_cv.notify_all();
    query.join();
    reader.join();
    Check(cached == 65536 - 32, "blocked read cannot hide or extend the committed cache prefix");
  }
  {
    auto source = Open("stream-large");
    const int calls = requests;
    const int64_t before = transport_bytes;
    Check(source->CachedBytes() == -1 && requests == calls && transport_bytes == before,
          "cache query before a read is unknown and does not start a request");
    CheckBytes(*source, 17, 32, "whole cache initial read preserves input bytes");
    const int64_t received = transport_bytes;
    Check(source->CachedBytes() == 65536 - 49 && transport_bytes == received,
          "whole cache counts committed prefix after the actual read without draining");
    CheckBytes(*source, 7, 2, "whole cache backward read preserves input bytes");
    Check(source->CachedBytes() == 65536 - 9 && transport_bytes == received,
          "backward read moves the cache origin without a new transfer");
    CheckBytes(*source, 65532, 8, "whole cache read crosses a stored-block boundary");
    Check(source->CachedBytes() == 131072 - 65540,
          "forward read counts only the newly committed prefix after its end");
    source->Cancel();
    Check(source->CachedBytes() == -1, "cancelled spool cannot report available cached bytes");
  }
  {
    auto source = Open("stream-unknown");
    CheckBytes(*source, 7, 32, "unknown whole cache records its first read");
    Check(source->CachedBytes() == 65536 - 39,
          "unknown content length still has an exact received cache prefix");
    Check(source->Size() == 2 * kFixtureChunk + 13,
          "explicit size query drains unknown response for cache fixture");
    const int64_t received = transport_bytes;
    Check(source->CachedBytes() == 2 * kFixtureChunk + 13 - 39 && transport_bytes == received,
          "size query extends cache without moving the last public read end");
    CheckBytes(*source, 2 * kFixtureChunk + 10, 3, "whole cache reads the final short tail");
    Check(source->CachedBytes() == 0, "fully consumed response has no following cache");
  }
  {
    auto source = Open("stream-error");
    std::vector<uint8_t> bytes(131072, 0xed);
    Check(source->ReadAt(0, bytes.data(), bytes.size()) == 65536,
          "network failure returns its complete verified prefix");
    Check(source->CachedBytes() == 0,
          "short successful prefix advances the public read end before network failure");
    CheckBytes(*source, 7, 32, "network failure preserves backward reads of verified bytes");
    Check(source->CachedBytes() == 65536 - 39,
          "cache never includes unreceived transport bytes after failure");
  }
  {
    auto source = Open("stream-large");
    CheckBytes(*source, 0, 32, "disk failure cache begins with a committed block");
    fail_write_after = 113;
    uint8_t byte = 99;
    Check(source->ReadAt(65536, &byte, 1) == -EIO && byte == 99,
          "failed next block does not produce a successful read");
    fail_write_after = -1;
    Check(source->CachedBytes() == 65536 - 32,
          "partial disk write remains outside the committed cache and read end");
  }
  {
    auto source = Open("range");
    CheckBytes(*source, 7, 32, "range cache records its public read end");
    Check(source->CachedBytes() == kFixtureChunk - 39,
          "range cache still counts the contiguous chunk suffix");
    CheckBytes(*source, 2 * kFixtureChunk, 1, "range fixture receives a disconnected chunk");
    CheckBytes(*source, 7, 32, "range cache returns to the first chunk");
    Check(source->CachedBytes() == kFixtureChunk - 39,
          "range cache excludes chunks beyond a missing range");
  }
  {
    auto source = Open("ignore");
    CheckBytes(*source, 7, 32, "partial range fixture starts with cached data");
    uint8_t bytes[4] = {};
    Check(source->ReadAt(kFixtureChunk - 2, bytes, sizeof(bytes)) == 2,
          "failed nonzero range returns the existing two-byte prefix");
    Check(source->CachedBytes() == 0,
          "range partial success moves the read end past the verified prefix");
  }
  Check(file_handles == 0 && handles == 0, "cache accounting retires every source resource");
}
}  // namespace

void RangeToWholeTransition(bool concurrent) {
  // Evict range zero using ordinary public reads, then make the transport
  // return a legal whole response when that range is requested again.
  {
    auto source = Open(concurrent ? "concurrent-transition" : "range-then-whole");
    uint8_t byte = 0xed;
    Check(source->ReadAt(0, &byte, 1) == 1 && byte == ByteAt(0),
          "initial 206 serves byte zero");
    for (int i = 1; i <= 17; ++i) {
      const int64_t at = i * kFixtureChunk;
      Check(source->ReadAt(at, &byte, 1) == 1 && byte == ByteAt(at),
            "ordinary range reads evict chunk zero");
    }
    switch_to_whole = true;
    if (concurrent) {
      ssize_t result1 = -99, result2 = -99;
      uint8_t first_byte = 0xed, second_byte = 0xed;
      std::thread first([&] {
        result1 = source->ReadAt(0, &first_byte, 1);
        ++transition_returned;
        transition_cv.notify_all();
      });
      std::thread second([&] {
        result2 = source->ReadAt(0, &second_byte, 1);
        ++transition_returned;
        transition_cv.notify_all();
      });
      {
        std::unique_lock<std::mutex> lock(transition_mu);
        Check(transition_cv.wait_for(lock, std::chrono::seconds(5), [] {
                return transition_returned.load() > 0;
              }), "first transition reader completes before second response");
      }
      Check(source->ReadAt(0, &byte, 1) == 1 && byte == ByteAt(0),
            "first adopted response establishes prefix");
      {
        std::lock_guard<std::mutex> lock(transition_mu);
        transition_release_second = true;
      }
      transition_cv.notify_all();
      first.join();
      second.join();
      Check(result1 == 1 && result2 == 1 && first_byte == ByteAt(0) && second_byte == ByteAt(0),
            "both concurrent transitions return requested bytes");
      byte = 0xed;
      Check(source->ReadAt(65536, &byte, 1) == 1 && byte == ByteAt(65536),
            "late response never restarts already-spooled prefix");
      Check(source->CachedBytes() == 131072 - 65537,
            "concurrent mode transition counts the one adopted spool after the final read");
    } else {
      byte = 0xed;
      Check(source->ReadAt(0, &byte, 1) == 1 && byte == ByteAt(0),
            "later legal 200 at zero must not report false EOF");
      Check(source->ReadAt(0, &byte, 1) == 1 && byte == ByteAt(0),
            "subsequent same read succeeds");
      Check(source->CachedBytes() == 65536 - 1,
            "range-to-whole transition replaces stale range accounting with the stored prefix");
    }
  }
  Check(handles == 0 && file_handles == 0, "transition releases every handle");
  switch_to_whole = false;
}

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "measure") == 0) {
    Measurements(false);
    return failures != 0;
  }
  RangeToWholeTransition(false);
  RangeToWholeTransition(true);
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
  StreamingContracts();
  CachedByteContracts();
  Measurements(true);
  Check(handles == 0 && file_handles == 0, "measured source leaves no owned resources");
  std::printf("HTTP source host contracts: %d checks, %d failures, %d requests\n",
              checks.load(), failures.load(), requests.load());
  return failures != 0;
}
