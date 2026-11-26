// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Separate actual converter/encoder translation units, complete callback bodies,
// an independent integer oracle, and real pinned libavc output.
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "compat/media/encode/encoder.h"

std::uint64_t coordinate_mappings = 0;
namespace {
size_t checks = 0;
bool efficiency_gate = true;
void Check(bool value, const char* what, int line) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "rgba_i420 check failed at %d: %s\n", line, what);
    std::abort();
  }
}
#define CHECK(value, what) Check((value), (what), __LINE__)
int64_t input_clock_ns = 1000000000;
uint64_t Hash(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint64_t hash = 1469598103934665603ULL;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
  return hash;
}
}  // namespace

// The callback's clock is explicit test input; benchmark clocks below are real.
namespace gesher {
int64_t MonotonicNanos() { return input_clock_ns; }
int AndroidClockGettime(int id, timespec* ts) {
  CHECK(id == CLOCK_MONOTONIC, "callback asks for the production monotonic clock");
  ts->tv_sec = input_clock_ns / 1000000000;
  ts->tv_nsec = input_clock_ns % 1000000000;
  return 0;
}
}  // namespace gesher

// Unused backend selection/metadata references from the complete converter TU
// fail closed on every platform, including COFF linkers without dead stripping.
namespace gesher::media {
bool Format::GetBuffer(const char*, std::vector<uint8_t>*) const { std::abort(); }
std::string Format::Mime() const { std::abort(); }
std::vector<VideoCodecInfo> D3D11VideoCodecs() { std::abort(); }
std::vector<VideoCodecInfo> MfVideoCodecs() { std::abort(); }
std::vector<VideoCodecInfo> SoftVideoCodecs() { std::abort(); }
std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateMfVideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format&, std::string*) { std::abort(); }
}  // namespace gesher::media
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t) { std::abort(); }

using gesher::media::VideoFrame;
using gesher::media::EncodedPacket;
using gesher::media::VideoEncoderParams;
using gesher::media::RgbaToI420;

namespace {
// Add the chroma offset before division, so all numerators are positive.
// This expresses the existing floor arithmetic without a signed right shift,
// and reads each top-left chroma sample rather than averaging it.
std::array<uint8_t, 3> Pixel(uint32_t p) {
  const int red = p % 256, green = (p / 256) % 256, blue = (p / 65536) % 256;
  return {static_cast<uint8_t>(std::clamp((66 * red + 129 * green + 25 * blue + 128) / 256 + 16, 0, 255)),
          static_cast<uint8_t>(std::clamp((-38 * red - 74 * green + 112 * blue + 128 + 32768) / 256, 0, 255)),
          static_cast<uint8_t>(std::clamp((112 * red - 94 * green - 18 * blue + 128 + 32768) / 256, 0, 255))};
}

void Reference(const uint32_t* input, int sw, int sh, VideoFrame* frame) {
  const int width = frame->width, height = frame->height;
  if (width <= 0 || height <= 0 || sw <= 0 || sh <= 0) return;
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const size_t source = size_t(int64_t(row) * sh / height) * sw + size_t(int64_t(col) * sw / width);
      frame->data[size_t(row) * frame->stride_y + col] = Pixel(input[source])[0];
    }
  }
  const size_t u = size_t(frame->stride_y) * height;
  const size_t v = u + size_t(frame->stride_uv) * ((height + 1) / 2);
  for (int row = 0; row < (height + 1) / 2; ++row) {
    for (int col = 0; col < (width + 1) / 2; ++col) {
      // 2*col <= width-1 and 2*row <= height-1 also hold for odd sizes.
      const size_t source = size_t(int64_t(2 * row) * sh / height) * sw + size_t(int64_t(2 * col) * sw / width);
      const auto rgb = Pixel(input[source]);
      frame->data[u + size_t(row) * frame->stride_uv + col] = rgb[1];
      frame->data[v + size_t(row) * frame->stride_uv + col] = rgb[2];
    }
  }
}
}  // namespace

#include "rgba_i420_callbacks.h"

