// Complete production codec/NDK units with declared metadata, codec, window,
// clock and guest-entry boundaries. This is not Windows/media validation.
#include <gesher/bionic_shim.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <variant>

#include "compat/media/codec/callback_queue.h"
#include "compat/media/codec/codec_engine.h"
#include "compat/media/ndk/media_codec_abi.h"
#include "compat/media/surface/queue_surface.h"

using namespace std::chrono_literals;
using namespace gesher::media;
namespace abi = gesher::media::codec_abi;
std::atomic<int> checks{0}, failures{0}, formats{0}, tls_entries{0};
thread_local bool entered_guest = false;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) {
    ++failures;
    std::fprintf(stderr, "FAIL NDK codec host: %s\n", why);
  }
}
struct Signal {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  void Set() {
    std::lock_guard lock(mu);
    done = true;
    cv.notify_all();
  }
  void Wait() {
    std::unique_lock lock(mu);
    if (!cv.wait_for(lock, 5s, [&] { return done; })) {
      std::fprintf(stderr, "FAIL NDK codec host: timed out\n");
      std::_Exit(2);
    }
  }
};

using Value =
    std::variant<int32_t, int64_t, float, std::string, std::vector<uint8_t>>;
struct AMediaFormat {
  std::map<std::string, Value> data;
};
AMediaFormat* AMediaFormat_new() {
  ++formats;
  return new AMediaFormat;
}
media_status_t AMediaFormat_delete(AMediaFormat* f) {
  if (f) {
    --formats;
    delete f;
  }
  return AMEDIA_OK;
}
media_status_t AMediaFormat_copy(AMediaFormat* to, AMediaFormat* from) {
  if (!to || !from) return AMEDIA_ERROR_INVALID_PARAMETER;
  to->data = from->data;
  return AMEDIA_OK;
}
void AMediaFormat_setString(AMediaFormat* f, const char* k, const char* v) {
  f->data[k] = std::string(v);
}
void AMediaFormat_setInt32(AMediaFormat* f, const char* k, int32_t v) {
  f->data[k] = v;
}
void AMediaFormat_setInt64(AMediaFormat* f, const char* k, int64_t v) {
  f->data[k] = v;
}
void AMediaFormat_setBuffer(AMediaFormat* f, const char* k, const void* v,
                            size_t n) {
  const auto* p = static_cast<const uint8_t*>(v);
  f->data[k] = std::vector<uint8_t>(p, p + n);
}
template <class T>
bool GetValue(AMediaFormat* f, const char* key, T* out) {
  if (!f) return false;
  auto found = f->data.find(key);
  if (found == f->data.end()) return false;
  if (auto* v = std::get_if<T>(&found->second)) {
    *out = *v;
    return true;
  }
  return false;
}
namespace gesher::media {
Format::Format() : f_(AMediaFormat_new()) {}
Format::~Format() { AMediaFormat_delete(f_); }
Format& Format::operator=(Format&& f) noexcept {
  if (this != &f) {
    AMediaFormat_delete(f_);
    f_ = std::exchange(f.f_, nullptr);
  }
  return *this;
}
bool Format::GetInt32(const char* k, int32_t* out) const {
  return GetValue(f_, k, out);
}
bool Format::GetInt64(const char* k, int64_t* out) const {
  return GetValue(f_, k, out);
}
bool Format::GetFloat(const char* k, float* out) const {
  return GetValue(f_, k, out);
}
bool Format::GetString(const char* k, std::string* out) const {
  return GetValue(f_, k, out);
}
bool Format::GetBuffer(const char* k, std::vector<uint8_t>* out) const {
  return GetValue(f_, k, out);
}
std::string Format::Mime() const {
  std::string result;
  GetString("mime", &result);
  return result;
}
std::string Format::ToString() const { return Mime(); }
std::atomic<uint32_t> decoded_channels{2};
struct RawDecoder final : AudioDecoder {
  PcmInfo info() const override {
    return {48000, decoded_channels, PcmFormat::kS16};
  }
  bool Decode(const Sample& s, std::vector<uint8_t>* out) override {
    *out = s.data;
    return true;
  }
};
std::unique_ptr<AudioDecoder> AudioDecoder::Create(const Format&,
                                                   std::string*) {
  return std::make_unique<RawDecoder>();
}
bool AudioDecoder::Supports(const std::string& mime) {
  return mime == "audio/raw";
}
const std::vector<VideoCodecInfo>& VideoDecoder::Available() {
  static std::vector<VideoCodecInfo> v{
      {"video/avc", "fixture", false, 4096, 4096}};
  return v;
}
std::unique_ptr<VideoDecoder> VideoDecoder::Create(const Format&,
                                                   std::string*) {
  return nullptr;
}
const std::vector<EncoderInfo>& AvailableEncoders() {
  static std::vector<EncoderInfo> v{{"video/avc", "fixture", true}};
  return v;
}
std::atomic<int> encoded_frames{0};
struct TestEncoder final : VideoEncoder {
  std::string Name() const override { return "fixture"; }
  bool SetBitrate(int) override { return true; }
  bool Encode(const VideoFrame&, bool, std::vector<EncodedPacket>*) override {
    ++encoded_frames;
    return true;
  }
  bool Drain(std::vector<EncodedPacket>*) override { return true; }
};
std::unique_ptr<VideoEncoder> VideoEncoder::Create(const std::string&,
                                                   const VideoEncoderParams&,
                                                   std::string*) {
  return std::make_unique<TestEncoder>();
}
std::unique_ptr<AudioEncoder> AudioEncoder::Create(const std::string&,
                                                   const AudioEncoderParams&,
                                                   std::string*) {
  return nullptr;
}
void VideoFrame::Allocate(int w, int h) {
  width = w;
  height = h;
  stride_y = w;
  stride_uv = (w + 1) / 2;
  crop_right = w - 1;
  crop_bottom = h - 1;
  data.resize(static_cast<size_t>(w) * h +
              2 * static_cast<size_t>(stride_uv) * ((h + 1) / 2));
}
void RgbaToI420(const uint32_t*, int, int, VideoFrame*) {}
void Nv12ToI420(const uint8_t*, int, int, VideoFrame*) {}
// Match video_decoder.h exactly, so a mismatched engine overload fails to link.
int RenderFrame(ANativeWindow*, const VideoFrame&, std::optional<int64_t>, int) { return 0; }
}  // namespace gesher::media
extern "C" void ANativeWindow_acquire(ANativeWindow* w) {
  w->incStrong(nullptr);
}
extern "C" void ANativeWindow_release(ANativeWindow* w) {
  w->decStrong(nullptr);
}

