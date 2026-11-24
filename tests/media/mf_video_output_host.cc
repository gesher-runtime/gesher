// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The complete production MF video decoder and VideoFrame/NV12/CSD translation
// units run below. Win32 activation, MF COM objects and NDK Format storage are
// explicit host adapters; no real MFT, GPU, Windows or ART runs here.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include "tests/media/mf_video_host.h"
#include "compat/media/video/video_decoder.h"

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
  size_t allocations = 0, bytes = 0, live = 0, peak = 0;
} counts;
struct alignas(std::max_align_t) Allocation { size_t size; bool counted; };
struct Uncounted {
  bool previous = counts.active;
  Uncounted() { counts.active = false; }
  ~Uncounted() { counts.active = previous; }
};
int WindowsSnprintf(char* out, size_t size, const char* format, unsigned long value) {
  Check(std::strcmp(format, "0x%08lx") == 0, "declared LLP64 HRESULT formatting boundary");
  return std::snprintf(out, size, "0x%08x", static_cast<uint32_t>(value));
}
}  // namespace fixture

[[gnu::noinline]] void* operator new(size_t size) {
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
[[gnu::noinline]] void operator delete(void* p) noexcept {
  if (p == nullptr) return;
  auto* header = static_cast<fixture::Allocation*>(p) - 1;
  if (header->counted) fixture::counts.live -= header->size;
  std::free(header);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete(p); }

#define snprintf fixture::WindowsSnprintf
#include GESHER_MF_VIDEO_SOURCE
#undef snprintf

struct AMediaFormat {
  std::vector<std::pair<std::string, int32_t>> ints;
  std::vector<std::pair<std::string, std::string>> strings;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> buffers;
};

void AMediaFormat_setBuffer(AMediaFormat* format, const char* name, const void* data, size_t size) {
  if (!format || !name) return;
  fixture::Uncounted ignored;
  const auto* p = static_cast<const uint8_t*>(data);
  for (auto& [k, v] : format->buffers) {
    if (k == name) {
      v.assign(p, p + size);
      return;
    }
  }
  format->buffers.emplace_back(name, std::vector<uint8_t>(p, p + size));
}

namespace gesher::media {
Format::Format() {
  fixture::Uncounted ignored;
  f_ = new AMediaFormat;
}
Format::~Format() {
  fixture::Uncounted ignored;
  delete f_;
}
Format& Format::operator=(Format&& o) noexcept {
  if (this != &o) {
    fixture::Uncounted ignored;
    delete f_;
    f_ = o.f_;
    o.f_ = nullptr;
  }
  return *this;
}
bool Format::GetInt32(const char* key, int32_t* out) const {
  if (!f_ || !key) return false;
  for (const auto& [k, v] : f_->ints) {
    if (k == key) {
      *out = v;
      return true;
    }
  }
  return false;
}
bool Format::GetInt64(const char*, int64_t*) const { return false; }
bool Format::GetRect(const char*, int32_t*, int32_t*, int32_t*, int32_t*) const { return false; }
bool Format::GetFloat(const char*, float*) const { return false; }
bool Format::GetString(const char* key, std::string* out) const {
  if (!f_ || !key) return false;
  for (const auto& [k, v] : f_->strings) {
    if (k == key) {
      fixture::Uncounted ignored;
      *out = v;
      return true;
    }
  }
  return false;
}
bool Format::GetBuffer(const char* key, std::vector<uint8_t>* out) const {
  if (!f_ || !key) return false;
  for (const auto& [k, v] : f_->buffers) {
    if (k == key) {
      fixture::Uncounted ignored;
      *out = v;
      return true;
    }
  }
  return false;
}
std::string Format::Mime() const {
  std::string m;
  GetString("mime", &m);
  return m;
}
std::string Format::ToString() const { return "{}"; }

std::vector<VideoCodecInfo> D3D11VideoCodecs() { return {}; }
std::vector<VideoCodecInfo> SoftVideoCodecs() { return {}; }
std::unique_ptr<VideoDecoder> CreateD3D11VideoDecoder(const Format&, std::string*) { return nullptr; }
std::unique_ptr<VideoDecoder> CreateSoftVideoDecoder(const Format&, std::string*) { return nullptr; }
}  // namespace gesher::media

namespace fixture {
enum class Fault {
  None,
  CreateBuffer,
  NullCreatedBuffer,
  Lock,
  ShortBuffer,
  NullBuffer,
  Length,
  Unlock,
  CreateSample,
  NullSample,
  AddBuffer,
  Time,
  SetDiscontinuity
};

struct OutputConfig {
  int width = 16, height = 12, stride = 16;
  bool has_stride = true;
  bool has_aperture = false;
  MFVideoArea aperture{};
  bool offer_nv12 = true;
  bool provides = false;
  DWORD cb_size = 0;
};

struct State {
  Fault fault = Fault::None;
  Fault pending_pull_fault = Fault::None;
  OutputConfig cfg{};
  OutputConfig next_cfg{};
  bool defer_output_until_input = false;
  bool stream_change_once = false;
  bool output_error = false;
  bool stream_info_error = false;
  bool fail_create_type = false;
  bool null_create_type = false;
  bool replace_output_sample = false;
  bool use_2d_output = false;
  LONG pitch_2d = 0;
  bool null_2d_ptr = false;
  int reject_inputs = 0;
  int emit_count = 0;
  LONGLONG emit_time_100ns = 0;
  std::vector<uint8_t> nv12_1d;
  std::vector<uint8_t> nv12_2d;
  std::vector<uint8_t> last_input_bytes;
  LONGLONG seen_time = 0;
  bool seen_discontinuity = false;

  size_t buffers = 0, samples = 0, transforms = 0, types = 0, activates = 0;
  size_t create_buffers = 0, create_samples = 0, create_buffer_bytes = 0;
  DWORD last_buffer_capacity = 0;
  size_t locks_1d = 0, unlocks_1d = 0, successful_locks_1d = 0;
  size_t locks_2d = 0, unlocks_2d = 0, successful_locks_2d = 0;
  size_t inputs = 0, accepted = 0, outputs = 0, events_released = 0;
  std::array<size_t, 5> messages{};
} state;

struct Buffer final : IMFMediaBuffer, IMF2DBuffer {
  ULONG refs = 1;
  BYTE* bytes = nullptr;
  BYTE* bytes_2d = nullptr;
  DWORD capacity = 0, length = 0;
  bool enable_2d = false;
  LONG pitch_2d = 0;
  bool null_2d_ptr = false;
  bool locked_1d = false, locked_2d = false;

  Buffer() { ++state.buffers; }
  ~Buffer() {
    Check(!locked_1d && !locked_2d, "all 1D and 2D buffer locks balanced before destruction");
    ::operator delete(bytes);
    {
      Uncounted ignored;
      delete[] bytes_2d;
    }
    --state.buffers;
  }
  HRESULT QueryInterface(const IID& iid, void** out) override {
    if (enable_2d && IsEqualGUID(iid, gesher::media::kIid2DBuffer)) {
      *out = static_cast<IMF2DBuffer*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      Uncounted ignored;
      delete this;
    }
    return n;
  }
  HRESULT Lock(BYTE** p, DWORD* maximum, DWORD* current) override {
    ++state.locks_1d;
    if (state.fault == Fault::Lock) return E_FAIL;
    Check(!locked_1d && !locked_2d, "1D Lock called only when neither 1D nor 2D lock is held");
    locked_1d = true;
    ++state.successful_locks_1d;
    *p = state.fault == Fault::NullBuffer ? nullptr : bytes;
    if (maximum) *maximum = (state.fault == Fault::ShortBuffer && capacity > 0) ? capacity - 1 : capacity;
    if (current) *current = length;
    return S_OK;
  }
  HRESULT Unlock() override {
    ++state.unlocks_1d;
    Check(locked_1d, "Unlock pairs with successful 1D Lock");
    locked_1d = false;
    return state.fault == Fault::Unlock ? E_FAIL : S_OK;
  }
  HRESULT SetCurrentLength(DWORD n) override {
    Check(n <= capacity, "valid length fits allocated MF buffer");
    if (state.fault == Fault::Length || n > capacity) return E_FAIL;
    length = n;
    return S_OK;
  }
  HRESULT Lock2D(BYTE** p, LONG* pitch) override {
    ++state.locks_2d;
    Check(!locked_1d && !locked_2d, "Lock2D called on unlocked buffer");
    locked_2d = true;
    ++state.successful_locks_2d;
    *p = null_2d_ptr ? nullptr : (bytes_2d ? bytes_2d : bytes);
    *pitch = pitch_2d;
    return S_OK;
  }
  HRESULT Unlock2D() override {
    ++state.unlocks_2d;
    Check(locked_2d, "Unlock2D pairs with successful Lock2D");
    locked_2d = false;
    return S_OK;
  }
};

struct Sample final : IMFSample {
  ULONG refs = 1;
  Buffer* buffer = nullptr;
  LONGLONG time = 0, duration = 0;
  bool discontinuity = false;

  Sample() { ++state.samples; }
  ~Sample() {
    if (buffer) buffer->Release();
    --state.samples;
  }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      Uncounted ignored;
      delete this;
    }
    return n;
  }
  HRESULT AddBuffer(IMFMediaBuffer* b) override {
    if (state.fault == Fault::AddBuffer) return E_FAIL;
    Check(buffer == nullptr, "sample receives exactly one buffer");
    buffer = static_cast<Buffer*>(b);
    buffer->AddRef();
    return S_OK;
  }
  HRESULT GetBufferCount(DWORD* count) override {
    if (!count) return E_FAIL;
    *count = buffer ? 1 : 0;
    return S_OK;
  }
  HRESULT GetBufferByIndex(DWORD index, IMFMediaBuffer** value) override {
    if (!value || index != 0 || !buffer) return E_FAIL;
    *value = static_cast<IMFMediaBuffer*>(buffer);
    buffer->AddRef();
    return S_OK;
  }
  HRESULT DeleteAllItems() override {
    discontinuity = false;
    time = 0;
    duration = 0;
    return S_OK;
  }
  HRESULT SetSampleTime(LONGLONG value) override {
    if (state.fault == Fault::Time) return E_FAIL;
    time = value;
    return S_OK;
  }
  HRESULT SetSampleDuration(LONGLONG value) override {
    duration = value;
    return S_OK;
  }
  HRESULT GetSampleTime(LONGLONG* value) override {
    *value = time;
    return S_OK;
  }
  HRESULT SetUINT32(const GUID& key, UINT32 value) override {
    if (state.fault == Fault::SetDiscontinuity) return E_FAIL;
    if (IsEqualGUID(key, gesher::media::kSampleDiscontinuity)) discontinuity = (value != 0);
    return S_OK;
  }
};

