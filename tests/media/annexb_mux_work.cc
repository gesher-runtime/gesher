// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete actual MP4 factory/writer/finalization, with ordinary files and
// allocators. Format's metadata storage is the declared host-only boundary;
// input packetization and finalized-file decoding are independently checked.
#include "compat/media/mux/muxer.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

struct AMediaFormat {
  std::string mime;
  std::map<std::string, int32_t> integers;
  std::map<std::string, std::vector<uint8_t>> buffers;
};
namespace gesher::media {
Format::Format() : f_(new AMediaFormat) {}
Format::~Format() { delete f_; }
std::string Format::Mime() const { return f_->mime; }
bool Format::GetInt32(const char* key, int32_t* out) const {
  auto it = f_->integers.find(key);
  if (it == f_->integers.end()) return false;
  *out = it->second;
  return true;
}
bool Format::GetBuffer(const char* key, std::vector<uint8_t>* out) const {
  auto it = f_->buffers.find(key);
  if (it == f_->buffers.end()) return false;
  *out = it->second;
  return true;
}
// The genuine MP4 factory is linked unchanged. Its unselected WebM branch
// fails closed at this explicit host boundary; it never manufactures output.
std::unique_ptr<Muxer> CreateWebmMuxer(int fd, std::string* error) {
  close(fd);
  *error = "WebM is outside the Annex-B MP4 host fixture";
  return nullptr;
}
}  // namespace gesher::media

namespace {
using Bytes = std::vector<uint8_t>;
using gesher::media::Format;
using gesher::media::Muxer;
using gesher::media::MuxerFormat;

void Check(bool success, const char* label) {
  if (!success) {
    std::fprintf(stderr, "FAIL Annex-B MP4: %s (errno=%d)\n", label, errno);
    std::exit(1);
  }
}

Bytes Read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  Check(static_cast<bool>(input), "open complete input/output");
  Bytes bytes{std::istreambuf_iterator<char>(input), {}};
  Check(input.eof() || !input.fail(), "read complete input/output");
  return bytes;
}

uint32_t U32(const Bytes& bytes, size_t* at) {
  Check(*at <= bytes.size() && bytes.size() - *at >= 4, "bounded fixture integer");
  uint32_t result = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) result |= uint32_t{bytes[(*at)++]} << shift;
  return result;
}

struct Packet {
  Bytes data;
  int64_t timestamp;
  int flags;
};
struct Input {
  int32_t width{}, height{};
  size_t bytes{};
  std::vector<Packet> packets;
};

Input Load(const std::string& path) {
  auto bytes = Read(path);
  size_t at = 0;
  Check(U32(bytes, &at) == 0x31425641, "packet fixture AVB1 identity");
  Input input;
  input.width = static_cast<int32_t>(U32(bytes, &at));
  input.height = static_cast<int32_t>(U32(bytes, &at));
  const uint32_t count = U32(bytes, &at);
  Check(input.width > 0 && input.height > 0 && count > 0, "fixture dimensions and nonempty packet list");
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t size = U32(bytes, &at), flags = U32(bytes, &at);
    const uint32_t low = U32(bytes, &at), high = U32(bytes, &at);
    Check(size <= bytes.size() - at, "bounded complete compressed packet");
    input.packets.push_back({Bytes(bytes.begin() + at, bytes.begin() + at + size),
        static_cast<int64_t>(uint64_t{low} | (uint64_t{high} << 32)), static_cast<int>(flags)});
    input.bytes += size;
    at += size;
  }
  Check(at == bytes.size(), "no ignored fixture suffix");
  return input;
}

uint64_t CpuNs() {
#ifdef _WIN32
  FILETIME creation{}, exit{}, kernel{}, user{};
  Check(GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0, "actual process CPU clock");
  const uint64_t k = (uint64_t{kernel.dwHighDateTime} << 32) | kernel.dwLowDateTime;
  const uint64_t u = (uint64_t{user.dwHighDateTime} << 32) | user.dwLowDateTime;
  return (k + u) * 100;
#else
  timespec value{};
  Check(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "actual process CPU clock");
  return static_cast<uint64_t>(value.tv_sec) * 1000000000 + value.tv_nsec;
#endif
}

uint64_t WallNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t Checksum(const Bytes& bytes) {
  uint64_t result = 1469598103934665603ULL;
  for (uint8_t byte : bytes) result = (result ^ byte) * 1099511628211ULL;
  return result;
}

