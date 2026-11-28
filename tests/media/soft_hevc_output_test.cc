// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete product decoder, real pinned libhevc and independent FFmpeg fixtures.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <map>
#include <thread>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>
#include "ihevc_typedefs.h"
#include "ihevcd_cxa.h"
#include "compat/media/video/video_decoder.h"
#include <dav1d/dav1d.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

namespace {
size_t checks = 0;
void Check(bool result, const char* condition, int line) {
  ++checks;
  if (!result) {
    std::fprintf(stderr, "HEVC output check failed at %d: %s\n", line, condition);
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
  size_t outputs = 0, reordered_outputs = 0, resolution_changes = 0;
  int fatal_after = -1;
  bool fatal_injected = false;
  unsigned external = 0;
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
std::string Format::Mime() const { CHECK(get() == nullptr); return "video/hevc"; }
std::vector<VideoCodecInfo> D3D11VideoCodecs() { std::abort(); }
std::vector<VideoCodecInfo> MfVideoCodecs() { std::abort(); }
std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateMfVideoDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateSoftAvcDecoder(const Format&, std::string*) { std::abort(); }
std::unique_ptr<VideoDecoder> CreateSoftM4vDecoder(const Format&, bool, std::string*) { std::abort(); }
}  // namespace gesher::media
// Headers are the real pinned dependencies. These unselected backends must
// never execute in this HEVC-only fixture, including in COFF-linked builds.
extern "C" {
vpx_codec_iface_t* vpx_codec_vp8_dx() { std::abort(); }
vpx_codec_iface_t* vpx_codec_vp9_dx() { std::abort(); }
vpx_codec_err_t vpx_codec_destroy(vpx_codec_ctx_t*) { std::abort(); }
const char* vpx_codec_err_to_string(vpx_codec_err_t) { std::abort(); }
const char* vpx_codec_error_detail(const vpx_codec_ctx_t*) { std::abort(); }
vpx_codec_err_t vpx_codec_dec_init_ver(vpx_codec_ctx_t*, vpx_codec_iface_t*,
    const vpx_codec_dec_cfg_t*, vpx_codec_flags_t, int) { std::abort(); }
vpx_codec_err_t vpx_codec_decode(vpx_codec_ctx_t*, const uint8_t*, unsigned int, void*, long) { std::abort(); }
vpx_image_t* vpx_codec_get_frame(vpx_codec_ctx_t*, vpx_codec_iter_t*) { std::abort(); }
void dav1d_default_settings(Dav1dSettings*) { std::abort(); }
int dav1d_open(Dav1dContext**, const Dav1dSettings*) { std::abort(); }
int dav1d_send_data(Dav1dContext*, Dav1dData*) { std::abort(); }
int dav1d_get_picture(Dav1dContext*, Dav1dPicture*) { std::abort(); }
void dav1d_close(Dav1dContext**) { std::abort(); }
void dav1d_flush(Dav1dContext*) { std::abort(); }
uint8_t* dav1d_data_create(Dav1dData*, size_t) { std::abort(); }
void dav1d_data_unref(Dav1dData*) { std::abort(); }
void dav1d_picture_unref(Dav1dPicture*) { std::abort(); }
}
extern "C" void AMediaFormat_setBuffer(AMediaFormat*, const char*, const void*, size_t) { std::abort(); }
void* ObservedCopy(void* target, const void* source, size_t size) {
  copied_bytes += size;
  return std::memcpy(target, source, size);
}

IV_API_CALL_STATUS_T ObservedHevcDecode(iv_obj_t* handle, void* input, void* output) {
  if (handle && handle->pv_fxns == reinterpret_cast<void*>(ObservedHevcDecode))
    handle->pv_fxns = reinterpret_cast<void*>(ihevcd_cxa_api_function);
  const auto command = static_cast<const ivd_video_decode_ip_t*>(input)->e_cmd;
  if (command == IVD_CMD_CREATE) {
    const auto& create = static_cast<const ihevcd_cxa_create_ip_t*>(input)->s_ivd_create_ip_t;
    CHECK(create.u4_share_disp_buf == 0);
    CHECK(create.e_output_format == IV_YUV_420P);
    CHECK(static_cast<const ihevcd_cxa_create_ip_t*>(input)->u4_keep_threads_active == 0);
  }
  if (command != IVD_CMD_VIDEO_DECODE) return ihevcd_cxa_api_function(handle, input, output);
  const auto& in = static_cast<const ihevcd_cxa_video_decode_ip_t*>(input)->s_ivd_video_decode_ip_t;
  auto& out = static_cast<ihevcd_cxa_video_decode_op_t*>(output)->s_ivd_video_decode_op_t;
  if (observer.fatal_after >= 0 && in.u4_num_Bytes > 0 &&
      observer.outputs >= static_cast<size_t>(observer.fatal_after)) {
    // Refuse before consuming an access unit. No successful frame is fabricated.
    observer.fatal_injected = true;
    out.u4_error_code = (1u << IVD_FATALERROR) | IVD_MEM_ALLOC_FAILED;
    return IV_FAIL;
  }
  const auto status = ihevcd_cxa_api_function(handle, input, output);
  if ((out.u4_error_code & IVD_ERROR_MASK) == IVD_RES_CHANGED) ++observer.resolution_changes;
  // This libhevc revision never assigns e_dec_pic_type. Observe actual
  // delayed marker identities instead of interpreting that unused field.
  if (out.u4_output_present && in.u4_num_Bytes > 0 && out.u4_ts != in.u4_ts)
    ++observer.reordered_outputs;
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
      if (observer.external & 1) frame.pv_y_buf = dst;
      if (observer.external & 2) frame.pv_u_buf = dst + y;
      if (observer.external & 4) frame.pv_v_buf = dst + y + c;
      frame.u4_y_strd = frame.u4_y_wd;
      frame.u4_u_strd = frame.u4_v_strd = frame.u4_y_wd / 2;
      // The now-unused application scratch no longer contains the result.
      // Ignoring pointer identity and adopting it must fail the pixel oracle.
      for (int plane = 0; plane < 3; ++plane)
        if (observer.external & (1u << plane)) std::memset(in.s_out_buffer.pu1_bufs[plane], 0xa3,
                    in.s_out_buffer.u4_min_out_buf_size[plane]);
      track_allocations = was_tracking;
    }
  }
  return status;
}