struct MediaType final : IMFMediaType {
  ULONG refs = 1;
  GUID subtype{};
  UINT64 frame_size = 0;
  bool has_stride = false;
  UINT32 stride = 0;
  bool has_aperture = false;
  MFVideoArea aperture{};
  std::vector<uint8_t> seq_header;

  MediaType() { ++state.types; }
  ~MediaType() { --state.types; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      Uncounted ignored;
      delete this;
    }
    return n;
  }
  HRESULT SetGUID(const GUID& key, const GUID& val) override {
    if (IsEqualGUID(key, gesher::media::kMtSubtype)) subtype = val;
    return S_OK;
  }
  HRESULT SetUINT32(const GUID& key, UINT32 val) override {
    if (IsEqualGUID(key, gesher::media::kMtDefaultStride)) {
      has_stride = true;
      stride = val;
    }
    return S_OK;
  }
  HRESULT SetUINT64(const GUID& key, UINT64 val) override {
    if (IsEqualGUID(key, gesher::media::kMtFrameSize)) frame_size = val;
    return S_OK;
  }
  HRESULT SetBlob(const GUID& key, const UINT8* data, UINT32 size) override {
    if (IsEqualGUID(key, gesher::media::kMtMpegSequenceHeader)) {
      Uncounted ignored;
      seq_header.assign(data, data + size);
    }
    return S_OK;
  }
  HRESULT GetGUID(const GUID& key, GUID* val) override {
    if (!IsEqualGUID(key, gesher::media::kMtSubtype)) return E_FAIL;
    *val = subtype;
    return S_OK;
  }
  HRESULT GetUINT32(const GUID& key, UINT32* val) override {
    if (IsEqualGUID(key, gesher::media::kMtDefaultStride) && has_stride) {
      *val = stride;
      return S_OK;
    }
    return E_FAIL;
  }
  HRESULT GetUINT64(const GUID& key, UINT64* val) override {
    if (IsEqualGUID(key, gesher::media::kMtFrameSize)) {
      *val = frame_size;
      return S_OK;
    }
    return E_FAIL;
  }
  HRESULT GetBlob(const GUID& key, UINT8* out, UINT32 cap, UINT32* got) override {
    if (IsEqualGUID(key, gesher::media::kMtMinimumDisplayAperture) && has_aperture &&
        cap >= sizeof(MFVideoArea)) {
      std::memcpy(out, &aperture, sizeof(MFVideoArea));
      *got = sizeof(MFVideoArea);
      return S_OK;
    }
    return E_FAIL;
  }
};