namespace gesher::bionic {
std::map<std::string, void*>& Symbols() {
  static std::map<std::string, void*> symbols;
  return symbols;
}
void AddBionicSymbol(const char* name, void* address) {
  Symbols()[name] = address;
}
void* LookupBionicSymbol(const char* name) { return Symbols().at(name); }
void EnterLoadedCode() {
  entered_guest = true;
  ++tls_entries;
}
}  // namespace gesher::bionic
template <class R, class... Args>
R Invoke(const char* name, Args... args) {
  using Fn = R(GESHER_CODEC_GUEST_ABI*)(Args...);
  return reinterpret_cast<Fn>(gesher::bionic::LookupBionicSymbol(name))(
      args...);
}

AMediaCodec* RawCodec() {
  auto* c = Invoke<AMediaCodec*>("AMediaCodec_createDecoderByType",
                                 static_cast<const char*>("audio/raw"));
  Check(c != nullptr, "real NDK adapter creates advertised decoder");
  Format f;
  AMediaFormat_setString(f.get(), "mime", "audio/raw");
  AMediaFormat_setInt32(f.get(), "max-input-size", 64);
  Check(Invoke<media_status_t>("AMediaCodec_configure", c,
                               static_cast<const AMediaFormat*>(f.get()),
                               static_cast<ANativeWindow*>(nullptr),
                               static_cast<AMediaCrypto*>(nullptr),
                               uint32_t{0}) == AMEDIA_OK,
        "raw format configured through exported ABI");
  return c;
}
void Send(AMediaCodec* c, size_t index) {
  size_t size = 0;
  auto* data = Invoke<uint8_t*>("AMediaCodec_getInputBuffer", c, index, &size);
  Check(data != nullptr && size == 64, "callback owns real engine input");
  if (!data || size < 4) return;
  std::memcpy(data, "abcd", 4);
  Check(Invoke<media_status_t>("AMediaCodec_queueInputBuffer", c, index,
                               int64_t{0}, size_t{4}, uint64_t{1234},
                               uint32_t{4}) == AMEDIA_OK,
        "callback queues payload and EOS");
}

