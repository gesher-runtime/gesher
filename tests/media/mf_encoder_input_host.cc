// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The complete production translation unit runs below. Win32 activation and MF
// objects are explicit adapters; no real MFT, GPU, Windows or ART runs here.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "tests/media/mf_encoder_host.h"
#include "compat/media/encode/encoder.h"

namespace fixture {
size_t checks = 0, failures = 0;
void Check(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    if (failures < 30) std::fprintf(stderr, "FAIL: %s\n", what);
  }
}
struct Counts {
  bool active = false;
  size_t allocations = 0, bytes = 0, live = 0, peak = 0, copies = 0, copy_bytes = 0;
} counts;
struct alignas(std::max_align_t) Allocation { size_t size; bool counted; };
struct Uncounted {
  bool previous = counts.active;
  Uncounted() { counts.active = false; }
  ~Uncounted() { counts.active = previous; }
};
void* Copy(void* to, const void* from, size_t size) {
  if (counts.active) { ++counts.copies; counts.copy_bytes += size; }
  return std::memcpy(to, from, size);
}
int WindowsSnprintf(char* out, size_t size, const char* format, unsigned long value) {
  // The real target's unsigned long is 32 bits; this Linux host's is 64 bits.
  Check(std::strcmp(format, "0x%08lx") == 0, "declared LLP64 HRESULT formatting boundary");
  return std::snprintf(out, size, "0x%08x", static_cast<uint32_t>(value));
}
}

void* operator new(size_t size) {
  using namespace fixture;
  auto* header = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + std::max<size_t>(size, 1)));
  if (header == nullptr) std::abort();
  header->size = size;
  header->counted = counts.active;
  if (counts.active) {
    ++counts.allocations;
    counts.bytes += size;
    counts.live += size;
    counts.peak = std::max(counts.peak, counts.live);
  }
  return header + 1;
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (p == nullptr) return;
  auto* header = static_cast<fixture::Allocation*>(p) - 1;
  if (header->counted) fixture::counts.live -= header->size;
  std::free(header);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

// Count explicit production memcpy bytes, not observer/oracle copies. All
// standard and production public headers above have already been included.
#define memcpy fixture::Copy
#define snprintf fixture::WindowsSnprintf
#include GESHER_MF_ENCODER_SOURCE
#undef snprintf
#undef memcpy