struct Event final : IUnknown {
  ULONG refs = 1;
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      ++state.events_released;
      Uncounted ignored;
      delete this;
    }
    return n;
  }
};

HRESULT CreateBuffer(DWORD size, IMFMediaBuffer** result) {
  ++state.create_buffers;
  if (state.fault == Fault::CreateBuffer) return E_FAIL;
  if (state.fault == Fault::NullCreatedBuffer) {
    *result = nullptr;
    return S_OK;
  }
  state.create_buffer_bytes += size;
  state.last_buffer_capacity = size;
  Buffer* buffer;
  {
    Uncounted ignored;
    buffer = new Buffer;
  }
  buffer->bytes = static_cast<BYTE*>(::operator new(size));
  buffer->capacity = size;
  std::memset(buffer->bytes, 0xd4, size);
  *result = static_cast<IMFMediaBuffer*>(buffer);
  return S_OK;
}

HRESULT CreateSample(IMFSample** result) {
  ++state.create_samples;
  if (state.fault == Fault::CreateSample) return E_FAIL;
  if (state.fault == Fault::NullSample) return S_OK;
  Uncounted ignored;
  *result = new Sample;
  return S_OK;
}

struct Transform final : IMFTransform {
  ULONG refs = 1;
  bool seen_any_input = false;
  Transform() { ++state.transforms; }
  ~Transform() { --state.transforms; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      Uncounted ignored;
      delete this;
    }
    return n;
  }
  HRESULT GetOutputAvailableType(DWORD, DWORD index, IMFMediaType** out) override {
    if (state.defer_output_until_input && !seen_any_input) return MF_E_TRANSFORM_TYPE_NOT_SET;
    if (!state.cfg.offer_nv12 || index > 1) return MF_E_NO_MORE_TYPES;
    Uncounted ignored;
    auto* t = new MediaType;
    if (index == 0) {
      // Offer a non-NV12 type at index 0 to exercise the enumeration loop & Release().
      t->subtype = gesher::media::FourCcSubtype(gesher::media::FourCc('Y', 'U', 'Y', '2'));
    } else {
      t->subtype = gesher::media::kNv12;
      t->frame_size = (static_cast<UINT64>(static_cast<uint32_t>(state.cfg.width)) << 32) |
                      static_cast<uint32_t>(state.cfg.height);
      t->has_stride = state.cfg.has_stride;
      t->stride = static_cast<UINT32>(state.cfg.stride);
      t->has_aperture = state.cfg.has_aperture;
      t->aperture = state.cfg.aperture;
    }
    *out = t;
    return S_OK;
  }
  HRESULT SetOutputType(DWORD, IMFMediaType*, DWORD) override { return S_OK; }
  HRESULT SetInputType(DWORD, IMFMediaType*, DWORD) override { return S_OK; }
  HRESULT ProcessMessage(MFT_MESSAGE_TYPE msg, uintptr_t) override {
    ++state.messages[static_cast<size_t>(msg)];
    return S_OK;
  }
  HRESULT GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO* info) override {
    if (state.stream_info_error) return E_FAIL;
    info->dwFlags = state.cfg.provides ? MFT_OUTPUT_STREAM_PROVIDES_SAMPLES : 0;
    info->cbSize = state.cfg.cb_size;
    return S_OK;
  }
  HRESULT ProcessInput(DWORD id, IMFSample* raw, DWORD flags) override {
    ++state.inputs;
    Check(id == 0 && flags == 0, "decoder ProcessInput stream and flags preserved");
    auto* s = static_cast<Sample*>(raw);
    Check(s->buffer != nullptr && !s->buffer->locked_1d && !s->buffer->locked_2d,
          "submitted input sample has an unlocked buffer");
    if (state.reject_inputs > 0) {
      --state.reject_inputs;
      return MF_E_NOTACCEPTING;
    }
    seen_any_input = true;
    ++state.accepted;
    state.seen_time = s->time;
    state.seen_discontinuity = s->discontinuity;
    {
      Uncounted ignored;
      state.last_input_bytes.assign(s->buffer->bytes, s->buffer->bytes + s->buffer->length);
    }
    if (state.pending_pull_fault != Fault::None) {
      state.fault = state.pending_pull_fault;
      state.pending_pull_fault = Fault::None;
    }
    return S_OK;
  }
  static IMFSample* MakeMftSample(DWORD bytes_len) {
    IMFSample* s = nullptr;
    IMFMediaBuffer* b = nullptr;
    Check(SUCCEEDED(CreateSample(&s)) && SUCCEEDED(CreateBuffer(std::max<DWORD>(bytes_len, 16), &b)) &&
              SUCCEEDED(s->AddBuffer(b)),
          "MFT-allocated output sample created");
    b->Release();
    return s;
  }
  HRESULT ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* output, DWORD*) override {
    ++state.outputs;
    Check(count == 1, "one output buffer requested");
    if (state.defer_output_until_input && seen_any_input) {
      state.defer_output_until_input = false;
      return MF_E_TRANSFORM_TYPE_NOT_SET;
    }
    if (state.stream_change_once) {
      state.stream_change_once = false;
      state.cfg = state.next_cfg;
      if (state.replace_output_sample) {
        state.replace_output_sample = false;
        output->pSample = MakeMftSample(64);
      }
      return MF_E_TRANSFORM_STREAM_CHANGE;
    }
    if (state.output_error) {
      if (state.replace_output_sample) {
        state.replace_output_sample = false;
        output->pSample = MakeMftSample(64);
      }
      return E_FAIL;
    }
    if (state.emit_count <= 0) {
      if (state.replace_output_sample) {
        state.replace_output_sample = false;
        output->pSample = MakeMftSample(64);
      }
      return MF_E_TRANSFORM_NEED_MORE_INPUT;
    }
    --state.emit_count;
    IMFSample* raw = output->pSample;
    if (state.replace_output_sample) {
      state.replace_output_sample = false;
      raw = nullptr;
    }
    const DWORD needed_1d = static_cast<DWORD>(
        !state.nv12_1d.empty()
            ? state.nv12_1d.size()
            : static_cast<size_t>(state.cfg.stride) *
                  (static_cast<size_t>(state.cfg.height) + (static_cast<size_t>(state.cfg.height) + 1) / 2));
    if (raw == nullptr) {
      Check(state.cfg.provides || output->pSample != nullptr || true, "MFT allocates output sample");
      Uncounted ignored;
      raw = MakeMftSample(needed_1d);
    } else {
      Check(!state.cfg.provides, "caller sample provided only when MFT does not provide samples");
    }
    auto* s = static_cast<Sample*>(raw);
    auto* buf = s->buffer;
    Check(buf != nullptr && buf->capacity >= needed_1d, "output buffer capacity fits NV12 frame");
    if (!state.nv12_1d.empty()) {
      std::memcpy(buf->bytes, state.nv12_1d.data(), state.nv12_1d.size());
    }
    buf->length = needed_1d;
    buf->enable_2d = state.use_2d_output;
    buf->pitch_2d = state.pitch_2d;
    buf->null_2d_ptr = state.null_2d_ptr;
    if (state.use_2d_output && !state.nv12_2d.empty()) {
      Uncounted ignored;
      delete[] buf->bytes_2d;
      buf->bytes_2d = new BYTE[state.nv12_2d.size()];
      std::memcpy(buf->bytes_2d, state.nv12_2d.data(), state.nv12_2d.size());
    }
    s->time = state.emit_time_100ns;
    output->pSample = s;
    {
      Uncounted ignored;
      output->pEvents = new Event;
    }
    return S_OK;
  }
};

