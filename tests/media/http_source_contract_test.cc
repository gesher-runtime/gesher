// Complete production HttpSource and real WinHTTP, against the owned loopback peer.
#include "compat/media/player/data_source.h"
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int64_t kChunk = 256 * 1024, kLength = 24 * kChunk + 13;
std::atomic<int> checks{0}, failures{0};
bool Check(bool ok, const char* reason) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL Windows HTTP source: %s\n", reason); }
  return ok;
}
uint8_t ByteAt(int64_t at) { return static_cast<uint8_t>(at % 251); }
std::unique_ptr<gesher::media::DataSource> Open(const std::string& base, const char* endpoint) {
  std::string error;
  auto source = gesher::media::OpenHttpSource(base + endpoint, {}, &error);
  if (!Check(source != nullptr, "open loopback source")) std::fprintf(stderr, "%s\n", error.c_str());
  return source;
}
}

int main(int argc, char** argv) {
  if (argc != 2) { std::fprintf(stderr, "Pass the owned HTTP fixture's base URL\n"); return 2; }
  const std::string base = argv[1];
  {
    auto source = Open(base, "/range");
    if (!source) return 1;
    Check(source->Size() == kLength, "real 206 Content-Range length");
    uint8_t bytes[32] = {};
    Check(source->ReadAt(kChunk + 7, bytes, sizeof(bytes)) == sizeof(bytes), "real nonzero range read");
    bool exact = true;
    for (size_t i = 0; i < sizeof(bytes); ++i) exact &= bytes[i] == ByteAt(kChunk + 7 + i);
    Check(exact, "range payload offset matches actual response");
    Check(source->ReadAt(kLength - 3, bytes, sizeof(bytes)) == 3, "final range clamps to resource EOF");
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) readers.emplace_back([&, t] {
      for (int j = 0; j < 12; ++j) {
        const int64_t at = ((t * 7 + j) % 24) * kChunk + 5;
        uint8_t data[16] = {};
        bool valid = source->ReadAt(at, data, sizeof(data)) == sizeof(data);
        for (size_t i = 0; i < sizeof(data); ++i) valid &= data[i] == ByteAt(at + i);
        Check(valid, "concurrent WinHTTP reads and LRU eviction preserve bytes");
      }
    });
    for (auto& reader : readers) reader.join();
  }
  {
    auto source = Open(base, "/whole");
    if (!source) return 1;
    char bytes[32] = {};
    Check(source->ReadAt(6, bytes, std::numeric_limits<size_t>::max()) == 11
              && std::memcmp(bytes, "file-source", 11) == 0, "initial 200 whole resource remains seekable");
    Check(source->Size() == 17, "whole resource has exact size");
  }
  {
    auto source = Open(base, "/ignore");
    if (!source) return 1;
    uint8_t bytes[32] = {};
    Check(source->ReadAt(kChunk + 7, bytes, sizeof(bytes)) == -EIO,
          "nonzero range ignores full 200 body");
    Check(source->ReadAt(kChunk - 2, bytes, 4) == 2 && bytes[0] == ByteAt(kChunk - 2)
              && bytes[1] == ByteAt(kChunk - 1), "later request failure preserves earlier bytes");
  }
  {
    auto source = Open(base, "/unknown");
    if (!source) return 1;
    uint8_t byte = 0;
    Check(source->Size() == -1, "unknown HTTP size");
    Check(source->ReadAt(kLength + kChunk, &byte, 1) == 0, "416 ends unknown resource");
    Check(source->ReadAt(INT64_MAX, &byte, 1) == 0, "highest range is representable");
  }
  // The peer withholds its tail until a separate request releases it. This
  // proves the first useful read does not require a complete response, without
  // depending on an elapsed-time performance threshold.
  {
    auto source = Open(base, "/stream-gated");
    if (!source) return 1;
    Check(source->Size() == 2 * 1024 * 1024 + 13, "known length is available before gated tail");
    uint8_t bytes[32] = {};
    const ssize_t initial = source->ReadAt(17, bytes, sizeof(bytes));
    bool exact = initial == sizeof(bytes);
    for (size_t i = 0; i < sizeof(bytes); ++i) exact &= bytes[i] == ByteAt(17 + i);
    Check(exact, "initial whole-body bytes arrive before peer releases its tail");
    {
      auto release = Open(base, "/release-gated");
      uint8_t released = 0;
      Check(release && release->ReadAt(0, &released, 1) == 1 && released == '1',
            "explicitly release owned loopback peer");
    }
    Check(source->ReadAt(2 * 1024 * 1024 + 10, bytes, sizeof(bytes)) == 3,
          "file-backed whole response supports forward seek to final bytes");
    Check(bytes[0] == ByteAt(2 * 1024 * 1024 + 10) && bytes[2] == ByteAt(2 * 1024 * 1024 + 12),
          "final whole-body bytes are exact");
    Check(source->ReadAt(7, bytes, 2) == 2 && bytes[0] == ByteAt(7) && bytes[1] == ByteAt(8),
          "file-backed whole response supports backward seek");
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) readers.emplace_back([&, t] {
      for (int j = 0; j < 15; ++j) {
        const int64_t at = (t * 9901 + j * 32771) % (2 * 1024 * 1024 - 100);
        uint8_t read[79];
        bool valid = source->ReadAt(at, read, sizeof(read)) == sizeof(read);
        for (size_t i = 0; i < sizeof(read); ++i) valid &= read[i] == ByteAt(at + i);
        Check(valid, "concurrent real spool reads preserve position and bytes");
      }
    });
    for (auto& reader : readers) reader.join();
    source->Cancel();
    Check(source->ReadAt(0, bytes, 1) == -EINTR, "whole source cancellation rejects cached reads");
  }
  {
    auto source = Open(base, "/stream-unknown");
    if (!source) return 1;
    uint8_t bytes[32] = {};
    Check(source->ReadAt(7, bytes, sizeof(bytes)) == sizeof(bytes), "unknown 200 early read");
    Check(source->Size() == 2 * 1024 * 1024 + 13, "unknown 200 size drains spool to real EOF");
    Check(source->ReadAt(INT64_MAX, bytes, 1) == 0, "unknown 200 maximum offset becomes EOF");
  }
  {
    auto source = Open(base, "/stream-empty");
    if (!source) return 1;
    uint8_t byte = 99;
    Check(source->ReadAt(0, nullptr, 0) == 0 && source->Size() == 0 &&
              source->ReadAt(0, &byte, 1) == 0 && byte == 99, "empty whole source and zero read");
  }
  {
    auto source = Open(base, "/stream-truncated");
    if (!source) return 1;
    uint8_t bytes[2000];
    std::memset(bytes, 0xed, sizeof(bytes));
    const ssize_t count = source->ReadAt(0, bytes, sizeof(bytes));
    Check(count == -EIO || count == 1000, "truncated WinHTTP body reports error or verified prefix only");
    bool guarded = true;
    for (size_t i = count > 0 ? static_cast<size_t>(count) : 0; i < sizeof(bytes); ++i)
      guarded &= bytes[i] == 0xed;
    Check(guarded, "truncated WinHTTP body never fabricates missing bytes");
    Check(source->ReadAt(1000, bytes, 1) == -EIO, "truncated body remains an error beyond stored prefix");
  }
  std::printf("Windows HTTP source contracts: %d checks, %d failures\n", checks.load(), failures.load());
  return failures != 0;
}