namespace fixture {
enum class Fault { None, CreateBuffer, Lock, ShortBuffer, NullBuffer, Length, Unlock,
                   CreateSample, NullSample, AddBuffer, Time, Duration };
struct State {
  Fault fault = Fault::None;
  size_t buffers = 0, samples = 0, transforms = 0, types = 0, activates = 0;
  size_t create_buffers = 0, locks = 0, unlocks = 0, successful_locks = 0;
  size_t inputs = 0, accepted = 0, outputs = 0, forced_keys = 0, events_released = 0;
  int reject_inputs = 0;
  bool output_error = false, emit = false, provides = true, hold = false, clean = false;
  bool next_idr = true;
  int64_t seen_time = 0, seen_duration = 0;
  IMFSample* first_input = nullptr;
  IMFSample* last_input = nullptr;
  const std::vector<uint8_t>* expected = nullptr;
  std::array<size_t, 4> messages{};
} state;

bool Same(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

struct Buffer final : IMFMediaBuffer {
  ULONG refs = 1;
  BYTE* bytes = nullptr;
  DWORD capacity = 0, length = 0;
  bool locked = false;
  Buffer() { ++state.buffers; }
  ~Buffer() { Check(!locked, "buffer lock balanced before destruction"); ::operator delete(bytes); --state.buffers; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) delete this; return n; }
  HRESULT Lock(BYTE** p, DWORD* maximum, DWORD* current) override {
    ++state.locks;
    if (state.fault == Fault::Lock) return E_FAIL;
    Check(!locked, "fresh sample locked once");
    locked = true;
    ++state.successful_locks;
    *p = state.fault == Fault::NullBuffer ? nullptr : bytes;
    if (maximum) *maximum = state.fault == Fault::ShortBuffer ? capacity - 1 : capacity;
    if (current) *current = length;
    return S_OK;
  }
  HRESULT Unlock() override {
    ++state.unlocks;
    Check(locked, "Unlock pairs with successful Lock");
    locked = false;
    return state.fault == Fault::Unlock ? E_FAIL : S_OK;
  }
  HRESULT SetCurrentLength(DWORD n) override {
    Check(n <= capacity, "valid length fits allocated MF buffer");
    if (state.fault == Fault::Length || n > capacity) return E_FAIL;
    length = n;
    return S_OK;
  }
};

struct Sample final : IMFSample {
  ULONG refs = 1;
  Buffer* buffer = nullptr;
  LONGLONG time = 0, duration = 0;
  bool clean = false;
  Sample() { ++state.samples; }
  ~Sample() { if (buffer) buffer->Release(); --state.samples; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) delete this; return n; }
  HRESULT AddBuffer(IMFMediaBuffer* b) override {
    if (state.fault == Fault::AddBuffer) return E_FAIL;
    Check(buffer == nullptr, "sample receives exactly one buffer");
    buffer = static_cast<Buffer*>(b);
    buffer->AddRef();
    return S_OK;
  }
  HRESULT SetSampleTime(LONGLONG value) override {
    if (state.fault == Fault::Time) return E_FAIL;
    time = value;
    return S_OK;
  }
  HRESULT SetSampleDuration(LONGLONG value) override {
    if (state.fault == Fault::Duration) return E_FAIL;
    duration = value;
    return S_OK;
  }
  HRESULT GetSampleTime(LONGLONG* value) override { *value = time; return S_OK; }
  HRESULT GetUINT32(const GUID&, UINT32* value) override { *value = clean; return S_OK; }
  HRESULT ConvertToContiguousBuffer(IMFMediaBuffer** value) override {
    *value = buffer;
    buffer->AddRef();
    return S_OK;
  }
};

struct MediaType final : IMFMediaType {
  ULONG refs = 1;
  MediaType() { ++state.types; }
  ~MediaType() { --state.types; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) delete this; return n; }
  HRESULT SetGUID(const GUID&, const GUID&) override { return S_OK; }
  HRESULT SetUINT32(const GUID&, UINT32) override { return S_OK; }
  HRESULT SetUINT64(const GUID&, UINT64) override { return S_OK; }
};

struct Codec final : gesher::media::CodecApi {
  ULONG refs = 1;
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { Check(refs > 1, "codec query reference released once"); return --refs; }
  HRESULT IsSupported(const GUID*) override { return S_OK; }
  HRESULT IsModifiable(const GUID*) override { return S_OK; }
  HRESULT GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) override { return E_FAIL; }
  HRESULT GetParameterValues(const GUID*, VARIANT**, ULONG*) override { return E_FAIL; }
  HRESULT GetDefaultValue(const GUID*, VARIANT*) override { return E_FAIL; }
  HRESULT GetValue(const GUID*, VARIANT*) override { return E_FAIL; }
  HRESULT SetValue(const GUID* key, VARIANT* value) override {
    Check(value->vt == VT_UI4, "codec property uses the original UINT32 variant");
    if (Same(*key, gesher::media::kApiForceKeyFrame)) {
      Check(value->ulVal == 1, "key-frame request keeps its value");
      ++state.forced_keys;
    }
    return S_OK;
  }
};

HRESULT CreateBuffer(DWORD size, IMFMediaBuffer** result) {
  ++state.create_buffers;
  if (state.fault == Fault::CreateBuffer) return E_FAIL;
  Buffer* buffer;
  { Uncounted ignored; buffer = new Buffer; }
  // Include exactly the requested MF payload allocation in the reported input
  // storage budget. Adapter COM object bookkeeping is excluded on both sources.
  buffer->bytes = static_cast<BYTE*>(::operator new(size));
  buffer->capacity = size;
  std::memset(buffer->bytes, 0xe3, size);
  *result = buffer;
  return S_OK;
}
HRESULT CreateSample(IMFSample** result) {
  if (state.fault == Fault::CreateSample) return E_FAIL;
  if (state.fault == Fault::NullSample) return S_OK;
  Uncounted ignored;
  *result = new Sample;
  return S_OK;
}