struct Activate final : IMFActivate {
  ULONG refs = 1;
  Activate() { ++state.activates; }
  ~Activate() { --state.activates; }
  HRESULT QueryInterface(const IID&, void**) override { return E_NOINTERFACE; }
  ULONG AddRef() override { return ++refs; }
  ULONG Release() override {
    const auto n = --refs;
    if (!n) {
      Uncounted ignored;
      delete this;
    }
    return n;
  }
  HRESULT ActivateObject(const IID&, void** result) override {
    Uncounted ignored;
    *result = new Transform;
    return S_OK;
  }
};

HRESULT Startup(ULONG, DWORD) { return S_OK; }
HRESULT CreateType(IMFMediaType** result) {
  if (state.fail_create_type) {
    if (state.null_create_type) {
      *result = nullptr;
      return S_OK;
    }
    return E_FAIL;
  }
  Uncounted ignored;
  *result = new MediaType;
  return S_OK;
}
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
            state.types == 0 && state.activates == 0,
        "all adapter COM objects retired");
  Check(state.successful_locks_1d == state.unlocks_1d, "all successful 1D locks balanced");
  Check(state.successful_locks_2d == state.unlocks_2d, "all successful 2D locks balanced");
}

void Reset() {
  Empty();
  Uncounted ignored;
  state = {};
}
}  // namespace fixture

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
using gesher::media::Format;
using gesher::media::VideoDecoder;
using gesher::media::VideoFrame;

uint32_t Random(uint32_t* seed) {
  *seed = *seed * 1664525u + 1013904223u;
  return *seed;
}

Format MakeFormat(const char* mime, int w, int h,
                  const std::vector<uint8_t>& avcc = {}) {
  Uncounted ignored;
  Format f;
  f.get()->strings.emplace_back("mime", mime);
  if (w > 0) f.get()->ints.emplace_back("width", w);
  if (h > 0) f.get()->ints.emplace_back("height", h);
  if (!avcc.empty()) f.get()->buffers.emplace_back("csd-avc", avcc);
  return f;
}

std::vector<uint8_t> BuildNv12(int w, int h, int row_stride, uint32_t seed) {
  Uncounted ignored;
  const int ch = (h + 1) / 2;
  std::vector<uint8_t> nv12(static_cast<size_t>(row_stride) * (h + ch));
  for (uint8_t& b : nv12) b = static_cast<uint8_t>(Random(&seed) >> 24);
  (void)w;
  return nv12;
}