namespace {
uint32_t Pattern(int x, int y, int frame) {
  const uint32_t r = (x * 13 + y * 7 + frame * 3) & 255;
  const uint32_t g = (x * 3 + y * 29 + frame * 5) & 255;
  const uint32_t b = (x * 17 + y * 11 + frame * 13) & 255;
  return r | (g << 8) | (b << 16) | (uint32_t((x + y + frame) & 255) << 24);
}

auto Metadata(const VideoFrame& f) {
  return std::tuple(f.width, f.height, f.stride_y, f.stride_uv,
                    f.crop_left, f.crop_top, f.crop_right, f.crop_bottom, f.time_us);
}

void GeometryCase(int sw, int sh, int w, int h, int padding) {
  std::vector<uint32_t> source(size_t(sw) * sh + 16, 0x13579bdf);
  for (int y = 0; y < sh; ++y)
    for (int x = 0; x < sw; ++x) source[8 + size_t(y) * sw + x] = Pattern(x, y, 9);
  const auto saved = source;
  VideoFrame actual;
  actual.width = w; actual.height = h;
  actual.stride_y = w + padding; actual.stride_uv = (w + 1) / 2 + padding;
  actual.crop_left = 1; actual.crop_top = 1; actual.crop_right = w - 2; actual.crop_bottom = h - 2;
  actual.time_us = 9000000001;
  actual.data.assign(size_t(actual.stride_y) * h + 2 * size_t(actual.stride_uv) * ((h + 1) / 2) + 32, 0xa5);
  VideoFrame expected = actual;
  const auto metadata = Metadata(actual);
  const auto mappings = coordinate_mappings;
  RgbaToI420(source.data() + 8, sw, sh, &actual);
  Reference(source.data() + 8, sw, sh, &expected);
  CHECK(actual.data == expected.data, "complete output and padding oracle");
  CHECK(source == saved, "input pixels and guard words remain untouched");
  CHECK(Metadata(actual) == metadata, "conversion preserves all frame metadata");
#ifdef GESHER_COUNT_COORDINATES
  const uint64_t count = uint64_t(w) * h + uint64_t((w + 1) / 2) * ((h + 1) / 2);
  if (efficiency_gate || sw != w || sh != h)
    CHECK(coordinate_mappings - mappings == (sw == w && sh == h ? 0 : count),
          sw == w && sh == h ? "same-size mapping budget" : "scaled mapping count is unchanged");
#else
  (void)mappings;
#endif
}

void Geometry(bool negative) {
  // Mutants stop here on a padded destination, never by corrupting memory.
  GeometryCase(17, 5, 17, 5, 7);
  GeometryCase(17, 9, 17, 5, 7);
  if (negative) return;
  for (int w : {1, 2, 3, 7, 8, 17, 31, 63, 127}) {
    for (int h : {1, 2, 3, 5, 17}) {
      for (int pad : {0, 1, 13}) {
        GeometryCase(w, h, w, h, pad);
        GeometryCase(w + 3, h + 2, w, h, pad);
        GeometryCase(std::max(1, w / 2), std::max(1, h / 2), w, h, pad);
      }
    }
  }
  for (auto dims : {std::array{0, 7, 3, 5}, std::array{7, 0, 3, 5},
                    std::array{-1, 7, 3, 5}, std::array{7, -1, 3, 5},
                    std::array{7, 9, 0, 5}, std::array{7, 9, 3, 0}}) {
    VideoFrame f; f.width = dims[2]; f.height = dims[3]; f.data.assign(32, 0x9d);
    const auto before = f.data;
    RgbaToI420(nullptr, dims[0], dims[1], &f);
    CHECK(f.data == before, "invalid dimensions keep the original no-op behavior");
  }
  for (int offset : {0, 4, 16, 64}) {
    VideoFrame a; a.Allocate(17, 9); a.data.resize(17 * 9 * 4 + offset + 32);
    for (size_t i = 0; i < a.data.size(); ++i) a.data[i] = uint8_t(i * 37 + 19);
    VideoFrame b = a;
    std::vector<uint32_t> words(17 * 9);
    for (size_t i = 0; i < words.size(); ++i) words[i] = Pattern(int(i % 17), int(i / 17), 2);
    // Establish implicit-lifetime uint32_t inputs inside aligned byte storage.
    std::memcpy(a.data.data() + offset, words.data(), words.size() * sizeof(uint32_t));
    std::memcpy(b.data.data() + offset, words.data(), words.size() * sizeof(uint32_t));
    RgbaToI420(reinterpret_cast<const uint32_t*>(a.data.data() + offset), 17, 9, &a);
    Reference(reinterpret_cast<const uint32_t*>(b.data.data() + offset), 17, 9, &b);
    CHECK(a.data == b.data, "overlapping storage preserves the original plane/pass order");
  }
}

void AllRgb() {
  constexpr int width = 512, height = 2;
  std::vector<uint32_t> input(width * height);
  VideoFrame actual; actual.Allocate(width, height);
  VideoFrame expected = actual;
  for (uint32_t base = 0; base < (1u << 24); base += width / 2) {
    for (int x = 0; x < width / 2; ++x) {
      const uint32_t rgb = base + uint32_t(x);
      input[2 * x] = rgb | (((base >> 8) ^ uint32_t(x)) << 24);
      input[2 * x + 1] = rgb ^ 0x81ffffffu;
      input[width + 2 * x] = rgb ^ 0x42ffffffu;
      input[width + 2 * x + 1] = rgb ^ 0xff000000u;
    }
    RgbaToI420(input.data(), width, height, &actual);
    Reference(input.data(), width, height, &expected);
    CHECK(actual.data == expected.data, "every RGB triple, alpha and top-left chroma matches the independent oracle");
  }
  std::puts("RGBA_ALL_RGB_PASS triples=16777216");
}

void Admissions() {
  std::vector<uint32_t> input(17 * 9);
  for (size_t i = 0; i < input.size(); ++i) input[i] = Pattern(int(i % 17), int(i / 17), 0);
  rgba_fixture::CodecInput<RgbaToI420> c; c.enc_params_.width = 17; c.enc_params_.height = 9;
  c.started_ = false; c.OnSurfaceFrame(input.data(), 17, 9, 1234000);
  CHECK(c.surface_frames_.empty(), "codec pre-start rejection remains");
  c.started_ = true; c.surface_eos_ = true; c.OnSurfaceFrame(input.data(), 17, 9, 1234000);
  CHECK(c.surface_frames_.empty(), "codec EOS rejection remains");
  c.surface_eos_ = false;
  for (int i = 0; i < 12; ++i) c.OnSurfaceFrame(input.data(), 17, 9, 1000000 + i * 1000);
  CHECK(c.surface_frames_.size() == 8 && c.surface_frames_.front()->time_us == 1004,
        "codec keeps the same bounded queue and oldest-drop policy");
  c.surface_frames_.clear(); input_clock_ns = 4321099000;
  c.OnSurfaceFrame(input.data(), 17, 9, 0);
  CHECK(c.surface_frames_.front()->time_us == 4321099, "codec fallback timestamp remains monotonic input");

  rgba_fixture::RecorderInput<RgbaToI420> r; r.video_params_ = c.enc_params_;
  r.running_ = false; r.OnFrame(input.data(), 17, 9, input_clock_ns);
  r.running_ = true; r.paused_ = true; r.OnFrame(input.data(), 17, 9, input_clock_ns);
  CHECK(r.frames_.empty() && r.video_rejected_ == 2, "recorder stopped/paused rejection remains");
  r.paused_ = false; r.start_us_ = 5000000;
  r.OnFrame(input.data(), 17, 9, input_clock_ns);
  r.start_us_ = 1000000; r.resume_at_us_ = 4500000;
  r.OnFrame(input.data(), 17, 9, input_clock_ns);
  CHECK(r.frames_.empty() && r.video_rejected_ == 4, "recorder pre-start/pre-resume timestamps remain rejected");
  r.resume_at_us_ = 0;
  for (int i = 0; i < 12; ++i) r.OnFrame(input.data(), 17, 9, input_clock_ns + i * 1000);
  CHECK(r.frames_.size() == 8 && r.video_queue_drops_ == 4, "recorder bounded queue and drop counter remain");
  const auto previous_pts = r.last_video_us_;
  r.frames_.clear();
  r.OnFrame(input.data(), 17, 9, input_clock_ns - 1000);
  CHECK(r.frames_.front()->time_us == previous_pts + 1, "recorder preserves increasing presentation timestamps");
}

uint64_t CpuNanos() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  CHECK(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "read process CPU time");
  ULARGE_INTEGER k, u; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
  return (k.QuadPart + u.QuadPart) * 100;
#else
  timespec ts{}; CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0, "read process CPU time");
  return uint64_t(ts.tv_sec) * 1000000000 + uint64_t(ts.tv_nsec);