struct Event final : IUnknown {
  ULONG refs = 1;
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) { ++state.events_released; delete this; } return n; }
};

struct Transform final : IMFTransform {
  ULONG refs = 1;
  Codec codec;
  std::array<Sample*, 256> retained{};
  size_t retained_count = 0;
  Transform() { ++state.transforms; }
  ~Transform() {
    for (size_t i = 0; i < retained_count; ++i) retained[i]->Release();
    Check(codec.refs == 1, "transform has no leaked codec interface refs");
    --state.transforms;
  }
  HRESULT QueryInterface(const IID& iid, void** result) override {
    if (!Same(iid, gesher::media::kIidCodecApi)) return E_NOINTERFACE;
    *result = static_cast<gesher::media::CodecApi*>(&codec);
    codec.AddRef();
    return S_OK;
  }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) delete this; return n; }
  HRESULT SetOutputType(DWORD, IMFMediaType*, DWORD) override { return S_OK; }
  HRESULT SetInputType(DWORD, IMFMediaType*, DWORD) override { return S_OK; }
  HRESULT ProcessMessage(MFT_MESSAGE_TYPE message, uintptr_t) override {
    ++state.messages[static_cast<size_t>(message)]; return S_OK;
  }
  HRESULT GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO* info) override {
    info->dwFlags = state.provides ? MFT_OUTPUT_STREAM_PROVIDES_SAMPLES : 0;
    info->cbSize = 512;
    return S_OK;
  }
  HRESULT ProcessInput(DWORD id, IMFSample* raw, DWORD flags) override {
    ++state.inputs;
    Check(id == 0 && flags == 0, "input stream and flags preserved");
    if (!state.first_input) state.first_input = raw;
    state.last_input = raw;
    auto* sample = static_cast<Sample*>(raw);
    Check(sample->buffer && !sample->buffer->locked, "submitted sample owns an unlocked buffer");
    if (state.expected) {
      Check(sample->buffer->length == state.expected->size(), "submitted packed size matches the oracle");
      Check(sample->buffer->length == state.expected->size() &&
            std::memcmp(sample->buffer->bytes, state.expected->data(), state.expected->size()) == 0,
            "every submitted crop byte matches the independent oracle");
    }
    state.seen_time = sample->time;
    state.seen_duration = sample->duration;
    if (state.reject_inputs > 0) { --state.reject_inputs; return MF_E_NOTACCEPTING; }
    ++state.accepted;
    if (state.hold) {
      Check(retained_count < retained.size(), "fixture retained sample capacity");
      sample->AddRef();
      retained[retained_count++] = sample;
    }
    return S_OK;
  }
  HRESULT ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* output, DWORD*) override {
    ++state.outputs;
    Check(count == 1, "one output buffer requested");
    if (state.output_error) return E_FAIL;
    if (!state.emit) return MF_E_TRANSFORM_NEED_MORE_INPUT;
    state.emit = false;
    IMFSample* raw = output->pSample;
    if (!raw) {
      Check(SUCCEEDED(CreateSample(&raw)), "fixture output sample created");
      IMFMediaBuffer* buffer = nullptr;
      Check(SUCCEEDED(CreateBuffer(64, &buffer)), "fixture output buffer created");
      Check(SUCCEEDED(raw->AddBuffer(buffer)), "fixture output buffer attached");
      buffer->Release();
    }
    auto* sample = static_cast<Sample*>(raw);
    const uint8_t data[] = {0,0,0,1,0x67,0x42,0x80, 0,0,1,0x68,0x11,
                           0,0,0,1,0x09,0xf0, 0,0,0,1,0x65,0x33,0x44};
    std::memcpy(sample->buffer->bytes, data, sizeof(data));
    if (!state.next_idr) sample->buffer->bytes[22] = 0x61;
    sample->buffer->length = sizeof(data);
    sample->time = 123450;
    sample->clean = state.clean;
    output->pSample = sample;
    output->pEvents = new Event;
    return S_OK;
  }
};
Transform* latest = nullptr;