void Synchronous() {
  const int before = formats;
  auto* c = RawCodec();
  Check(Invoke<media_status_t>("AMediaCodec_start", c) == AMEDIA_OK,
        "sync start");
  size_t cap = 99;
  Check(Invoke<uint8_t*>("AMediaCodec_getInputBuffer", c, size_t{0}, &cap) ==
                nullptr &&
            cap == 0,
        "undequeued input is not owned by the caller");
  const int64_t index =
      Invoke<int64_t>("AMediaCodec_dequeueInputBuffer", c, int64_t{1000000});
  Check(index >= 0, "sync input dequeue");
  const size_t huge = (size_t{1} << 32) + static_cast<size_t>(index);
  Check(
      Invoke<uint8_t*>("AMediaCodec_getInputBuffer", c, huge, &cap) == nullptr,
      "LP64 input index cannot alias a 32-bit slot");
  Check(Invoke<media_status_t>("AMediaCodec_queueInputBuffer", c, huge,
                               int64_t{0}, size_t{4}, uint64_t{0},
                               uint32_t{0}) == AMEDIA_ERROR_INVALID_PARAMETER,
        "large queue index is rejected");
  Check(Invoke<media_status_t>("AMediaCodec_queueInputBuffer", c,
                               static_cast<size_t>(index), int64_t{-1},
                               size_t{4}, uint64_t{0},
                               uint32_t{0}) == AMEDIA_ERROR_INVALID_PARAMETER,
        "negative 64-bit offset remains negative across NDK ABI");
  for (uint32_t flag : {uint32_t{8}, uint32_t{32}}) {
    Check(Invoke<media_status_t>("AMediaCodec_queueInputBuffer", c,
                                 static_cast<size_t>(index), int64_t{0},
                                 size_t{4}, uint64_t{0}, flag) == AMEDIA_ERROR_UNSUPPORTED,
          "unsupported input operation reports unsupported");
    Check(Invoke<uint8_t*>("AMediaCodec_getInputBuffer", c,
                           static_cast<size_t>(index), &cap) != nullptr && cap == 64,
          "rejected operation preserves the client's input buffer for retry");
  }
  Send(c, static_cast<size_t>(index));
  Check(Invoke<uint8_t*>("AMediaCodec_getInputBuffer", c,
                         static_cast<size_t>(index), &cap) == nullptr,
        "queued input is no longer owned by client");
  bool eos = false, data = false;
  for (int tries = 0; tries < 10 && !eos; ++tries) {
    abi::BufferInfo info{};
    const int64_t slot = Invoke<int64_t>("AMediaCodec_dequeueOutputBuffer", c,
                                         &info, int64_t{1000000});
    if (slot == -2) continue;
    Check(slot >= 0, "sync output dequeue");
    if (slot < 0) break;
    auto* snapshot = Invoke<AMediaFormat*>("AMediaCodec_getBufferFormat", c,
                                           static_cast<size_t>(slot));
    int32_t rate = 0;
    Check(snapshot && GetValue(snapshot, "sample-rate", &rate) && rate == 48000,
          "buffer format reflects the published output");
    AMediaFormat_delete(snapshot);
    Check(Invoke<AMediaFormat*>("AMediaCodec_getBufferFormat", c, huge) ==
              nullptr,
          "invalid buffer format index is rejected");
    auto* bytes = Invoke<uint8_t*>("AMediaCodec_getOutputBuffer", c,
                                   static_cast<size_t>(slot), &cap);
    Check(cap == static_cast<size_t>(info.size), "output range excludes unused backing storage");
    if (info.size == 4) {
      Check(bytes && std::memcmp(bytes, "abcd", 4) == 0 &&
                info.presentationTimeUs == 1234,
            "output preserves actual PCM bytes and presentation time");
      data = true;
    }
    eos = (info.flags & 4) != 0;
    Check(Invoke<media_status_t>("AMediaCodec_releaseOutputBuffer", c,
                                 static_cast<size_t>(slot), false) == 0,
          "owned output releases");
    Check(Invoke<AMediaFormat*>("AMediaCodec_getBufferFormat", c,
                                static_cast<size_t>(slot)) == nullptr,
          "released output no longer exposes a format");
  }
  Check(eos && data, "sync decode emits payload and EOS");
  Invoke<media_status_t>("AMediaCodec_delete", c);
  Check(formats == before, "sync decoder releases every format snapshot");
}