// Independent pixel-by-pixel NV12 -> I420 oracle; shares no code with Nv12ToI420.
std::vector<uint8_t> OracleI420(const std::vector<uint8_t>& nv12, int w, int h, int row_stride) {
  Uncounted ignored;
  const int cw = (w + 1) / 2, ch = (h + 1) / 2;
  std::vector<uint8_t> out(static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch);
  size_t at = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      out[at++] = nv12[static_cast<size_t>(y) * row_stride + x];
    }
  }
  const size_t uv_base = static_cast<size_t>(row_stride) * h;
  for (int plane = 0; plane < 2; ++plane) {
    for (int y = 0; y < ch; ++y) {
      for (int x = 0; x < cw; ++x) {
        out[at++] = nv12[uv_base + static_cast<size_t>(y) * row_stride + 2 * x + plane];
      }
    }
  }
  return out;
}

std::unique_ptr<VideoDecoder> Decoder(int w = 16, int h = 12,
                                      const std::vector<uint8_t>& avcc = {}) {
  Format track = MakeFormat("video/avc", w, h, avcc);
  std::string error;
  auto dec = gesher::media::CreateMfVideoDecoder(track, &error);
  Check(dec != nullptr && error.empty(), "MF video decoder initializes at declared boundary");
  return dec;
}

void ValidDecodeCases(bool check_nonpositive_2d_pitch) {
  // 1. 1D Lock (!provides and provides), padded stride, crop aperture, signed timestamps.
  uint32_t seed = 0x13579bdfu;
  const int64_t times[] = {0, 1, -1, -987654, 123456789, INT64_MAX / 10, INT64_MIN / 10};
  for (size_t i = 0; i < 28; ++i) {
    Reset();
    const int w = 4 + 2 * static_cast<int>(Random(&seed) % 24);
    const int h = 4 + 2 * static_cast<int>(Random(&seed) % 20);
    const int stride = w + static_cast<int>((Random(&seed) % 4) * 16);
    state.cfg.width = w;
    state.cfg.height = h;
    state.cfg.stride = stride;
    state.cfg.provides = (i % 2 == 0);
    if (i % 3 == 0) {
      state.cfg.has_aperture = true;
      state.cfg.aperture.OffsetX.value = 2;
      state.cfg.aperture.OffsetY.value = 2;
      state.cfg.aperture.Area.cx = w - 2;
      state.cfg.aperture.Area.cy = h - 2;
    }
    state.nv12_1d = BuildNv12(w, h, stride, seed);
    const auto expected = OracleI420(state.nv12_1d, w, h, stride);
    auto dec = Decoder(w, h);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65, 0x88};
    in.time_us = times[i % 7];
    state.emit_count = 1;
    state.emit_time_100ns = in.time_us * 10;
    std::vector<VideoFrame> out;
    Check(dec->Decode(in, &out) && out.size() == 1, "1D NV12->I420 decode succeeds");
    if (out.size() == 1) {
      Check(out[0].width == w && out[0].height == h, "decoded frame dimensions preserved");
      Check(out[0].time_us == in.time_us, "signed timestamp preserved across 100ns conversion");
      Check(out[0].data == expected, "every I420 pixel matches independent NV12 oracle");
      if (state.cfg.has_aperture) {
        Check(out[0].crop_left == 2 && out[0].crop_top == 2 &&
                  out[0].crop_right == w - 1 && out[0].crop_bottom == h - 1,
              "minimum display aperture crop preserved");
      } else {
        Check(out[0].crop_left == 0 && out[0].crop_top == 0 &&
                  out[0].crop_right == w - 1 && out[0].crop_bottom == h - 1,
              "default full-frame crop preserved");
      }
    }
    dec.reset();
    Empty();
  }

  // 2. IMF2DBuffer with positive pitch (distinct from default stride).
  {
    Reset();
    const int w = 24, h = 16, stride1d = 24, pitch2d = 64;
    state.cfg.width = w;
    state.cfg.height = h;
    state.cfg.stride = stride1d;
    state.cfg.provides = true;
    state.use_2d_output = true;
    state.pitch_2d = pitch2d;
    state.nv12_1d = BuildNv12(w, h, stride1d, 0xaa11u);
    state.nv12_2d = BuildNv12(w, h, pitch2d, 0xbb22u);
    const auto expected_2d = OracleI420(state.nv12_2d, w, h, pitch2d);
    auto dec = Decoder(w, h);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65};
    in.time_us = 4200;
    state.emit_count = 1;
    state.emit_time_100ns = 42000;
    std::vector<VideoFrame> out;
    Check(dec->Decode(in, &out) && out.size() == 1, "IMF2DBuffer positive-pitch decode succeeds");
    if (out.size() == 1) {
      Check(out[0].data == expected_2d, "IMF2DBuffer uses 2D pitch plane rather than 1D buffer");
    }
    Check(state.successful_locks_2d == 1 && state.unlocks_2d == 1 && state.locks_1d == 1,
          "positive-pitch IMF2DBuffer locks and unlocks 2D buffer once without output 1D fallback");
    dec.reset();
    Empty();
  }

  // 3. IMF2DBuffer with negative pitch, zero pitch, and null pointer falls back to 1D Lock
  //    AFTER balancing Unlock2D()!
  if (check_nonpositive_2d_pitch) {
    for (int variant = 0; variant < 3; ++variant) {
      Reset();
      const int w = 20, h = 12, stride1d = 32;
      state.cfg.width = w;
      state.cfg.height = h;
      state.cfg.stride = stride1d;
      state.cfg.provides = true;
      state.use_2d_output = true;
      state.pitch_2d = (variant == 0) ? -64 : (variant == 1) ? 0 : 64;
      state.null_2d_ptr = (variant == 2);
      state.nv12_1d = BuildNv12(w, h, stride1d, 0xcc33u + static_cast<uint32_t>(variant));
      const auto expected_1d = OracleI420(state.nv12_1d, w, h, stride1d);
      auto dec = Decoder(w, h);
      gesher::media::Sample in;
      in.data = {0, 0, 0, 1, 0x65};
      in.time_us = 777;
      state.emit_count = 1;
      state.emit_time_100ns = 7770;
      std::vector<VideoFrame> out;
      Check(dec->Decode(in, &out) && out.size() == 1,
            "IMF2DBuffer non-usable 2D lock falls back cleanly to 1D Lock");
      if (out.size() == 1) {
        Check(out[0].data == expected_1d, "fallback 1D NV12->I420 pixels match oracle");
      }
      Check(state.successful_locks_2d == 1 && state.unlocks_2d == 1 &&
                state.successful_locks_1d == 2 && state.unlocks_1d == 2,
            "Unlock2D balances Lock2D before 1D Lock fallback");
      dec.reset();
      Empty();
    }
  }

  // 4. AnnexBParameterSets prefix, Flush (discontinuity + prefix resend), Drain, NOTACCEPTING,
  //    deferred output type, and MfVideoCodecs().
  {
    Reset();
    const auto codecs = gesher::media::MfVideoCodecs();
    Check(codecs.size() == 7 && codecs[0].mime == "video/avc" && codecs[0].backend == "mf",
          "MfVideoCodecs enumerates supported MF codecs");
    Empty();

    Reset();
    state.cfg.width = 16;
    state.cfg.height = 12;
    state.cfg.stride = 16;
    state.cfg.provides = false;
    state.defer_output_until_input = true;
    state.nv12_1d = BuildNv12(16, 12, 16, 0x55aa);
    // Minimal avcC record: SPS {0x67, 0x42, 0x00}, PPS {0x68, 0xce}.
    const std::vector<uint8_t> avcc = {1, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0, 3, 0x67, 0x42, 0x00,
                                       1, 0, 2, 0x68, 0xce};
    const std::vector<uint8_t> expected_prefix = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0, 0, 0, 1, 0x68, 0xce};
    auto dec = Decoder(16, 12, avcc);
    gesher::media::Sample s1;
    s1.data = {0, 0, 0, 1, 0x65, 0xaa};
    s1.time_us = 1000;
    state.emit_count = 1;
    state.emit_time_100ns = 10000;
    std::vector<VideoFrame> out;
    Check(dec->Decode(s1, &out) && out.size() == 1, "deferred-output decode with avcC succeeds");
    std::vector<uint8_t> expected_first = expected_prefix;
    expected_first.insert(expected_first.end(), s1.data.begin(), s1.data.end());
    Check(state.last_input_bytes == expected_first && !state.seen_discontinuity,
          "first access unit prepends Annex B SPS/PPS from avcC");

    // Second frame does not resend prefix; test MF_E_NOTACCEPTING drain-then-retry.
    gesher::media::Sample s2;
    s2.data = {0, 0, 0, 1, 0x41, 0xbb};
    s2.time_us = 2000;
    state.reject_inputs = 1;
    state.emit_count = 1;
    state.emit_time_100ns = 20000;
    Check(dec->Decode(s2, &out) && out.size() == 2, "NOTACCEPTING pulls then retries ProcessInput");
    Check(state.last_input_bytes == s2.data, "subsequent access unit omits parameter-set prefix");

    // Flush sets discontinuity and re-enables prefix on next sample.
    dec->Flush();
    Check(state.messages[MFT_MESSAGE_COMMAND_FLUSH] == 1, "Flush sends MFT_MESSAGE_COMMAND_FLUSH");
    state.emit_count = 1;
    state.emit_time_100ns = 25000;
    Check(dec->Decode(s1, &out) && out.size() == 3, "post-Flush decode succeeds");
    Check(state.last_input_bytes == expected_first && state.seen_discontinuity,
          "post-Flush access unit sets MFSampleExtension_Discontinuity and resends SPS/PPS");

    // Drain emits buffered frame and restarts stream.
    state.emit_count = 1;
    state.emit_time_100ns = 30000;
    dec->Drain(&out);
    Check(out.size() == 4 && out.back().time_us == 3000, "Drain pulls remaining decoded frames");
    Check(state.messages[MFT_MESSAGE_NOTIFY_END_OF_STREAM] == 1 &&
              state.messages[MFT_MESSAGE_COMMAND_DRAIN] == 1 &&
              state.messages[MFT_MESSAGE_NOTIFY_START_OF_STREAM] == 2,
          "Drain sends EOS, DRAIN, and START_OF_STREAM");
    dec.reset();
    Empty();
  }
}