struct Activate final : IMFActivate {
  ULONG refs = 1;
  Activate() { ++state.activates; }
  ~Activate() { --state.activates; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override { const auto n = --refs; if (!n) delete this; return n; }
  HRESULT ActivateObject(const IID&, void** result) override {
    Uncounted ignored;
    latest = new Transform;
    *result = latest;
    return S_OK;
  }
};
HRESULT Startup(ULONG, DWORD) { return S_OK; }
HRESULT CreateType(IMFMediaType** result) { Uncounted ignored; *result = new MediaType; return S_OK; }
HRESULT Enum(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*,
             IMFActivate*** result, UINT32* count) {
  Uncounted ignored;
  auto** entries = static_cast<IMFActivate**>(std::malloc(sizeof(IMFActivate*)));
  if (!entries) std::abort();
  entries[0] = new Activate;
  *result = entries;
  *count = 1;
  return S_OK;
}
void Empty() {
  Check(state.buffers == 0 && state.samples == 0 && state.transforms == 0 &&
        state.types == 0 && state.activates == 0, "all adapter COM objects retired");
  Check(state.successful_locks == state.unlocks, "all successful locks have one unlock");
}
void Reset() { Empty(); state = {}; latest = nullptr; }
}

HMODULE LoadLibraryW(const wchar_t*) { return reinterpret_cast<HMODULE>(uintptr_t{1}); }
FARPROC GetProcAddress(HMODULE, const char* name) {
  using namespace fixture;
  if (std::strcmp(name, "MFStartup") == 0) return reinterpret_cast<FARPROC>(Startup);
  if (std::strcmp(name, "MFCreateMediaType") == 0) return reinterpret_cast<FARPROC>(CreateType);
  if (std::strcmp(name, "MFCreateSample") == 0) return reinterpret_cast<FARPROC>(CreateSample);
  if (std::strcmp(name, "MFCreateMemoryBuffer") == 0) return reinterpret_cast<FARPROC>(CreateBuffer);
  if (std::strcmp(name, "MFTEnumEx") == 0) return reinterpret_cast<FARPROC>(Enum);
  return nullptr;
}
HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
void CoTaskMemFree(void* p) { std::free(p); }

namespace fixture {
using gesher::media::VideoFrame;
using gesher::media::EncodedPacket;
using gesher::media::VideoEncoder;
using gesher::media::VideoEncoderParams;
uint32_t Random(uint32_t* seed) { *seed = *seed * 1664525u + 1013904223u; return *seed; }
VideoFrame Frame(int w, int h, int left, int top, uint32_t seed) {
  VideoFrame f;
  f.width = w + left + 3;
  f.height = h + top + 3;
  f.stride_y = f.width + 7;
  f.stride_uv = (f.width + 1) / 2 + 5;
  f.crop_left = left; f.crop_top = top;
  f.crop_right = left + w - 1; f.crop_bottom = top + h - 1;
  f.time_us = 9876543;
  f.data.resize(static_cast<size_t>(f.stride_y) * f.height +
                2 * static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2));
  for (uint8_t& b : f.data) b = static_cast<uint8_t>(Random(&seed) >> 24);
  return f;
}

// Address each visible byte independently; no row-copy helper is shared with
// production. Odd crop origins retain the baseline's integer chroma selection.
std::vector<uint8_t> Oracle(const VideoFrame& f, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 3 / 2);
  size_t at = 0;
  for (int plane = 0; plane != 3; ++plane) {
    const int rows = plane == 0 ? h : h / 2;
    const int cols = plane == 0 ? w : w / 2;
    const int stride = plane == 0 ? f.stride_y : f.stride_uv;
    const int x0 = plane == 0 ? f.crop_left : f.crop_left / 2;
    const int y0 = plane == 0 ? f.crop_top : f.crop_top / 2;
    const size_t begin = plane == 0 ? 0 : static_cast<size_t>(f.stride_y) * f.height +
        (plane - 1) * static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
    for (int y = 0; y < rows; ++y)
      for (int x = 0; x < cols; ++x)
        out[at++] = f.data[begin + static_cast<size_t>(y0 + y) * stride + x0 + x];
  }
  return out;
}
std::unique_ptr<VideoEncoder> Encoder(int w, int h, int rate = 30) {
  VideoEncoderParams p;
  p.width = w; p.height = h; p.frame_rate = rate;
  std::string error;
  auto encoder = gesher::media::CreateMfH264Encoder(p, &error);
  Check(encoder != nullptr, "production encoder initializes at the declared MFT boundary");
  return encoder;
}