#endif
}
uint64_t WallNanos() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Case { const char* name; int sw, sh, w, h, count; bool recorder; };
struct Pipeline {
  std::vector<EncodedPacket> packets;
  std::vector<std::shared_ptr<VideoFrame>> retained;
  std::vector<uint64_t> hashes;
  uint64_t cpu_ns = 0, wall_ns = 0, mappings = 0;
};

template <bool IsRecorder, rgba_fixture::Converter Convert>
Pipeline Encode(const Case& c, const std::vector<std::vector<uint32_t>>& inputs, bool measure) {
  using Path = std::conditional_t<IsRecorder, rgba_fixture::RecorderInput<Convert>, rgba_fixture::CodecInput<Convert>>;
  Path path;
  VideoEncoderParams params; params.width = c.w; params.height = c.h; params.bitrate = 3000000;
  if constexpr (IsRecorder) path.video_params_ = params; else path.enc_params_ = params;
  Pipeline result; std::string error;
  const uint64_t mapped = coordinate_mappings, cpu = CpuNanos(), wall = WallNanos();
  auto encoder = gesher::media::CreateSoftAvcEncoder(params, &error);
  CHECK(encoder != nullptr, error.c_str());
  for (int i = 0; i < c.count; ++i) {
    input_clock_ns = (1000000 + 791 + int64_t(i) * 33333) * 1000;
    std::shared_ptr<VideoFrame> frame;
    if constexpr (IsRecorder) {
      path.OnFrame(inputs[i].data(), c.sw, c.sh, input_clock_ns);
      CHECK(path.frames_.size() == 1, "real recorder callback queued exactly one frame");
      frame = path.frames_.front(); path.frames_.pop_front();
    } else {
      path.OnSurfaceFrame(inputs[i].data(), c.sw, c.sh, input_clock_ns);
      CHECK(path.surface_frames_.size() == 1, "real codec callback queued exactly one frame");
      frame = path.surface_frames_.front(); path.surface_frames_.pop_front();
    }
    const int64_t pts = (IsRecorder ? 0 : 1000000) + 791 + int64_t(i) * 33333;
    CHECK(frame->time_us == pts, "actual callback preserves its timestamp contract");
    if (!measure) {
      VideoFrame expected; expected.Allocate(c.w, c.h);
      Reference(inputs[i].data(), c.sw, c.sh, &expected);
      CHECK(frame->data == expected.data, "real callback's complete frame matches independent oracle");
      result.hashes.push_back(Hash(frame->data.data(), frame->data.size()));
      result.retained.push_back(frame);
    }
    std::vector<EncodedPacket> output;
    CHECK(encoder->Encode(*frame, i == 0 || i == c.count / 2, &output), "actual AVC provider encoded frame");
    for (auto& packet : output) result.packets.push_back(std::move(packet));
  }
  std::vector<EncodedPacket> tail;
  CHECK(encoder->Drain(&tail), "actual AVC provider drained");
  for (auto& packet : tail) result.packets.push_back(std::move(packet));
  encoder.reset();
  result.wall_ns = WallNanos() - wall; result.cpu_ns = CpuNanos() - cpu;
  result.mappings = coordinate_mappings - mapped;
  for (size_t i = 0; i < result.retained.size(); ++i)
    CHECK(Hash(result.retained[i]->data.data(), result.retained[i]->data.size()) == result.hashes[i],
          "consumer frames remain owned after later encoding, drain and destroy");
  return result;
}