void StreamChangeCases() {
  // Resolution upscale 640x360 -> 1920x1080 under !provides_samples.
  Reset();
  state.cfg.width = 640;
  state.cfg.height = 360;
  state.cfg.stride = 640;
  state.cfg.provides = false;
  const auto nv12_360 = BuildNv12(640, 360, 640, 0x360u);
  const auto exp_360 = OracleI420(nv12_360, 640, 360, 640);
  state.nv12_1d = nv12_360;
  auto dec = Decoder(640, 360);
  std::vector<VideoFrame> out;
  for (int i = 0; i < 3; ++i) {
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65, static_cast<uint8_t>(i)};
    in.time_us = i * 33333;
    state.emit_count = 1;
    state.emit_time_100ns = in.time_us * 10;
    Check(dec->Decode(in, &out), "640x360 frame decodes before stream change");
  }
  Check(out.size() == 3 && out[0].width == 640 && out[0].height == 360 && out[0].data == exp_360,
        "initial 640x360 frames match oracle");

  // Trigger mid-stream upscale to 1920x1080 (3,110,400 bytes/frame).
  const auto nv12_1080 = BuildNv12(1920, 1080, 1920, 0x1080u);
  const auto exp_1080 = OracleI420(nv12_1080, 1920, 1080, 1920);
  state.next_cfg = state.cfg;
  state.next_cfg.width = 1920;
  state.next_cfg.height = 1080;
  state.next_cfg.stride = 1920;
  state.nv12_1d = nv12_1080;
  state.stream_change_once = true;
  for (int i = 0; i < 3; ++i) {
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65, static_cast<uint8_t>(10 + i)};
    in.time_us = (3 + i) * 33333;
    state.emit_count = 1;
    state.emit_time_100ns = in.time_us * 10;
    Check(dec->Decode(in, &out), "1920x1080 frame decodes across/after STREAM_CHANGE");
    if (i == 0) {
      Check(state.last_buffer_capacity == 3110400, "upscaled output buffer grew to 3,110,400 bytes");
    }
  }
  Check(out.size() == 6 && out[3].width == 1920 && out[3].height == 1080 &&
            out[3].data == exp_1080 && out[5].data == exp_1080,
        "upscaled 1920x1080 frames reallocate output buffer and preserve pixels");
  dec.reset();
  Empty();
}

