// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete production wrapper and real pinned libavc. The optional receipt
// delay/error controls below are explicit test boundaries, not codec features.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "ih264_typedefs.h"
#include "ih264e.h"
#include "ih264e_error.h"

namespace {
size_t checks = 0;
void Check(bool value, const char* condition, int line) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "soft AVC check failed at %d: %s\n", line, condition);
    std::abort();
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

thread_local bool count_allocations = false;
thread_local size_t allocations = 0, allocated_bytes = 0, large_allocations = 0;
thread_local size_t large_threshold = 0;
void* observed_output = nullptr;
void* observed_returned_input = nullptr;
bool output_freed = false, returned_input_freed = false;

uint64_t Hash(const void* bytes, size_t size) {
  uint64_t value = 1469598103934665603ULL;
  const auto* p = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 1099511628211ULL;
  return value;
}

struct Borrow {
  void* pointer = nullptr;
  size_t bytes = 0;
  uint64_t hash = 0;
};
struct Observer {
  std::array<Borrow, 32> borrowed{};
  std::array<void*, 32> receipts{};
  size_t receipt_count = 0;
  int delay = 0;
  bool overflow = false, fatal = false, injected = false;
  size_t retries = 0, output_floor = 0;
  void* retry_input = nullptr;
  uint32_t retry_index = 0;
  size_t input_bytes = 0;

  void Verify() const {
    for (const auto& b : borrowed) {
      if (b.pointer) CHECK(Hash(b.pointer, b.bytes) == b.hash);
    }
  }
  void Add(void* pointer) {
    if (!pointer) return;
    for (const auto& b : borrowed) CHECK(b.pointer != pointer);
    for (auto& b : borrowed) {
      if (!b.pointer) {
        b = {pointer, input_bytes, Hash(pointer, input_bytes)};
        return;
      }
    }
    CHECK(false);
  }
  void Release(void* pointer) {
    if (!pointer) return;
    for (auto& b : borrowed) {
      if (b.pointer == pointer) { b = {}; return; }
    }
    CHECK(false);
  }
} observer;
}  // namespace

