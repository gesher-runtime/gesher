// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete product decoder, real pinned libavc and independent FFmpeg fixtures.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include "ih264_typedefs.h"
#include "ih264d.h"
#include "compat/media/video/video_decoder.h"

namespace {
size_t checks = 0;
void Check(bool result, const char* condition, int line) {
  ++checks;
  if (!result) {
    std::fprintf(stderr, "AVC output check failed at %d: %s\n", line, condition);
    std::abort();
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)
uint64_t Hash(const void* bytes, size_t size) {
  uint64_t value = 1469598103934665603ULL;
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 1099511628211ULL;
  return value;
}
// Only C++ allocations inside product calls are tracked, including later frees.
// Provider C allocations and test metadata/input storage are excluded.
struct Allocation { void* pointer = nullptr; size_t bytes = 0; };
std::array<Allocation, 256> raw_allocations{};
bool track_allocations = false;
size_t raw_threshold = 0, raw_live = 0, raw_peak = 0, raw_count = 0, copied_bytes = 0;
struct Observer {
  size_t outputs = 0, b_frames = 0, resolution_changes = 0;
  int fatal_after = -1;
  bool fatal_injected = false, external = false;
  std::vector<uint8_t> external_storage;
} observer;
}  // namespace

void* operator new(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) std::abort();
  if (track_allocations && n >= raw_threshold) {
    bool recorded = false;
    for (auto& item : raw_allocations) {
      if (!item.pointer) { item = {p, n}; recorded = true; break; }
    }
    CHECK(recorded);
    raw_live += n;
    raw_peak = std::max(raw_peak, raw_live);
    ++raw_count;
  }
  return p;
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (p) for (auto& item : raw_allocations) {
    if (item.pointer == p) { raw_live -= item.bytes; item = {}; break; }
  }
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }
#ifndef _WIN32
void* _aligned_malloc(size_t bytes, size_t alignment) {
  void* p = nullptr;
  return posix_memalign(&p, alignment, bytes) == 0 ? p : nullptr;
}
void _aligned_free(void* p) { std::free(p); }
#endif
// Explicit null metadata boundary: the real decoder learns dimensions and
// parameter sets from the supplied access units, as its fallback permits.
namespace gesher::media {
Format::~Format() { CHECK(get() == nullptr); }
bool Format::GetInt32(const char*, int32_t*) const { CHECK(get() == nullptr); return false; }
bool Format::GetBuffer(const char*, std::vector<uint8_t>*) const {
  CHECK(get() == nullptr);
  return false;
}
// COFF resolves the unused selection functions in video_decoder.cc too.
// Fail closed if a test accidentally invokes any other backend or metadata API.
std::string Format::Mime() const { std::abort(); }
std::vector<VideoCodecInfo> D3D11VideoCodecs() { std::abort(); }
std::vector<VideoCodecInfo> MfVideoCodecs() { std::abort(); }
std::vector<VideoCodecInfo> SoftVideoCodecs() { std::abort(); }
std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateMfVideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format&, std::string*) { std::abort(); }
}  // namespace gesher::media
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t) { std::abort(); }
void* ObservedCopy(void* target, const void* source, size_t size) {
  copied_bytes += size;
  return std::memcpy(target, source, size);
}