// Hold the first exact-production Start notification before it returns to
// Start's loop. Another caller flushes and starts a new epoch, then releases
// the old suffix. No sleeps or scheduler probability establish this ordering.
void DelayedStartEpoch() {
  auto engine = std::make_shared<CodecEngine>(
      *CodecEngine::FindByType("audio/raw", false));
  Format f;
  AMediaFormat_setString(f.get(), "mime", "audio/raw");
  AMediaFormat_setInt32(f.get(), "max-input-size", 64);
  std::string error;
  Check(engine->Configure(std::move(f), nullptr, {}, &error),
        "epoch decoder configure");
  Signal first, resume;
  std::mutex seen_mu;
  std::vector<std::pair<uint64_t, int>> seen;
  std::atomic<bool> hold{true};
  CodecCallbacks callbacks;
  callbacks.on_input_available = [&](uint64_t generation, int index) {
    if (index == 0 && hold.exchange(false)) {
      first.Set();
      resume.Wait();
    }
    std::lock_guard lock(seen_mu);
    seen.emplace_back(generation, index);
  };
  Check(engine->SetAsync(true, std::move(callbacks)),
        "epoch callback registration");
  std::thread original(
      [&] { Check(engine->Start(&error), "epoch original Start returns"); });
  first.Wait();
  int status = 0;
  engine->Flush(&status);
  Check(status == 0,
        "flush completes while original Start notification is delayed");
  Check(engine->Start(&error), "new epoch starts before old batch finishes");
  resume.Set();
  original.join();
  Check(seen.size() == 2 * kCodecSlots,
        "both exact-production batches reached dispatch boundary");
  std::vector<bool> live(kCodecSlots, false);
  int accepted = 0, stale = 0;
  for (auto [generation, index] : seen) {
    if (!engine->IsCallbackCurrent(generation)) {
      ++stale;
      continue;
    }
    Check(!live[index], "no stale duplicate input is accepted after restart");
    live[index] = true;
    ++accepted;
  }
  Check(accepted == kCodecSlots && stale == kCodecSlots,
        "delayed original suffix retains its obsolete epoch across resume");
  const auto active = seen.front().first;
  engine->Stop();
  Check(!engine->IsCallbackCurrent(active),
        "stop invalidates already-dispatched buffer events");
}

void InvalidPcmLayout() {
  for (uint32_t channels :
       {uint32_t{0}, uint32_t{32769}, uint32_t{0x80000000}}) {
    decoded_channels = channels;
    auto* c = RawCodec();
    Check(Invoke<media_status_t>("AMediaCodec_start", c) == 0,
          "invalid PCM start");
    const auto index =
        Invoke<int64_t>("AMediaCodec_dequeueInputBuffer", c, int64_t{1000000});
    Check(index >= 0, "invalid PCM input slot");
    Send(c, static_cast<size_t>(index));
    abi::BufferInfo info{};
    auto status = Invoke<int64_t>("AMediaCodec_dequeueOutputBuffer", c, &info,
                                  int64_t{1000000});
    if (status == -2)
      status = Invoke<int64_t>("AMediaCodec_dequeueOutputBuffer", c, &info,
                               int64_t{1000000});
    Check(status == AMEDIA_ERROR_UNKNOWN,
          "invalid PCM layout reports error without overflow or zero-progress "
          "loop");
    Check(Invoke<int64_t>("AMediaCodec_dequeueInputBuffer", c,
                          int64_t{1000000}) == AMEDIA_ERROR_UNKNOWN,
          "failed decoder cannot return more successful input slots");
    Invoke<media_status_t>("AMediaCodec_delete", c);
  }
  decoded_channels = 2;
}