void ValidCases() {
  uint32_t seed = 0x42af;
  const int64_t times[] = {0, 1, -1, 987654321, INT64_MAX / 10, INT64_MIN / 10};
  const int rates[] = {0, -1, 1, 30, 60, 1000001};
  for (size_t i = 0; i < 256; ++i) {
    Reset();
    int w = 2 + 2 * (Random(&seed) % 64), h = 2 + 2 * (Random(&seed) % 48);
    auto frame = Frame(w, h, Random(&seed) % 6, Random(&seed) % 6, seed);
    frame.time_us = times[i % 6];
    auto before = frame.data;
    auto expected = Oracle(frame, w, h);
    auto encoder = Encoder(w, h, rates[i % 6]);
    state.expected = &expected;
    std::vector<EncodedPacket> out(1);
    out.front().data = {0x29};
    const bool key = i % 9 == 0;
    Check(encoder->Encode(frame, key, &out), "valid cropped I420 input succeeds");
    Check(state.seen_time == frame.time_us * 10, "signed timestamp converted exactly to 100ns");
    Check(state.seen_duration == (1000000 / std::max(rates[i % 6], 1)) * 10,
          "frame duration keeps integer rounding");
    Check(state.forced_keys == static_cast<size_t>(key), "key request sent exactly when requested");
    Check(frame.data == before, "source pixels and padding remain unchanged");
    Check(out.size() == 1 && out.front().data == std::vector<uint8_t>{0x29},
          "no output invented and earlier packet remains");
    encoder.reset(); Empty();
  }
}

void OwnershipAndRetry() {
  Reset();
  auto encoder = Encoder(16, 12);
  std::vector<std::vector<uint8_t>> expected;
  std::vector<EncodedPacket> out;
  state.hold = true;
  for (int i = 0; i < 3; ++i) {
    auto frame = Frame(16, 12, i, 2, i + 99);
    expected.push_back(Oracle(frame, 16, 12));
    state.expected = &expected.back();
    Check(encoder->Encode(frame, false, &out), "MFT may retain input after Encode returns");
    std::fill(frame.data.begin(), frame.data.end(), 0);
    Check(latest->retained_count == static_cast<size_t>(i + 1), "one independent retained sample per input");
    for (int k = 0; k <= i; ++k) {
      auto* buffer = latest->retained[k]->buffer;
      Check(buffer->length == expected[k].size() &&
            std::memcmp(buffer->bytes, expected[k].data(), expected[k].size()) == 0,
            "retained sample survives source mutation and later inputs");
      if (k < i) Check(buffer != latest->retained[i]->buffer, "input samples never reuse a live buffer");
    }
  }
  Check(encoder->Drain(&out), "drain succeeds with MFT-owned samples still alive");
  Check(state.messages[MFT_MESSAGE_NOTIFY_END_OF_STREAM] == 1 &&
        state.messages[MFT_MESSAGE_COMMAND_DRAIN] == 1, "drain command order retains existing messages");
  encoder.reset(); Empty();

  for (int rejected = 1; rejected <= 2; ++rejected) {
    Reset(); encoder = Encoder(8, 6);
    auto frame = Frame(8, 6, 1, 1, 9);
    auto bytes = Oracle(frame, 8, 6); state.expected = &bytes;
    state.reject_inputs = rejected;
    Check(encoder->Encode(frame, true, &out) == (rejected == 1), "NOTACCEPTING drains then retries once");
    Check(state.inputs == 2 && state.first_input == state.last_input, "retry uses the same completed sample");
    Check(state.forced_keys == 1, "retry does not duplicate the key-frame request");
    encoder.reset(); Empty();
  }
  Reset(); encoder = Encoder(8, 6);
  auto frame = Frame(8, 6, 0, 0, 9);
  state.reject_inputs = 1; state.output_error = true;
  Check(!encoder->Encode(frame, false, &out), "failed draining rejects pending input");
  Check(state.inputs == 1 && state.accepted == 0, "failed drain prevents resubmission");
  encoder.reset(); Empty();
}