IV_API_CALL_STATUS_T ObservedAvcDecode(iv_obj_t* handle, void* input, void* output) {
  if (handle && handle->pv_fxns == reinterpret_cast<void*>(ObservedAvcDecode))
    handle->pv_fxns = reinterpret_cast<void*>(ih264d_api_function);
  const auto command = static_cast<const ivd_video_decode_ip_t*>(input)->e_cmd;
  if (command == IVD_CMD_CREATE) {
    const auto& create = static_cast<const ih264d_create_ip_t*>(input)->s_ivd_create_ip_t;
    CHECK(create.u4_share_disp_buf == 0);
    CHECK(create.e_output_format == IV_YUV_420P);
  }
  if (command != IVD_CMD_VIDEO_DECODE) return ih264d_api_function(handle, input, output);
  const auto& in = static_cast<const ih264d_video_decode_ip_t*>(input)->s_ivd_video_decode_ip_t;
  auto& out = static_cast<ih264d_video_decode_op_t*>(output)->s_ivd_video_decode_op_t;
  if (observer.fatal_after >= 0 && in.u4_num_Bytes > 0 &&
      observer.outputs >= static_cast<size_t>(observer.fatal_after)) {
    // Refuse before consuming an access unit. No successful frame is fabricated.
    observer.fatal_injected = true;
    out.u4_error_code = (1u << IVD_FATALERROR) | IVD_MEM_ALLOC_FAILED;
    return IV_FAIL;
  }
  const auto status = ih264d_api_function(handle, input, output);
  if ((out.u4_error_code & IVD_ERROR_MASK) == IVD_RES_CHANGED) ++observer.resolution_changes;
  if (out.u4_frame_decoded_flag && out.e_pic_type == IV_B_FRAME) ++observer.b_frames;
  if (out.u4_output_present) {
    ++observer.outputs;
    auto& frame = out.s_disp_frm_buf;
    CHECK(frame.pv_y_buf == in.s_out_buffer.pu1_bufs[0]);
    CHECK(frame.pv_u_buf == in.s_out_buffer.pu1_bufs[1]);
    CHECK(frame.pv_v_buf == in.s_out_buffer.pu1_bufs[2]);
    if (observer.external) {
      // Ownership-negative control: forward actual pixels through non-owned
      // storage. The product's pointer check must copy them before it is reused.
      const bool was_tracking = track_allocations;
      track_allocations = false;
      const size_t y = size_t(frame.u4_y_wd) * frame.u4_y_ht;
      const size_t c = size_t(frame.u4_y_wd / 2) * (frame.u4_y_ht / 2);
      observer.external_storage.resize(y + 2 * c);
      auto* dst = observer.external_storage.data();
      for (UWORD32 r = 0; r < frame.u4_y_ht; ++r)
        std::memcpy(dst + size_t(r) * frame.u4_y_wd,
                    static_cast<const uint8_t*>(frame.pv_y_buf) + size_t(r) * frame.u4_y_strd,
                    frame.u4_y_wd);
      for (UWORD32 r = 0; r < frame.u4_y_ht / 2; ++r) {
        std::memcpy(dst + y + size_t(r) * (frame.u4_y_wd / 2),
                    static_cast<const uint8_t*>(frame.pv_u_buf) + size_t(r) * frame.u4_u_strd,
                    frame.u4_y_wd / 2);
        std::memcpy(dst + y + c + size_t(r) * (frame.u4_y_wd / 2),
                    static_cast<const uint8_t*>(frame.pv_v_buf) + size_t(r) * frame.u4_v_strd,
                    frame.u4_y_wd / 2);
      }
      frame.pv_y_buf = dst;
      frame.pv_u_buf = dst + y;
      frame.pv_v_buf = dst + y + c;
      frame.u4_y_strd = frame.u4_y_wd;
      frame.u4_u_strd = frame.u4_v_strd = frame.u4_y_wd / 2;
      // The now-unused application scratch no longer contains the result.
      // Ignoring pointer identity and adopting it must fail the pixel oracle.
      for (int plane = 0; plane < 3; ++plane)
        std::memset(in.s_out_buffer.pu1_bufs[plane], 0xa3,
                    in.s_out_buffer.u4_min_out_buf_size[plane]);
      track_allocations = was_tracking;
    }
  }
  return status;
}

#define ih264d_api_function ObservedAvcDecode
#define memcpy ObservedCopy
#ifndef GESHER_SOFT_AVC_DECODE_SOURCE
#define GESHER_SOFT_AVC_DECODE_SOURCE "compat/media/video/soft_video_avc.cc"
#endif
#include GESHER_SOFT_AVC_DECODE_SOURCE
#undef memcpy
#undef ih264d_api_function