size_t WriteMovie(const Input& input, const std::vector<Bytes>& config,
                  const std::string& path, bool config_sample) {
  int flags = O_CREAT | O_TRUNC | O_RDWR;
#ifdef _WIN32
  flags |= O_BINARY;
#endif
  const int fd = open(path.c_str(), flags, 0600);
  Check(fd >= 0, "create ordinary output file");
  size_t output = 0;
  {
    std::string error;
    auto mux = Muxer::Create(fd, MuxerFormat::kMpeg4, &error);
    Check(mux != nullptr, "actual public MP4 factory and descriptor duplication");
    Format format;
    format.get()->mime = "video/avc";
    format.get()->integers = {{"width", input.width}, {"height", input.height}};
    if (!config_sample) {
      for (size_t i = 0; i < config.size(); ++i)
        format.get()->buffers["csd-" + std::to_string(i)] = config[i];
    }
    const int track = mux->AddTrack(format, &error);
    Check(track == 0, "actual AVC track admission");
    Check(mux->Start(&error), "actual ftyp and mdat creation");
    if (config_sample) {
      Bytes combined;
      for (const auto& nal : config) {
        combined.insert(combined.end(), {0, 0, 0, 1});
        combined.insert(combined.end(), nal.begin(), nal.end());
      }
      Check(mux->WriteSample(track, combined.data(), combined.size(), 0, 2, &error), "actual codec-config sample");
    }
    for (const auto& packet : input.packets)
      Check(mux->WriteSample(track, packet.data.data(), packet.data.size(), packet.timestamp,
          packet.flags, &error), "actual complete compressed sample write");
    Check(mux->Stop(&error), "actual mdat backpatch, sample tables, moov and owned-descriptor close");
    struct stat info{};
    Check(fstat(fd, &info) == 0 && info.st_size > 0 && mux->BytesWritten() == info.st_size,
        "final file size and caller descriptor retained");
    output = static_cast<size_t>(info.st_size);
  }
  Check(close(fd) == 0, "caller descriptor and all writer/format ownership retired");
  return output;
}

void Measure(const char* phase, unsigned sample, size_t repetitions, const Input& input,
    const std::vector<Bytes>& config, const std::string& path, const Bytes& expected, bool config_sample) {
  uint64_t cpu = 0, wall = 0, checksum = 0;
  size_t size = 0;
  for (size_t i = 0; i < repetitions; ++i) {
    const auto start_wall = WallNs(), start_cpu = CpuNs();
    size = WriteMovie(input, config, path, config_sample);
    cpu += CpuNs() - start_cpu;
    wall += WallNs() - start_wall;
    // Complete file verification is outside every measured lifecycle. No
    // production output or source sample is discarded, padded or substituted.
    auto actual = Read(path);
    Check(actual.size() == size && (expected.empty() || actual == expected), "every complete output byte equals reference");
    checksum = Checksum(actual);
  }
  std::printf("{\"phase\":\"%s\",\"sample\":%u,\"lifecycles\":%zu,\"packets_per_file\":%zu,"
      "\"input_bytes_per_file\":%zu,\"output_bytes_per_file\":%zu,\"cpu_ns\":%llu,\"wall_ns\":%llu,"
      "\"checksum\":%llu}\n", phase, sample, repetitions, input.packets.size(), input.bytes, size,
      static_cast<unsigned long long>(cpu), static_cast<unsigned long long>(wall),
      static_cast<unsigned long long>(checksum));
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 9, "arguments: packets csd output expected-or-dash repetitions samples format-or-config prepare-or-verify");
  const Input input = Load(argv[1]);
  const auto config_input = Load(argv[2]);
  std::vector<Bytes> config;
  for (const auto& packet : config_input.packets) config.push_back(packet.data);
  Check(config.size() == 2, "actual SPS and PPS configuration");
  const size_t repetitions = std::stoull(argv[5]);
  const unsigned samples = static_cast<unsigned>(std::stoul(argv[6]));
  Check(repetitions > 0 && repetitions <= 10000 && samples <= 100, "bounded workload configuration");
  Check(std::string(argv[7]) == "format" || std::string(argv[7]) == "config", "declared codec configuration path");
  const bool config_sample = std::string(argv[7]) == "config";
  const bool prepare = std::string(argv[8]) == "prepare";
  Check(prepare || std::string(argv[8]) == "verify", "explicit reference preparation or verification");
  Bytes expected;
  if (!prepare) {
    expected = Read(argv[4]);
    Check(!expected.empty(), "mandatory complete finalized reference file");
  }
  Measure("first", 0, 1, input, config, argv[3], expected, config_sample);
  for (unsigned sample = 0; sample < samples; ++sample)
    Measure("steady", sample, repetitions, input, config, argv[3], expected, config_sample);
  if (!prepare) Check(unlink(argv[3]) == 0, "ordinary temporary output retired");
}