void Write(const std::string& path, const std::vector<EncodedPacket>& packets) {
  FILE* f = std::fopen(path.c_str(), "wb"); CHECK(f != nullptr, "open encoded fixture");
  for (const auto& p : packets)
    CHECK(std::fwrite(p.data.data(), 1, p.data.size(), f) == p.data.size(), "write complete actual bitstream");
  CHECK(std::fclose(f) == 0, "close encoded fixture");
}

void Stream(const Case& c, bool measure, const std::string& directory) {
  std::vector<std::vector<uint32_t>> input(c.count, std::vector<uint32_t>(size_t(c.sw) * c.sh));
  std::vector<uint64_t> input_hashes;
  for (int frame = 0; frame < c.count; ++frame) {
    for (int y = 0; y < c.sh; ++y)
      for (int x = 0; x < c.sw; ++x)
        input[frame][size_t(y) * c.sw + x] = Pattern(x / 4, y / 4, frame);
    input_hashes.push_back(Hash(input[frame].data(), input[frame].size() * sizeof(uint32_t)));
  }
  Pipeline reference, actual;
  if (c.recorder) {
    actual = Encode<true, RgbaToI420>(c, input, measure);
    reference = Encode<true, Reference>(c, input, measure);
  } else {
    actual = Encode<false, RgbaToI420>(c, input, measure);
    reference = Encode<false, Reference>(c, input, measure);
  }
  CHECK(actual.packets.size() == reference.packets.size(), "complete encoded packet count matches reference conversion");
  int frames = 0;
  for (size_t i = 0; i < actual.packets.size(); ++i) {
    const auto& a = actual.packets[i]; const auto& b = reference.packets[i];
    CHECK(a.data == b.data && a.time_us == b.time_us && a.config == b.config && a.key == b.key,
          "complete bitstream and packet metadata match reference conversion");
    if (!a.config) {
      CHECK(a.time_us == (c.recorder ? 0 : 1000000) + 791 + int64_t(frames) * 33333,
            "real encoded timestamp matches accepted input");
      if (frames == 0 || frames == c.count / 2) CHECK(a.key, "requested key frame is produced");
      ++frames;
    }
  }
  CHECK(frames == c.count, "actual encoder produced every accepted frame");
  for (int i = 0; i < c.count; ++i)
    CHECK(Hash(input[i].data(), input[i].size() * sizeof(uint32_t)) == input_hashes[i], "real encoding leaves original RGBA input intact");
#ifdef GESHER_COUNT_COORDINATES
  const uint64_t expected = c.sw == c.w && c.sh == c.h ? 0 :
      uint64_t(c.count) * (uint64_t(c.w) * c.h + uint64_t((c.w + 1) / 2) * ((c.h + 1) / 2));
  if (efficiency_gate || c.sw != c.w || c.sh != c.h)
    CHECK(actual.mappings == expected, "callback whole-stream coordinate mapping budget");
#endif
  if (!directory.empty()) {
    Write(directory + "/" + c.name + ".actual.h264", actual.packets);
    Write(directory + "/" + c.name + ".reference.h264", reference.packets);
  }
  std::printf("STREAM %s frames=%d mappings=%llu cpu_ns=%llu wall_ns=%llu\n", c.name, frames,
      static_cast<unsigned long long>(actual.mappings), static_cast<unsigned long long>(actual.cpu_ns),
      static_cast<unsigned long long>(actual.wall_ns));
}