struct AsyncState {
  Signal completed;
  std::atomic<bool> sent{false}, flushed{false};
  bool flush = false, delete_first = false;
  std::atomic<int> format_calls{0}, output_calls{0};
};
void GESHER_CODEC_GUEST_ABI AsyncInput(AMediaCodec* c, void* opaque,
                                       int32_t index) {
  auto* state = static_cast<AsyncState*>(opaque);
  Check(entered_guest,
        "native callback enters loaded-code TLS before application code");
  if (state->delete_first) {
    Check(Invoke<media_status_t>("AMediaCodec_delete", c) == 0,
          "delete from initial native callback");
    state->completed.Set();
    return;
  }
  if (state->flush && !state->flushed.exchange(true)) {
    Check(Invoke<media_status_t>("AMediaCodec_flush", c) == 0,
          "flush from native callback completes");
    Check(Invoke<media_status_t>("AMediaCodec_start", c) == 0,
          "async restart after callback flush");
    return;
  }
  if (!state->sent.exchange(true)) Send(c, static_cast<size_t>(index));
}
void GESHER_CODEC_GUEST_ABI AsyncFormat(AMediaCodec*, void* opaque,
                                        AMediaFormat* format) {
  auto* state = static_cast<AsyncState*>(opaque);
  ++state->format_calls;
  Check(entered_guest && format != nullptr,
        "format callback owns a live format");
  AMediaFormat_delete(format);
}
void GESHER_CODEC_GUEST_ABI AsyncOutput(AMediaCodec* c, void* opaque,
                                        int32_t index, abi::BufferInfo* info) {
  auto* state = static_cast<AsyncState*>(opaque);
  ++state->output_calls;
  const bool eos = info && (info->flags & 4);
  Check(Invoke<media_status_t>("AMediaCodec_releaseOutputBuffer", c,
                               static_cast<size_t>(index), false) == 0,
        "async callback releases its output");
  if (eos) {
    Check(Invoke<media_status_t>("AMediaCodec_stop", c) == 0,
          "stop from output callback completes");
    Check(Invoke<media_status_t>("AMediaCodec_delete", c) == 0,
          "delete after callback stop completes");
    state->completed.Set();
  }
}
void GESHER_CODEC_GUEST_ABI AsyncError(AMediaCodec* c, void* opaque,
                                       media_status_t, int32_t, const char*) {
  Check(false, "unexpected error callback");
  Invoke<media_status_t>("AMediaCodec_delete", c);
  static_cast<AsyncState*>(opaque)->completed.Set();
}
void Asynchronous(bool flush, bool indirect, bool delete_first) {
  auto* c = RawCodec();
  AsyncState state;
  state.flush = flush;
  state.delete_first = delete_first;
  abi::OnAsyncNotifyCallback cb{AsyncInput, AsyncOutput, AsyncFormat,
                                AsyncError};
  const media_status_t registered =
      indirect ? Invoke<media_status_t>(
                     "__gesher_AMediaCodec_setAsyncNotifyCallback", c,
                     static_cast<const abi::OnAsyncNotifyCallback*>(&cb),
                     static_cast<void*>(&state))
               : Invoke<media_status_t>("AMediaCodec_setAsyncNotifyCallback", c,
                                        cb, static_cast<void*>(&state));
  Check(registered == 0,
        "native callback aggregate preserves callback pointers and userdata");
  Check(Invoke<media_status_t>("AMediaCodec_start", c) == 0,
        "async start survives callback-owned deletion");
  state.completed.Wait();
  if (!delete_first)
    Check(state.format_calls == 1 && state.output_calls == 2,
          "serialized callback format, payload and EOS are delivered once");
}