void OutputCases() {
  for (bool provides : {true, false}) {
    Reset(); auto encoder = Encoder(8, 6);
    auto frame = Frame(8, 6, 0, 0, 7);
    std::vector<EncodedPacket> out;
    state.provides = provides; state.emit = true;
    Check(encoder->Encode(frame, true, &out), "output path succeeds for both allocation modes");
    Check(out.size() == 2, "SPS/PPS and picture produce their original packets");
    if (out.size() == 2) {
      Check(out[0].data == std::vector<uint8_t>({0,0,0,1,0x67,0x42,0x80,0,0,0,1,0x68,0x11}) &&
            out[0].config && out[0].time_us == 12345, "configuration bytes and PTS preserved");
      Check(out[1].data == std::vector<uint8_t>({0,0,0,1,0x65,0x33,0x44}) &&
            out[1].key && !out[1].config && out[1].time_us == 12345,
            "IDR classification, AUD removal, Annex B and PTS preserved");
    }
    state.emit = true; state.next_idr = false; state.clean = true;
    Check(encoder->Encode(frame, false, &out), "next clean non-IDR output succeeds");
    Check(out.size() == 3 && out.back().key && out.back().data[4] == 0x61,
          "configuration emitted once and clean-point metadata preserves key flag");
    Check(state.events_released == 2, "output event collections released");
    encoder.reset(); Empty();
  }
}

void FailuresAndBounds() {
  for (auto fault : {Fault::CreateBuffer, Fault::Lock, Fault::ShortBuffer, Fault::NullBuffer,
                     Fault::Length, Fault::Unlock, Fault::CreateSample, Fault::NullSample,
                     Fault::AddBuffer, Fault::Time, Fault::Duration}) {
    Reset(); auto encoder = Encoder(8, 6);
    auto frame = Frame(8, 6, 2, 2, 4);
    std::vector<EncodedPacket> out;
    state.fault = fault;
    Check(!encoder->Encode(frame, true, &out), "sample construction failure returns false");
    Check(state.inputs == 0 && state.forced_keys == 0 && out.empty(),
          "failed sample is neither submitted nor reported as encoded");
    Check(state.buffers == 0 && state.samples == 0, "sample construction failure immediately retires its owners");
    encoder.reset(); Empty();

    Reset(); state.fault = fault;
    const uint8_t audio[] = {1, 2, 3, 4};
    IMFSample* sample = gesher::media::NewSample(audio, sizeof(audio), 5, 6);
    Check(sample == nullptr, "shared AAC sample-copy helper handles the same failures");
    if (sample) sample->Release();
    Empty();
  }
  for (int invalid = 0; invalid < 13; ++invalid) {
    Reset(); auto encoder = Encoder(8, 6);
    auto frame = Frame(8, 6, 2, 2, 5);
    switch (invalid) {
      case 0: frame.crop_left = -1; break;
      case 1: frame.crop_top = -1; break;
      case 2: frame.crop_left = frame.width; break;
      case 3: frame.crop_top = frame.height; break;
      case 4: frame.stride_y = frame.width - 1; break;
      case 5: frame.stride_uv = (frame.width - 1) / 2; break;
      case 6: frame.data.pop_back(); break;
      case 7: frame.stride_y = -1; break;
      case 8: frame.height = INT32_MAX; break;
      case 9: frame.width = 0; break;
      case 10: frame.time_us = INT64_MAX / 10 + 1; break;
      case 11: frame.time_us = INT64_MIN / 10 - 1; break;
      case 12: frame.data.clear(); break;
    }
    std::vector<EncodedPacket> out;
    Check(!encoder->Encode(frame, false, &out), "invalid input layout or unrepresentable time rejected");
    Check(state.create_buffers == 0 && state.inputs == 0, "invalid frame rejected before allocation or input");
    encoder.reset(); Empty();
  }
  for (auto [w, h] : {std::pair{0, 2}, {-2, 2}, {3, 2}, {2, 3}, {65536, 65536}, {INT32_MAX - 1, 2}}) {
    Reset(); VideoEncoderParams p; p.width = w; p.height = h;
    std::string error;
    auto encoder = gesher::media::CreateMfH264Encoder(p, &error);
    Check(!encoder && !error.empty() && state.transforms == 0,
          "unrepresentable sample/output dimensions fail before MFT activation");
    encoder.reset(); Empty();
  }
  Reset();
  const uint8_t byte = 2;
  auto sample = gesher::media::NewSample(&byte, size_t{1} << 32, 0, 1);
  Check(sample == nullptr && state.create_buffers == 0, "DWORD input-size overflow rejected without truncation");
  sample = gesher::media::NewSample(&byte, 1, 0, INT64_MAX / 10 + 1);
  Check(sample == nullptr && state.create_buffers == 0, "duration overflow rejected before allocation");
  Empty();
}