// Count the wrapper's C++ allocations. Codec C allocations are not included.
void* operator new(size_t n) {
  if (count_allocations) {
    ++allocations;
    allocated_bytes += n;
    if (n >= large_threshold) ++large_allocations;
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  std::abort();
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (p == observed_output) output_freed = true;
  if (p == observed_returned_input) returned_input_freed = true;
  std::free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

#ifndef _WIN32
// The only host adaptation: the production wrapper uses Windows aligned alloc.
void* _aligned_malloc(size_t bytes, size_t alignment) {
  void* p = nullptr;
  return posix_memalign(&p, alignment, bytes) == 0 ? p : nullptr;
}
void _aligned_free(void* p) { std::free(p); }
#endif

IV_STATUS_T ObservedAvcApi(iv_obj_t* ctx, void* input, void* output) {
  // The observation macro also sees the wrapper's stored entry-point address.
  // Restore the real address before libavc validates its own handle identity.
  if (ctx && ctx->pv_fxns == reinterpret_cast<void*>(ObservedAvcApi))
    ctx->pv_fxns = reinterpret_cast<void*>(ih264e_api_function);
  const auto* command = static_cast<const iv_num_mem_rec_ip_t*>(input);
  if (command->e_cmd == IV_CMD_RETRIEVE_MEMREC) {
    observer.Verify();
    const auto result = ih264e_api_function(ctx, input, output);
    observer.borrowed = {};
    observer.receipt_count = 0;
    return result;
  }
  if (static_cast<int>(command->e_cmd) != static_cast<int>(IVE_CMD_VIDEO_ENCODE)) {
    const auto result = ih264e_api_function(ctx, input, output);
    if (result != IV_SUCCESS)
      std::fprintf(stderr, "provider command %u failed: 0x%x\n", unsigned(command->e_cmd),
                   static_cast<const UWORD32*>(output)[1]);
    return result;
  }

  auto& ip = static_cast<ih264e_video_encode_ip_t*>(input)->s_ive_ip;
  auto& op = static_cast<ih264e_video_encode_op_t*>(output)->s_ive_op;
  observed_output = ip.s_out_buf.pv_buf;
  output_freed = false;
  observer.Verify();
  void* current = ip.s_inp_buf.apv_bufs[0];
  if (current && !observer.injected && ip.u4_timestamp_low == 5 &&
      (observer.overflow || observer.fatal)) {
    // Refuse before consuming this input. No successful packet is fabricated.
    observer.injected = true;
    observer.retry_input = current;
    observer.retry_index = ip.u4_timestamp_low;
    observer.output_floor = size_t(ip.s_out_buf.u4_bufsize) * 2;
    if (observer.fatal) observer.Add(current);
    op.u4_error_code = observer.overflow ? static_cast<UWORD32>(IH264E_BITSTREAM_BUFFER_OVERFLOW)
                                       : (1u << IVE_FATALERROR) | IH264E_INIT_NOT_DONE;
    return IV_FAIL;
  }
  if (current && current == observer.retry_input) {
    CHECK(ip.u4_timestamp_low == observer.retry_index);
    CHECK(ip.s_out_buf.u4_bufsize >= observer.output_floor);
    observer.retry_input = nullptr;
    ++observer.retries;
  }
  observer.Add(current);
  const auto result = ih264e_api_function(ctx, input, output);
  if (result != IV_SUCCESS) return result;
  observer.Verify();
  void* released = op.s_inp_buf.apv_bufs[0];
  if (observer.delay && released) {
    CHECK(observer.receipt_count < observer.receipts.size());
    observer.receipts[observer.receipt_count++] = released;
    released = nullptr;
    if (observer.receipt_count > static_cast<size_t>(observer.delay)) {
      released = observer.receipts[0];
      for (size_t i = 1; i < observer.receipt_count; ++i)
        observer.receipts[i - 1] = observer.receipts[i];
      --observer.receipt_count;
    }
    op.s_inp_buf.apv_bufs[0] = released;
  }
  observer.Release(released);
  if (released) {
    observed_returned_input = released;
    returned_input_freed = false;
  }
  return result;
}

#define ih264e_api_function ObservedAvcApi
#ifndef GESHER_SOFT_AVC_SOURCE
#define GESHER_SOFT_AVC_SOURCE "compat/media/encode/soft_avc_encoder.cc"
#endif
#include GESHER_SOFT_AVC_SOURCE
#undef ih264e_api_function

namespace {
using gesher::media::EncodedPacket;
using gesher::media::VideoEncoderParams;
using gesher::media::VideoFrame;

VideoFrame Frame(int width, int height, bool padded) {
  VideoFrame frame;
  frame.width = width + (padded ? 16 : 0);
  frame.height = height + (padded ? 8 : 0);
  frame.stride_y = frame.width + (padded ? 16 : 0);
  frame.stride_uv = frame.stride_y / 2;
  frame.crop_left = padded ? 6 : 0;
  frame.crop_top = padded ? 4 : 0;
  frame.crop_right = frame.crop_left + width - 1;
  frame.crop_bottom = frame.crop_top + height - 1;
  frame.data.resize(size_t(frame.stride_y) * frame.height * 3 / 2);
  return frame;
}

void Fill(VideoFrame* frame, int index) {
  // A moving gradient with independent chroma, including non-picture padding.
  for (size_t i = 0; i < frame->data.size(); ++i)
    frame->data[i] = static_cast<uint8_t>(16 + ((i / 43 + index * 7) % 220));
  frame->time_us = 791 + int64_t(index) * 33333;
}

void Run(const char* name, int width, int height, int count, bool padded,
         bool delayed, bool overflow, bool fatal, bool measure, const std::string& directory) {
  observer = {};
  observed_output = observed_returned_input = nullptr;
  output_freed = returned_input_freed = false;
  observer.delay = delayed ? 2 : 0;
  observer.overflow = overflow;
  observer.fatal = fatal;
  observer.input_bytes = size_t(width) * height * 3 / 2;
  VideoEncoderParams params;
  params.width = width;
  params.height = height;
  params.bitrate = 2000000;
  std::string error;
  auto encoder = gesher::media::CreateSoftAvcEncoder(params, &error);
  if (!encoder) std::fprintf(stderr, "libavc init: %s\n", error.c_str());
  CHECK(encoder != nullptr);
  CHECK(!encoder->SetBitrate(0));
  auto frame = Frame(width, height, padded);
  std::vector<EncodedPacket> packets;
  packets.reserve(count + 16);
  allocations = allocated_bytes = large_allocations = 0;
  large_threshold = observer.input_bytes;
  size_t measured_frames = 0;
  for (int i = 0; i < count; ++i) {
    Fill(&frame, i);
    if (i == count / 3) CHECK(encoder->SetBitrate(1000000));
    if (i == count * 2 / 3) CHECK(encoder->SetBitrate(params.bitrate));
    // Warm-up is excluded identically from both A/B counts. Fault tests have
    // their own assertions and do not contribute to the steady-state budget.
    count_allocations = i >= 4 && !overflow && !fatal;
    if (count_allocations) ++measured_frames;
    const bool ok = encoder->Encode(frame, i == 0 || i == 9, &packets);
    count_allocations = false;
    if (fatal && i == 5) { CHECK(!ok); break; }
    CHECK(ok);
    // Caller storage is immediately reusable; deferred inputs must be owned.
    std::fill(frame.data.begin(), frame.data.end(), 0xD7);
    observer.Verify();
  }
  if (!fatal) {
    CHECK(encoder->Drain(&packets));
    CHECK(output_freed && returned_input_freed);
  }
  encoder.reset();
  for (const auto& b : observer.borrowed) CHECK(!b.pointer);
  CHECK(!packets.empty() && packets.front().config);
  CHECK(packets.front().data.size() > 0);
  size_t pictures = 0;
  uint64_t digest = 0;
  FILE* stream = nullptr;
  FILE* manifest = nullptr;
  if (!directory.empty()) {
    stream = std::fopen((directory + "/" + name + ".h264").c_str(), "wb");
    manifest = std::fopen((directory + "/" + name + ".packets").c_str(), "wb");
    CHECK(stream && manifest);
  }
  for (const auto& packet : packets) {
    CHECK(!packet.data.empty());
    if (!packet.config) {
      CHECK(packet.time_us == 791 + int64_t(pictures) * 33333);
      if (pictures == 0 || pictures == 9) CHECK(packet.key);
      ++pictures;
    }
    const uint64_t hash = Hash(packet.data.data(), packet.data.size());
    digest ^= hash;
    if (stream) {
      CHECK(std::fwrite(packet.data.data(), 1, packet.data.size(), stream) == packet.data.size());
      CHECK(std::fprintf(manifest, "%lld %d %d %zu %llu\n", static_cast<long long>(packet.time_us),
                         packet.key, packet.config, packet.data.size(),
                         static_cast<unsigned long long>(hash)) > 0);
    }
  }
  if (stream) { CHECK(std::fclose(stream) == 0); CHECK(std::fclose(manifest) == 0); }
  CHECK(pictures == static_cast<size_t>(fatal ? 5 : count));
  if (overflow) CHECK(observer.injected && observer.retries == 1);
  if (fatal) CHECK(observer.injected && observer.retries == 0);
  if (!overflow && !fatal && !measure) CHECK(large_allocations == 0);
  std::printf("CASE %s frames=%zu measured=%zu cpp_allocations=%zu cpp_bytes=%zu large=%zu digest=%llu\n",
              name, pictures, measured_frames, allocations, allocated_bytes, large_allocations,
              static_cast<unsigned long long>(digest));
}
}  // namespace

int main(int argc, char** argv) {
  const bool measure = argc > 1 && std::strcmp(argv[1], "measure") == 0;
  const std::string directory = argc > 2 ? argv[2] : "";
  Run("plain", 320, 240, 24, false, false, false, false, measure, directory);
  Run("crop", 320, 240, 24, true, false, false, false, measure, directory);
  Run("delayed", 320, 240, 24, true, true, false, false, measure, directory);
  Run("overflow", 320, 240, 12, true, false, true, false, measure, directory);
  Run("fatal", 320, 240, 12, true, true, false, true, measure, directory);
  Run("720p", 1280, 720, 24, true, false, false, false, measure, directory);
  Run("1080p", 1920, 1080, 12, true, false, false, false, measure, directory);
  std::printf("soft_avc_reuse_test: %zu checks; real codec packets, owned inputs and bounded scratch passed\n", checks);
  return 0;
}