AMediaCodec* VideoCodec() {
  auto* c = Invoke<AMediaCodec*>("AMediaCodec_createEncoderByType",
                                 static_cast<const char*>("video/avc"));
  Format f;
  AMediaFormat_setString(f.get(), "mime", "video/avc");
  AMediaFormat_setInt32(f.get(), "width", 16);
  AMediaFormat_setInt32(f.get(), "height", 16);
  Check(Invoke<media_status_t>("AMediaCodec_configure", c,
                               static_cast<const AMediaFormat*>(f.get()),
                               static_cast<ANativeWindow*>(nullptr),
                               static_cast<AMediaCrypto*>(nullptr),
                               uint32_t{1}) == 0,
        "fixture-backed video encoder configured");
  return c;
}
void PersistentSurface() {
  auto* first = VideoCodec();
  auto* second = VideoCodec();
  ANativeWindow* window = nullptr;
  Check(Invoke<media_status_t>("AMediaCodec_createPersistentInputSurface",
                               &window) == 0,
        "persistent producer created");
  Check(
      Invoke<media_status_t>("AMediaCodec_setInputSurface", first, window) == 0,
      "first encoder acquires persistent producer");
  Check(
      Invoke<media_status_t>("AMediaCodec_setInputSurface", first, window) == 0,
      "same encoder reattachment is idempotent");
  Check(Invoke<media_status_t>("AMediaCodec_signalEndOfInputStream", first) ==
            AMEDIA_ERROR_INVALID_OPERATION,
        "configured surface encoder cannot signal successful EOS before start");
  Format parameters;
  Check(Invoke<media_status_t>(
            "AMediaCodec_setParameters", first,
            static_cast<const AMediaFormat*>(parameters.get())) ==
            AMEDIA_ERROR_INVALID_OPERATION,
        "parameters reject nonexecuting codec state");
  Check(Invoke<media_status_t>("AMediaCodec_setOutputSurface", first, window) ==
            AMEDIA_ERROR_INVALID_OPERATION,
        "encoder cannot pretend to retarget a decoder output surface");
  Check(Invoke<media_status_t>("AMediaCodec_setOutputSurface", first,
                               static_cast<ANativeWindow*>(nullptr)) ==
            AMEDIA_ERROR_INVALID_PARAMETER,
        "NDK output surface setter rejects null destination");
  Check(Invoke<media_status_t>("AMediaCodec_setInputSurface", second, window) ==
            AMEDIA_ERROR_INVALID_OPERATION,
        "second encoder cannot replace the live consumer");
  auto* queue = QueueSurface::From(window);
  auto stale_listener = queue->SnapshotListener();
  Check(static_cast<bool>(stale_listener), "producer has an actual listener");
  Check(Invoke<media_status_t>("AMediaCodec_stop", first) == 0,
        "stop releases persistent consumer");
  Check(Invoke<media_status_t>("AMediaCodec_setInputSurface", second, window) ==
            0,
        "retired producer can attach to next encoder");
  stale_listener();
  ANativeWindow* ordinary = nullptr;
  auto* third = VideoCodec();
  Check(Invoke<media_status_t>("AMediaCodec_createInputSurface", third,
                               &ordinary) == 0,
        "ordinary encoder surface created");
  Check(Invoke<media_status_t>("AMediaCodec_setInputSurface", first,
                               ordinary) == AMEDIA_ERROR_INVALID_PARAMETER,
        "ordinary QueueSurface cannot masquerade as persistent producer");
  Invoke<media_status_t>("AMediaCodec_delete", first);
  Invoke<media_status_t>("AMediaCodec_delete", second);
  Invoke<media_status_t>("AMediaCodec_delete", third);
  ANativeWindow_release(window);
  ANativeWindow_release(ordinary);
  Check(QueueSurface::live == 0,
        "all native input producers retire without leaks");
}

void CallbackQueue() {
  Signal entered, release, stopped;
  auto queue = CodecCallbackQueue::Create({});
  std::atomic<int> calls{0};
  queue->Post([&] {
    ++calls;
    entered.Set();
    release.Wait();
  });
  entered.Wait();
  for (int i = 0; i < 64; ++i) queue->Post([&] { ++calls; });
  std::thread closing([&] {
    queue->Close();
    stopped.Set();
  });
  queue->Pause();
  release.Set();
  stopped.Wait();
  closing.join();
  Check(calls == 1,
        "close cancels pending callbacks and waits for the active invocation");
  queue->Post([&] { ++calls; });
  Check(calls == 1, "closed queue never admits another callback");
}

#include "jni_codec_callbacks.inc"

int main() {
  jni_callbacks_host::Run();
  CallbackQueue();
  Synchronous();
  DelayedStartEpoch();
  InvalidPcmLayout();
  PersistentSurface();
  Asynchronous(false, false, false);
  Asynchronous(true, false, false);
  Asynchronous(false, true, false);
  for (int i = 0; i < 32; ++i) Asynchronous(false, false, true);
  Check(tls_entries == 35,
        "one loaded-code entry per native notification worker");
  std::printf("NDK codec host: %d checks, %d failures\n", checks.load(),
              failures.load());
  return failures == 0 ? 0 : 1;
}