void ConverterTiming(int w, int h, int count) {
  std::vector<uint32_t> input(size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) input[size_t(y) * w + x] = Pattern(x, y, 7);
  VideoFrame f; f.Allocate(w, h);
  RgbaToI420(input.data(), w, h, &f);
  const auto cpu = CpuNanos(), wall = WallNanos();
  for (int i = 0; i < count; ++i) RgbaToI420(input.data(), w, h, &f);
  const auto wall_ns = WallNanos() - wall, cpu_ns = CpuNanos() - cpu;
  VideoFrame expected; expected.Allocate(w, h); Reference(input.data(), w, h, &expected);
  CHECK(f.data == expected.data, "timed converter output matches independent oracle");
  std::printf("CONVERT %dx%d frames=%d cpu_ns=%llu wall_ns=%llu digest=%llu\n", w, h, count,
      static_cast<unsigned long long>(cpu_ns), static_cast<unsigned long long>(wall_ns),
      static_cast<unsigned long long>(Hash(f.data.data(), f.data.size())));
}
}  // namespace

int main(int argc, char** argv) {
  const bool measure = argc > 1 && std::strcmp(argv[1], "measure") == 0;
  const bool negative = argc > 1 && std::strcmp(argv[1], "negative") == 0;
  efficiency_gate = !(argc > 1 && std::strcmp(argv[1], "reference") == 0);
  const std::string directory = argc > 2 ? argv[2] : "";
  if (!measure) {
    Geometry(negative);
    if (negative) return 0;
    AllRgb(); Admissions();
  } else {
    ConverterTiming(1280, 720, 30);
    ConverterTiming(1920, 1080, 20);
    ConverterTiming(3840, 2160, 8);
  }
  const Case cases[] = {
      {"codec-small-12", 320, 240, 320, 240, 12, false},
      {"recorder-scaled-12", 641, 479, 320, 240, 12, true},
      {"codec-720p-24", 1280, 720, 1280, 720, 24, false},
      {"recorder-720p-24", 1280, 720, 1280, 720, 24, true},
      {"codec-1080p-8", 1920, 1080, 1920, 1080, 8, false},
      {"recorder-1080p-8", 1920, 1080, 1920, 1080, 8, true}};
  for (const auto& c : cases) Stream(c, measure, directory);
  std::printf("rgba_i420_test: %zu checks; complete pixels, actual callbacks and real AVC packets passed\n", checks);
  return 0;
}