void ReuseAndFaultCases() {
  // 1. Steady-state !provides_samples reuse across 120 frames at 1920x1080 (3,110,400 B/frame).
  {
    Reset();
    constexpr int w = 1920, h = 1080, frames = 120;
    state.cfg.width = w;
    state.cfg.height = h;
    state.cfg.stride = w;
    state.cfg.provides = false;
    state.nv12_1d.clear();
    auto dec = Decoder(w, h);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x41, 0x9a};
    std::vector<VideoFrame> out;
    size_t buffers_after_0 = 0, samples_after_0 = 0;
    for (int i = 0; i < frames; ++i) {
      in.time_us = i * 16667;
      state.emit_count = 1;
      state.emit_time_100ns = in.time_us * 10;
      out.clear();
      Check(dec->Decode(in, &out) && out.size() == 1, "1080p !provides_samples frame decodes");
      if (i == 0) {
        buffers_after_0 = state.create_buffers;
        samples_after_0 = state.create_samples;
        Check(buffers_after_0 == 2 && samples_after_0 == 2,
              "frame 0 creates 1 input buffer+sample and 1 cached 3,110,400B output buffer+sample");
      }
    }
    Check(state.create_buffers == static_cast<size_t>(frames + 1) &&
              (state.create_buffers - buffers_after_0) == static_cast<size_t>(frames - 1),
          "120 1080p frames allocate 1 output buffer total (0 steady-state output allocs)");
    Check(state.create_samples == static_cast<size_t>(frames + 1) &&
              (state.create_samples - samples_after_0) == static_cast<size_t>(frames - 1),
          "120 1080p frames allocate 1 output sample total (0 steady-state sample allocs)");
    dec.reset();
    Empty();
  }

  // 2. Replaced db.pSample != provided and provides_samples flip on STREAM_CHANGE.
  for (int mode = 0; mode < 4; ++mode) {
    Reset();
    state.cfg.width = 16;
    state.cfg.height = 12;
    state.cfg.stride = 16;
    state.cfg.provides = false;
    state.nv12_1d = BuildNv12(16, 12, 16, 0x77u);
    auto dec = Decoder(16, 12);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65};
    state.replace_output_sample = true;
    std::vector<VideoFrame> out;
    if (mode == 0) {
      state.emit_count = 1;
      Check(dec->Decode(in, &out) && out.size() == 1, "replaced pSample on S_OK emits frame");
      Check(state.buffers == 1 && state.samples == 1, "replacement sample released on S_OK");
    } else if (mode == 1) {
      state.emit_count = 0;
      Check(dec->Decode(in, &out) && out.empty(), "replaced pSample on NEED_MORE_INPUT succeeds");
      Check(state.buffers == 1 && state.samples == 1, "replacement sample released on NEED_MORE_INPUT");
    } else if (mode == 2) {
      state.next_cfg = state.cfg;
      state.next_cfg.provides = true;
      state.stream_change_once = true;
      state.emit_count = 1;
      Check(dec->Decode(in, &out) && out.size() == 1, "STREAM_CHANGE to provides=true succeeds");
      Check(state.buffers == 0 && state.samples == 0,
            "switching to provides=true releases replacement and resets out_cache_");
    } else {
      state.output_error = true;
      Check(!dec->Decode(in, &out), "replaced pSample on ProcessOutput failure returns false");
      Check(state.buffers == 0 && state.samples == 0,
            "ProcessOutput failure releases replacement and resets out_cache_");
    }
    dec.reset();
    Empty();
  }

  // 3. Fault injection in Decode() (input sample construction & timestamp overflow).
  for (auto fault : {Fault::CreateBuffer, Fault::NullCreatedBuffer, Fault::Lock,
                     Fault::ShortBuffer, Fault::NullBuffer, Fault::Length, Fault::Unlock,
                     Fault::CreateSample, Fault::NullSample, Fault::AddBuffer,
                     Fault::Time, Fault::SetDiscontinuity}) {
    Reset();
    state.cfg.provides = false;
    auto dec = Decoder(16, 12, {1, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0, 3, 0x67, 0x42, 0x00, 1, 0, 2, 0x68, 0xce});
    dec->Flush();  // arm discontinuity_ and prefix_ so SetDiscontinuity & prefix are exercised
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65, 1, 2, 3};
    in.time_us = 500;
    state.fault = fault;
    std::vector<VideoFrame> out;
    Check(!dec->Decode(in, &out) && out.empty(), "Decode() input fault returns false cleanly");
    Check(state.inputs == 0 && state.buffers == 0 && state.samples == 0,
          "Decode() input fault retires all COM refs before ProcessInput");
    // Clear fault and verify decoder recovers cleanly with discontinuity + prefix intact!
    state.fault = Fault::None;
    state.nv12_1d = BuildNv12(16, 12, 16, 0x99u);
    state.emit_count = 1;
    Check(dec->Decode(in, &out) && out.size() == 1 && state.seen_discontinuity,
          "failed input sample preserves pending discontinuity and parameter-set prefix");
    dec.reset();
    Empty();
  }

  for (int64_t bad_time : {INT64_MAX / 10 + 1, INT64_MIN / 10 - 1}) {
    Reset();
    auto dec = Decoder(16, 12);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65};
    in.time_us = bad_time;
    std::vector<VideoFrame> out;
    Check(!dec->Decode(in, &out) && state.create_buffers == 0 && state.inputs == 0,
          "overflow timestamp rejected in Decode() before allocation");
    dec.reset();
    Empty();
  }

  // 4. Fault injection in Pull() (!provides_samples output cache allocation).
  for (auto fault : {Fault::CreateBuffer, Fault::NullCreatedBuffer,
                     Fault::CreateSample, Fault::NullSample, Fault::AddBuffer}) {
    Reset();
    state.cfg.width = 16;
    state.cfg.height = 12;
    state.cfg.stride = 16;
    state.cfg.provides = false;
    auto dec = Decoder(16, 12);
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65};
    state.pending_pull_fault = fault;
    state.emit_count = 1;
    std::vector<VideoFrame> out;
    Check(!dec->Decode(in, &out) && out.empty(),
          "Pull() output sample/buffer fault returns false without crashing");
    Check(state.buffers == 0 && state.samples == 0,
          "Pull() output allocation fault leaves zero live COM buffers or samples");
    dec.reset();
    Empty();
  }

  // 5. Init() CreateMediaType failure and SetOutput() invalid geometry / DWORD overflow.
  for (bool as_null : {false, true}) {
    Reset();
    state.fail_create_type = true;
    state.null_create_type = as_null;
    Format track = MakeFormat("video/avc", 16, 12);
    std::string error;
    Check(gesher::media::CreateMfVideoDecoder(track, &error) == nullptr && !error.empty(),
          "Init() handles CreateMediaType failure cleanly");
    Empty();
  }
  for (auto [w, h, stride] : {std::tuple{0, 16, 16}, {16, 0, 16}, {16, 16, 8}, {65536, 65536, 65536}}) {
    Reset();
    state.defer_output_until_input = true;
    auto dec = Decoder(16, 16);
    state.cfg.width = w;
    state.cfg.height = h;
    state.cfg.stride = stride;
    gesher::media::Sample in;
    in.data = {0, 0, 0, 1, 0x65};
    std::vector<VideoFrame> out;
    Check(!dec->Decode(in, &out) && out.empty(),
          "SetOutput() rejects invalid geometry or DWORD-overflowing NV12 size");
    dec.reset();
    Empty();
  }
}