void AudioCases() {
  Reset();
  gesher::media::AudioEncoderParams p;
  p.channels = 2; p.sample_rate = 48000;
  std::string error;
  auto encoder = gesher::media::CreateMfAacEncoder(p, &error);
  Check(encoder != nullptr, "AAC still initializes with the shared sample helper");
  const std::array<int16_t, 8> audio{-32768, 32767, 0, 1, -1, 42, -42, 120};
  std::vector<uint8_t> expected(sizeof(audio));
  std::memcpy(expected.data(), audio.data(), sizeof(audio));
  state.expected = &expected;
  std::vector<EncodedPacket> out;
  Check(encoder->Encode(audio.data(), 4, -99, &out), "AAC copies its original PCM payload");
  Check(state.seen_time == -990 && state.seen_duration == 830, "AAC signed time and rounded duration preserved");
  Check(out.size() == 1 && out.front().config && out.front().time_us == -99,
        "AAC configuration still precedes data");
  encoder.reset(); Empty();
  Reset();
  IMFSample* empty = gesher::media::NewSample(nullptr, 0, 0, 0);
  Check(empty != nullptr && static_cast<Sample*>(empty)->buffer->length == 0,
        "zero-byte helper input retains an owned zero-length sample");
  if (empty) empty->Release();
  Empty();
}

void Measure(bool enforce) {
  Reset();
  constexpr int w = 1920, h = 1080, frames = 120;
  constexpr size_t payload = size_t{w} * h * 3 / 2;
  auto encoder = Encoder(w, h);
  auto frame = Frame(w, h, 2, 2, 77);
  auto expected = Oracle(frame, w, h);
  state.expected = &expected;
  std::vector<EncodedPacket> out;
  counts = {};
  counts.active = true;
  for (int i = 0; i < frames; ++i) Check(encoder->Encode(frame, false, &out), "measured input encodes");
  counts.active = false;
  std::printf("INPUT_METRICS frames=%d width=%d height=%d allocations=%zu allocated_bytes=%zu "
              "peak_payload_bytes=%zu memcpy_calls=%zu memcpy_bytes=%zu\n",
              frames, w, h, counts.allocations, counts.bytes, counts.peak, counts.copies, counts.copy_bytes);
  Check(counts.live == 0, "measured input allocations fully released");
  if (enforce) {
    Check(counts.allocations == frames, "one payload allocation per frame budget");
    Check(counts.bytes == frames * payload, "one payload size allocated per frame budget");
    Check(counts.peak == payload, "one live packed frame payload budget");
    Check(counts.copy_bytes == frames * payload, "one crop-to-input copy per frame budget");
  }
  encoder.reset(); Empty();
}
}

int main(int argc, char** argv) {
  using namespace fixture;
  const bool measure = argc > 1 && std::strcmp(argv[1], "measure") == 0;
  const bool valid_only = measure || (argc > 1 && std::strcmp(argv[1], "valid-gate") == 0);
  ValidCases();
  OwnershipAndRetry();
  OutputCases();
  AudioCases();
  if (!valid_only) FailuresAndBounds();
  Measure(!measure);
  std::printf("MF encoder input: %zu checks, %zu failures\n", checks, failures);
  return failures ? 1 : 0;
}