namespace {
using gesher::media::Format;
using gesher::media::Sample;
using gesher::media::VideoDecoder;
using gesher::media::VideoFrame;
struct Clip {
  std::string name;
  int width = 0, height = 0;
  std::vector<uint64_t> hashes;
  std::vector<Sample> samples;
  size_t FrameBytes() const { return size_t(width) * height * 3 / 2; }
};
uint64_t ReadNumber(FILE* file, int bytes) {
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) {
    const int b = std::fgetc(file);
    CHECK(b != EOF);
    value |= uint64_t(b) << (8 * i);
  }
  return value;
}
Clip ReadClip(const std::string& directory, const char* name) {
  Clip clip;
  clip.name = name;
  FILE* file = std::fopen((directory + '/' + name + ".avct").c_str(), "rb");
  CHECK(file != nullptr);
  CHECK(ReadNumber(file, 4) == 0x43564147);
  clip.width = static_cast<int>(ReadNumber(file, 4));
  clip.height = static_cast<int>(ReadNumber(file, 4));
  const size_t count = ReadNumber(file, 4);
  CHECK(clip.width > 0 && clip.width <= 8192 && clip.height > 0 && clip.height <= 8192);
  CHECK(count > 0 && count <= 128);
  clip.hashes.resize(count);
  clip.samples.resize(count);
  for (auto& hash : clip.hashes) hash = ReadNumber(file, 8);
  for (auto& sample : clip.samples) {
    const size_t bytes = ReadNumber(file, 4);
    CHECK(bytes > 0 && bytes <= 16 * 1024 * 1024);
    sample.time_us = static_cast<int64_t>(ReadNumber(file, 8));
    sample.data.resize(bytes);
    CHECK(std::fread(sample.data.data(), 1, bytes, file) == bytes);
  }
  CHECK(std::fgetc(file) == EOF && !std::ferror(file));
  CHECK(std::fclose(file) == 0);
  return clip;
}
std::unique_ptr<VideoDecoder> Create(const Clip& clip) {
  CHECK(raw_live == 0);
  raw_threshold = clip.FrameBytes();
  raw_peak = raw_count = copied_bytes = 0;
  observer = {};
  Format format(nullptr);
  std::string error;
  track_allocations = true;
  auto decoder = gesher::media::CreateSoftAvcDecoder(format, &error);
  track_allocations = false;
  CHECK(decoder != nullptr && error.empty());
  return decoder;
}
void Verify(const VideoFrame& frame, const Clip& clip, size_t index, int64_t offset = 0) {
  CHECK(index < clip.hashes.size());
  CHECK(frame.width == clip.width && frame.height == clip.height);
  CHECK(frame.stride_y == clip.width && frame.stride_uv == clip.width / 2);
  CHECK(frame.crop_left == 0 && frame.crop_top == 0);
  CHECK(frame.crop_right == clip.width - 1 && frame.crop_bottom == clip.height - 1);
  CHECK(frame.data.size() == clip.FrameBytes());
  CHECK(Hash(frame.data.data(), frame.data.size()) == clip.hashes[index]);
  if (frame.time_us != int64_t(index) * 33333 + 7 + offset)
    std::fprintf(stderr, "timestamp %s frame %zu: actual %lld expected %lld\n", clip.name.c_str(),
                 index, static_cast<long long>(frame.time_us),
                 static_cast<long long>(int64_t(index) * 33333 + 7 + offset));
  CHECK(frame.time_us == int64_t(index) * 33333 + 7 + offset);
}
bool Decode(VideoDecoder* decoder, const Sample& source, std::vector<VideoFrame>* out,
            int64_t offset = 0) {
  Sample sample = source;
  sample.time_us += offset;
  track_allocations = true;
  const bool ok = decoder->Decode(sample, out);
  track_allocations = false;
  std::fill(sample.data.begin(), sample.data.end(), 0xdb);
  std::fill(observer.external_storage.begin(), observer.external_storage.end(), 0xe7);
  return ok;
}
void Drain(VideoDecoder* decoder, std::vector<VideoFrame>* out) {
  track_allocations = true;
  decoder->Drain(out);
  track_allocations = false;
  std::fill(observer.external_storage.begin(), observer.external_storage.end(), 0xe7);
}
void Retained(const Clip& clip, const std::string& directory, bool external = false) {
  auto decoder = Create(clip);
  observer.external = external;
  std::vector<VideoFrame> frames;
  for (const auto& sample : clip.samples) {
    CHECK(Decode(decoder.get(), sample, &frames));
    for (size_t i = 0; i < frames.size(); ++i) Verify(frames[i], clip, i);
  }
  CHECK(observer.b_frames > 0);
  CHECK(frames.size() < clip.samples.size());  // Real reordered output remains to drain.
  Drain(decoder.get(), &frames);
  CHECK(frames.size() == clip.samples.size());
  decoder.reset();
  FILE* actual = external ? nullptr : std::fopen((directory + '/' + clip.name + ".actual.i420").c_str(), "wb");
  CHECK(external || actual != nullptr);
  for (size_t i = 0; i < frames.size(); ++i) {
    Verify(frames[i], clip, i);  // Frames outlive the codec itself.
    if (actual) CHECK(std::fwrite(frames[i].data.data(), 1, frames[i].data.size(), actual) == frames[i].data.size());
  }
  if (actual) CHECK(std::fclose(actual) == 0);
  if (external) CHECK(copied_bytes == frames.size() * clip.FrameBytes());
  frames.clear();
  CHECK(raw_live == 0);
}
void Measure(const Clip& clip, bool gate) {
  auto decoder = Create(clip);
  std::vector<VideoFrame> frames;
  size_t index = 0, measured = 0, bytes = 0, peak = 0, allocations = 0;
  for (size_t i = 0; i < clip.samples.size(); ++i) {
    frames.clear();
    raw_peak = raw_live;
    raw_count = copied_bytes = 0;
    CHECK(Decode(decoder.get(), clip.samples[i], &frames));
    for (const auto& frame : frames) Verify(frame, clip, index++);
    if (i >= 6 && !frames.empty()) {
      measured += frames.size();
      bytes += copied_bytes;
      peak = std::max(peak, raw_peak);
      allocations += raw_count;
    }
  }
  const bool packed = clip.width % 128 == 0 && clip.height % 16 == 0;
  CHECK(measured > 0);
  if (gate && packed) { CHECK(bytes == 0); CHECK(peak <= clip.FrameBytes()); }
  if (!packed) CHECK(bytes == measured * clip.FrameBytes());
  std::printf("metric,%s,frames=%zu,copied_bytes=%zu,peak_raw_bytes=%zu,raw_allocations=%zu\n",
              clip.name.c_str(), measured, bytes, peak, allocations);
  frames.clear();
  Drain(decoder.get(), &frames);
  for (const auto& frame : frames) Verify(frame, clip, index++);
  CHECK(index == clip.samples.size());
  decoder.reset();
  frames.clear();
  CHECK(raw_live == 0);
}
void ResetAndFailure(const Clip& clip) {
  auto decoder = Create(clip);
  std::vector<VideoFrame> retained;
  for (size_t i = 0; i < 6; ++i) CHECK(Decode(decoder.get(), clip.samples[i], &retained));
  CHECK(!retained.empty());
  track_allocations = true;
  decoder->Flush();
  track_allocations = false;
  std::vector<VideoFrame> replay;
  for (const auto& sample : clip.samples) CHECK(Decode(decoder.get(), sample, &replay, 10000000));
  Drain(decoder.get(), &replay);
  CHECK(replay.size() == clip.samples.size());
  for (size_t i = 0; i < replay.size(); ++i) Verify(replay[i], clip, i, 10000000);
  replay.clear();
  for (const auto& sample : clip.samples) CHECK(Decode(decoder.get(), sample, &replay, 20000000));
  Drain(decoder.get(), &replay);
  CHECK(replay.size() == clip.samples.size());
  decoder.reset();
  for (size_t i = 0; i < replay.size(); ++i) Verify(replay[i], clip, i, 20000000);
  for (size_t i = 0; i < retained.size(); ++i) Verify(retained[i], clip, i);
  retained.clear();
  replay.clear();
  CHECK(raw_live == 0);

  decoder = Create(clip);
  observer.fatal_after = 3;
  bool ok = true;
  for (const auto& sample : clip.samples) {
    ok = Decode(decoder.get(), sample, &retained);
    if (!ok) break;
  }
  CHECK(!ok && observer.fatal_injected && retained.size() == 3);
  decoder.reset();
  for (size_t i = 0; i < retained.size(); ++i) Verify(retained[i], clip, i);
  retained.clear();
  CHECK(raw_live == 0);
}
void ResolutionChange(const Clip& first, const Clip& second) {
  auto decoder = Create(first);
  std::vector<VideoFrame> frames;
  for (const auto& sample : first.samples) CHECK(Decode(decoder.get(), sample, &frames));
  for (const auto& sample : second.samples) CHECK(Decode(decoder.get(), sample, &frames, 10000000));
  Drain(decoder.get(), &frames);
  CHECK(observer.resolution_changes > 0);
  CHECK(frames.size() == first.samples.size() + second.samples.size());
  decoder.reset();
  for (size_t i = 0; i < first.samples.size(); ++i) Verify(frames[i], first, i);
  for (size_t i = 0; i < second.samples.size(); ++i)
    Verify(frames[first.samples.size() + i], second, i, 10000000);
  frames.clear();
  CHECK(raw_live == 0);
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  CHECK(argc == 3);
  const bool gate = std::string(argv[1]) != "measure";
  const std::string directory = argv[2];
  std::vector<Clip> clips;
  for (const char* name : {"small", "cropped", "720p", "1080p", "4k"})
    clips.push_back(ReadClip(directory, name));
  for (const auto& clip : clips) { Retained(clip, directory); Measure(clip, gate); }
  Retained(clips[0], directory, true);
  ResetAndFailure(clips[0]);
  ResolutionChange(clips[0], clips[2]);
  ResolutionChange(clips[2], clips[0]);
  std::printf("AVC output: %zu checks, real reordered pixels and owned output storage passed\n", checks);
}