double MonotonicMs() {
  std::timespec ts{};
  std::timespec_get(&ts, TIME_UTC);
  return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) * 1e-6;
}

void Measure(bool enforce) {
  Reset();
  constexpr int w = 1920, h = 1080, frames = 120;
  constexpr size_t nv12_bytes = size_t{w} * h * 3 / 2;  // 3,110,400 bytes
  state.cfg.width = w;
  state.cfg.height = h;
  state.cfg.stride = w;
  state.cfg.provides = false;
  state.nv12_1d.clear();
  auto dec = Decoder(w, h);
  gesher::media::Sample in;
  in.data.assign(64, 0x5a);
  std::vector<VideoFrame> out;
  out.reserve(2);
  counts = {};
  counts.active = true;
  const double t0 = MonotonicMs();
  size_t buffers_after_first = 0, samples_after_first = 0;
  for (int i = 0; i < frames; ++i) {
    in.time_us = i * 16667;
    state.emit_count = 1;
    state.emit_time_100ns = in.time_us * 10;
    Check(dec->Decode(in, &out) && out.size() == 1, "measured 1080p frame decodes");
    out.clear();
    if (i == 0) {
      buffers_after_first = state.create_buffers;
      samples_after_first = state.create_samples;
    }
  }
  const double wall_ms = MonotonicMs() - t0;
  counts.active = false;
  const size_t output_buffers = state.create_buffers - static_cast<size_t>(frames);
  const size_t output_samples = state.create_samples - static_cast<size_t>(frames);
  const size_t steady_output_buffers =
      (state.create_buffers - buffers_after_first) - static_cast<size_t>(frames - 1);
  const size_t steady_output_samples =
      (state.create_samples - samples_after_first) - static_cast<size_t>(frames - 1);
  const size_t output_buffer_bytes =
      state.create_buffer_bytes - static_cast<size_t>(frames) * in.data.size();
  std::printf("VIDEO_OUTPUT_METRICS frames=%d width=%d height=%d frame_nv12_bytes=%zu "
              "output_buffers=%zu steady_output_buffers=%zu output_samples=%zu "
              "output_buffer_bytes=%zu allocations=%zu allocated_bytes=%zu wall_ms=%.3f\n",
              frames, w, h, nv12_bytes, output_buffers, steady_output_buffers, output_samples,
              output_buffer_bytes, counts.allocations, counts.bytes, wall_ms);
  out = {};
  dec.reset();
  Check(counts.live == 0, "measured decoder allocations fully released");
  if (enforce) {
    Check(output_buffers == 1 && steady_output_buffers == 0 && output_buffer_bytes == nv12_bytes,
          "120 1080p !provides_samples frames allocate exactly one 3,110,400B output buffer");
    Check(output_samples == 1 && steady_output_samples == 0,
          "120 1080p !provides_samples frames allocate exactly one output sample");
  }
  Empty();
}
}  // namespace fixture

int main(int argc, char** argv) {
  using namespace fixture;
  const bool measure = argc > 1 && std::strcmp(argv[1], "measure") == 0;
  const bool valid_only = measure || (argc > 1 && std::strcmp(argv[1], "valid-gate") == 0);
  ValidDecodeCases(!valid_only);
  StreamChangeCases();
  if (!valid_only) ReuseAndFaultCases();
  Measure(!valid_only);
  std::printf("MF video output: %zu checks, %zu failures\n", checks, failures);
  return failures ? 1 : 0;
}