#define ihevcd_cxa_api_function ObservedHevcDecode
#define memcpy ObservedCopy
#ifndef GESHER_SOFT_HEVC_DECODE_SOURCE
#define GESHER_SOFT_HEVC_DECODE_SOURCE "compat/media/video/soft_video.cc"
#endif
#include GESHER_SOFT_HEVC_DECODE_SOURCE
#undef memcpy
#undef ihevcd_cxa_api_function

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
  FILE* file = std::fopen((directory + '/' + name + ".hevct").c_str(), "rb");
  CHECK(file != nullptr);
  CHECK(ReadNumber(file, 4) == 0x43564548);
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
  CHECK(!std::is_sorted(clip.samples.begin(), clip.samples.end(),
      [](const Sample& a, const Sample& b) { return a.time_us < b.time_us; }));
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
  auto decoder = gesher::media::CreateSoftVideoDecoder(format, &error);
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
void Retained(const Clip& clip, const std::string& directory, unsigned external = 0) {
  auto decoder = Create(clip);
  observer.external = external;
  std::vector<VideoFrame> frames;
  for (const auto& sample : clip.samples) {
    CHECK(Decode(decoder.get(), sample, &frames));
    for (size_t i = 0; i < frames.size(); ++i) Verify(frames[i], clip, i);
  }
  CHECK(observer.reordered_outputs > 0);
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
  size_t full_bytes = 0, full_peak = 0, full_allocations = 0;
  auto include_call = [&] {
    full_bytes += copied_bytes;
    full_peak = std::max(full_peak, raw_peak);
    full_allocations += raw_count;
  };
  for (size_t i = 0; i < clip.samples.size(); ++i) {
    frames.clear();
    raw_peak = raw_live;
    raw_count = copied_bytes = 0;
    CHECK(Decode(decoder.get(), clip.samples[i], &frames));
    for (const auto& frame : frames) Verify(frame, clip, index++);
    include_call();
    if (i >= 6 && !frames.empty()) {
      measured += frames.size();
      bytes += copied_bytes;
      peak = std::max(peak, raw_peak);
      allocations += raw_count;
    }
  }
  frames.clear();
  raw_peak = raw_live;
  raw_count = copied_bytes = 0;
  Drain(decoder.get(), &frames);
  include_call();
  for (const auto& frame : frames) Verify(frame, clip, index++);
  CHECK(index == clip.samples.size());
  const bool packed = clip.width % 128 == 0 && clip.height % 16 == 0;
  CHECK(measured > 0);
  if (gate && packed) {
    CHECK(bytes == 0 && full_bytes == 0);
    CHECK(peak <= clip.FrameBytes());
  }
  if (!packed) {
    CHECK(bytes == measured * clip.FrameBytes());
    CHECK(full_bytes == index * clip.FrameBytes());
  }
  std::printf("metric,%s,frames=%zu,copied_bytes=%zu,peak_raw_bytes=%zu,raw_allocations=%zu,"
              "steady_frames=%zu,steady_copied_bytes=%zu,steady_peak_raw_bytes=%zu,"
              "steady_raw_allocations=%zu,drain_frames=%zu\n", clip.name.c_str(), index,
              full_bytes, full_peak, full_allocations, measured, bytes, peak, allocations, frames.size());
  decoder.reset();
  frames.clear();
  CHECK(raw_live == 0);
}
void MutatedConsumerOutput(const Clip& clip) {
  auto decoder = Create(clip);
  std::vector<VideoFrame> frames;
  auto inspect = [&](size_t previous) {
    for (size_t i = 0; i < previous; ++i)
      CHECK(std::all_of(frames[i].data.begin(), frames[i].data.end(), [](uint8_t v) { return v == 0xa7; }));
    for (size_t i = previous; i < frames.size(); ++i) {
      Verify(frames[i], clip, i);
      // Caller writes must not alter the provider's retained prediction frame.
      std::fill(frames[i].data.begin(), frames[i].data.end(), 0xa7);
    }
  };
  for (const auto& sample : clip.samples) {
    size_t previous = frames.size();
    CHECK(Decode(decoder.get(), sample, &frames));
    inspect(previous);
  }
  size_t previous = frames.size();
  Drain(decoder.get(), &frames);
  CHECK(frames.size() == clip.samples.size());
  inspect(previous);
  decoder.reset();
  inspect(frames.size());
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
  for (unsigned foreign_plane_mask : {1u, 2u, 4u, 7u}) Retained(clips[0], directory, foreign_plane_mask);
  MutatedConsumerOutput(clips[0]);
  ResetAndFailure(clips[0]);
  ResolutionChange(clips[0], clips[2]);
  ResolutionChange(clips[2], clips[0]);
  std::printf("HEVC output: %zu checks, real reordered pixels and owned output storage passed\n", checks);
}
