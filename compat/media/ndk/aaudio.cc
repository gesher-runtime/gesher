// Gesher: the NDK's AAudio (libaaudio.so) for an app's ELF libraries
// (docs/media.md), over the audio engine: an output stream is an engine
// track; a stream with a data callback has a thread that calls it whenever
// the track has room for a burst. An input stream (media3, ADR 0059) is a
// capture stream (compat/media/audio/capture.h): its callback thread calls the
// data callback with each burst captured; AAudioStream_read reads it. Opening
// one needs RECORD_AUDIO in the app's manifest (AAUDIO_ERROR_UNAVAILABLE
// otherwise).
//
// The functions are System V entries the .so's imports bind to, added to the
// bionic shim's table by a static initializer (as libandroid's are,
// docs/native-libs.md); the callbacks the .so passes are System V too, called
// on threads set up for loaded code (EnterLoadedCode).

#include <time.h>
#include <gesher/clocks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <new>
#include <mutex>
#include <thread>
#include <vector>

#include <gesher/bionic_shim.h>

#include "../audio/capture.h"
#include "../audio/engine.h"
#include "../audio/log.h"
#include "../audio/record_permission.h"
#include "../audio/shared_state.h"

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define SYSV
#endif

namespace gesher::media {

struct AAudioStream;

namespace {

// aaudio/AAudio.h
using aaudio_result_t = int32_t;
constexpr aaudio_result_t kOk = 0;
constexpr aaudio_result_t kErrorDisconnected = -899;
constexpr aaudio_result_t kErrorIllegalArgument = -898;
constexpr aaudio_result_t kErrorInternal = -896;
constexpr aaudio_result_t kErrorInvalidState = -895;
constexpr aaudio_result_t kErrorUnimplemented = -890;
constexpr aaudio_result_t kErrorUnavailable = -889;
constexpr aaudio_result_t kErrorNoMemory = -887;
constexpr aaudio_result_t kErrorNull = -886;
constexpr aaudio_result_t kErrorTimeout = -885;
constexpr aaudio_result_t kErrorInvalidFormat = -883;
constexpr aaudio_result_t kErrorOutOfRange = -882;
constexpr aaudio_result_t kErrorInvalidRate = -880;

enum : int32_t { kDirectionOutput = 0, kDirectionInput = 1 };
enum : int32_t { kFormatUnspecified = 0, kFormatI16 = 1, kFormatFloat = 2, kFormatI24 = 3, kFormatI32 = 4 };
enum : int32_t {
  kStateUninitialized = 0, kStateOpen = 2, kStateStarting = 3, kStateStarted = 4,
  kStatePausing = 5, kStatePaused = 6, kStateFlushing = 7, kStateFlushed = 8,
  kStateStopping = 9, kStateStopped = 10, kStateClosing = 11, kStateClosed = 12, kStateDisconnected = 13,
};
constexpr int32_t kCallbackContinue = 0;
constexpr int32_t kSharingShared = 1;
constexpr int32_t kPerformanceNone = 10;

using DataCallback = int32_t(SYSV*)(AAudioStream*, void*, void*, int32_t);
using ErrorCallback = void(SYSV*)(AAudioStream*, void*, aaudio_result_t);

struct Builder {
  int32_t device_id = 0;
  int32_t sample_rate = 0;
  int32_t channels = 0;
  // Zero with a positive count is an indexed layout, which the public
  // channel-mask query reports as AAUDIO_UNSPECIFIED.
  uint32_t channel_mask = 0;
  int32_t format = kFormatUnspecified;
  int32_t sharing = kSharingShared;
  int32_t direction = kDirectionOutput;
  int32_t capacity = 0;
  int32_t performance = kPerformanceNone;
  int32_t usage = 1;  // AAUDIO_USAGE_MEDIA
  int32_t content_type = 2;  // AAUDIO_CONTENT_TYPE_MUSIC
  int32_t input_preset = 0;
  int32_t session_id = -1;  // AAUDIO_SESSION_ID_NONE
  int32_t frames_per_callback = 0;
  DataCallback data_cb = nullptr;
  void* data_user = nullptr;
  ErrorCallback error_cb = nullptr;
  void* error_user = nullptr;
};

// Public lifecycle operations cannot run from this stream's data callback.
// Unlike std::thread::get_id(), this does not race a concurrent worker join.
thread_local AAudioStream* t_callback_stream = nullptr;

}  // namespace

struct AAudioStream {
  Builder b;
  std::shared_ptr<Track> track;
  std::shared_ptr<CaptureStream> capture;  // input streams
  PcmFormat pcm;
  size_t frame_size;
  int32_t burst;
  std::atomic<int32_t> state{kStateOpen};
  std::atomic<int64_t> written{0};
  std::mutex mu;
  std::mutex release_mu;
  std::condition_variable cv;
  std::thread callback_thread;
  bool quit = false;
  bool error_notified = false;
  uint64_t generation = 0;  // under mu; retires work admitted before a transition
};

namespace {

// AAUDIO_USAGE_* to Android stream types, as the audio policy maps them.
int StreamForUsage(int32_t usage) {
  switch (usage) {
    case 2: case 3: return 0;    // VOICE_COMMUNICATION(_SIGNALLING)
    case 4: return 4;            // ALARM
    case 5: case 7: case 8: case 9: case 10: return 5;  // NOTIFICATION*
    case 6: return 2;            // NOTIFICATION_RINGTONE
    case 13: return 1;           // ASSISTANCE_SONIFICATION
    case 11: return 10;          // ASSISTANCE_ACCESSIBILITY
    default: return 3;           // MEDIA, GAME, ...
  }
}

int64_t Now() {
  timespec ts;
  gesher::AndroidClockGettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void TraceInput(AAudioStream* s, const char* operation, int32_t requested, int32_t returned) {
  const char* trace = getenv("GESHER_MEDIA_TRACE_RECORD");
  if (s->capture == nullptr || trace == nullptr || trace[0] != '1') return;
  const CaptureStats stats = s->capture->stats();
  MEDIA_LOGI("AAudio input %s trace: %d Hz, capacity %zu, requested %d frames, returned %d frames, "
             "captured %llu, read %llu, lost %u, xruns %u, timestamp %lld ns",
             operation, s->b.sample_rate, s->capture->capacity_frames(), requested, returned,
             static_cast<unsigned long long>(stats.captured_frames),
             static_cast<unsigned long long>(stats.read_frames), stats.lost_frames, stats.overruns,
             static_cast<long long>(stats.timestamp_ns));
}

// Called only by this stream's callback/monitor worker, outside capture locks.
void InputDisconnected(AAudioStream* s) {
  ErrorCallback callback;
  void* user;
  {
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->quit || s->error_notified) return;
    s->error_notified = true;
    ++s->generation;
    s->state = kStateDisconnected;
    callback = s->b.error_cb;
    user = s->b.error_user;
  }
  s->cv.notify_all();
  if (callback != nullptr) callback(s, user, kErrorDisconnected);
}

// An input stream's callback thread: each burst captured goes to the data callback.
void InputCallbackLoop(AAudioStream* s) {
  gesher::bionic::EnterLoadedCode();
  RaiseAudioThreadPriority(true);
  const int32_t callback_frames = s->b.frames_per_callback;
  std::vector<uint8_t> buf(static_cast<size_t>(callback_frames) * s->frame_size);
  for (;;) {
    uint64_t generation;
    {
      std::unique_lock<std::mutex> lock(s->mu);
      s->cv.wait(lock, [s] { return s->quit || s->state == kStateStarted || s->state == kStateDisconnected; });
      if (s->quit) return;
      generation = s->generation;
    }
    if (s->capture->error() != 0 || s->state == kStateDisconnected) {
      InputDisconnected(s);
      return;
    }
    if (s->b.data_cb == nullptr) {
      std::unique_lock<std::mutex> lock(s->mu);
      s->cv.wait_for(lock, std::chrono::milliseconds(50), [s] { return s->quit || s->state != kStateStarted; });
      continue;
    }
    if (!s->capture->WaitForData(static_cast<size_t>(callback_frames), 50)) continue;
    {
      std::lock_guard<std::mutex> lock(s->mu);
      if (s->quit) return;
      if (s->state != kStateStarted || s->generation != generation) continue;
      if (s->capture->Read(buf.data(), buf.size(), false) != static_cast<ssize_t>(buf.size())) continue;
    }
    int32_t r = s->b.data_cb(s, s->b.data_user, buf.data(), callback_frames);
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->quit) return;
    if (s->state != kStateStarted || s->generation != generation) continue;
    if (r != kCallbackContinue) {
      s->capture->Stop();
      ++s->generation;
      s->state = kStateStopped;
    }
  }
}

void CallbackLoop(AAudioStream* s) {
  t_callback_stream = s;
  if (s->capture != nullptr) {
    InputCallbackLoop(s);
    return;
  }
  gesher::bionic::EnterLoadedCode();
  RaiseAudioThreadPriority(true);
  const int32_t callback_frames = s->b.frames_per_callback;
  std::vector<uint8_t> buf(static_cast<size_t>(callback_frames) * s->frame_size);
  for (;;) {
    uint64_t generation;
    {
      std::unique_lock<std::mutex> lock(s->mu);
      s->cv.wait(lock, [s] { return s->quit || s->state == kStateStarted; });
      if (s->quit) return;
      generation = s->generation;
    }
    // Woken by the mixer as it plays (not a sleep: under load a sleeping feeder wakes
    // late and the stream's few bursts run dry).
    if (!s->track->WaitForRoom(static_cast<size_t>(callback_frames), 20)) continue;
    {
      std::lock_guard<std::mutex> lock(s->mu);
      if (s->quit) return;
      if (s->state != kStateStarted || s->generation != generation) continue;
    }
    int32_t r = s->b.data_cb(s, s->b.data_user, buf.data(), callback_frames);
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->quit) return;
    if (s->state != kStateStarted || s->generation != generation) continue;
    const ssize_t written = s->track->Write(buf.data(), buf.size(), false);
    if (written > 0) s->written += static_cast<int64_t>(written / s->frame_size);
    if (r != kCallbackContinue) {
      s->track->Stop();
      ++s->generation;
      s->state = kStateStopped;
    }
  }
}

// --- AAudioStreamBuilder --------------------------------------------------------------

aaudio_result_t SYSV CreateStreamBuilder(Builder** out) {
  if (out == nullptr) return kErrorNull;
  *out = new (std::nothrow) Builder();
  return *out != nullptr ? kOk : kErrorNoMemory;
}
#define SETTER(Name, field)                                          \
  void SYSV Name(Builder* b, int32_t v) {                           \
    if (b != nullptr) b->field = v;                                 \
  }
SETTER(SetDeviceId, device_id)
SETTER(SetSampleRate, sample_rate)
SETTER(SetFormat, format)
SETTER(SetSharingMode, sharing)
SETTER(SetDirection, direction)
SETTER(SetBufferCapacityInFrames, capacity)
SETTER(SetPerformanceMode, performance)
SETTER(SetUsage, usage)
SETTER(SetContentType, content_type)
SETTER(SetInputPreset, input_preset)
SETTER(SetSessionId, session_id)
SETTER(SetFramesPerDataCallback, frames_per_callback)
#undef SETTER
void SYSV SetIgnored(Builder*, int32_t) {}
void SYSV SetIgnoredString(Builder*, const char*) {}
void SYSV SetChannelCount(Builder* b, int32_t count) {
  if (b == nullptr) return;
  b->channels = count;
  b->channel_mask = 0;
}
void SYSV SetChannelMask(Builder* b, uint32_t mask) {
  if (b == nullptr) return;
  b->channel_mask = mask;
  b->channels = __builtin_popcount(mask);
}
void SYSV SetDataCallback(Builder* b, DataCallback cb, void* user) {
  if (b == nullptr) return;
  b->data_cb = cb;
  b->data_user = user;
}
void SYSV SetErrorCallback(Builder* b, ErrorCallback cb, void* user) {
  if (b == nullptr) return;
  b->error_cb = cb;
  b->error_user = user;
}
aaudio_result_t SYSV BuilderDelete(Builder* b) {
  delete b;
  return kOk;
}

// Android 16r4 AAudioConvert_aaudioToAndroidChannelLayoutMask, using the
// public AAudio bit values (not Java AudioFormat's shifted input masks).
bool AndroidChannelMask(uint32_t mask, bool input) {
  switch (mask) {
    case 0: case 0x1: case 0x3:  // unspecified, mono, stereo
    case 0xc0003: case 0xc000b: case 0xc0007: case 0xc000f:
    case 0x3f:  // 2.0.2, 2.1.2, 3.0.2, 3.1.2, 5.1
      return true;
    case 0x104:  // FRONT_BACK
      return input;
    case 0xb: case 0x7: case 0x103: case 0xf:  // 2.1, TRI, TRI_BACK, 3.1
    case 0x33: case 0x603: case 0x107: case 0x37:  // QUAD, QUAD_SIDE, SURROUND, PENTA
    case 0x60f: case 0x13f: case 0x63f:  // 5.1_SIDE, 6.1, 7.1
    case 0xc003f: case 0x2d03f: case 0xc063f: case 0x2d63f:
    case 0x302d63f: case 0x30ed63f:  // height/wide layouts
      return !input;
    default:
      return false;
  }
}

bool SupportedChannelMask(uint32_t mask, bool input) {
  if (mask == 0 || mask == 0x1 || mask == 0x3) return true;
  if (input) return false;
  // Track::MixInto consumes FL FR FC LFE followed by surround pairs.
  // These layouts have exactly that order. A QUAD or 2.1 request must not
  // reinterpret a rear/LFE sample as the center channel merely by popcount.
  return mask == 0x7 || mask == 0xf || mask == 0x3f || mask == 0x60f || mask == 0x63f;
}

// Android 16's AAudioStreamParameters::validate / AudioStreamBuilder::validate.
// Accepted ranges precede device setup; legal but unavailable backend features
// report an error rather than silently becoming a different stream direction.
aaudio_result_t ValidateBuilder(const Builder& b) {
  if (b.channels < 0 || b.channels > 12 || b.device_id < 0) return kErrorOutOfRange;
  if (b.sharing != 0 && b.sharing != kSharingShared) return kErrorIllegalArgument;
  if (b.format < kFormatUnspecified || b.format > kFormatI32) return kErrorInvalidFormat;
  if (b.sample_rate != 0 && (b.sample_rate < 8000 || b.sample_rate > 1600000)) return kErrorInvalidRate;
  if (b.capacity < 0 || b.frames_per_callback < 0 || b.frames_per_callback > 1024 * 1024) {
    return kErrorOutOfRange;
  }
  if (b.direction != kDirectionInput && b.direction != kDirectionOutput) return kErrorIllegalArgument;
  if (!AndroidChannelMask(b.channel_mask, b.direction == kDirectionInput)) return kErrorIllegalArgument;
  if (!SupportedChannelMask(b.channel_mask, b.direction == kDirectionInput)) return kErrorUnavailable;
  if (!((b.usage >= 0 && b.usage <= 6) || (b.usage >= 10 && b.usage <= 14) ||
        b.usage == 16 || (b.usage >= 1000 && b.usage <= 1003))) return kErrorIllegalArgument;
  if (b.content_type < 0 || b.content_type > 4) return kErrorIllegalArgument;
  switch (b.input_preset) {
    case 0: case 1: case 5: case 6: case 7: case 9: case 10: case 1997: case 1999: break;
    default: return kErrorIllegalArgument;
  }
  if (b.performance < 10 || b.performance > 13) return kErrorIllegalArgument;
  if (b.performance == 13) return kErrorUnavailable;  // no offloaded hardware path
  if (b.device_id != 0 && b.device_id !=
      (b.direction == kDirectionInput ? kInputDeviceId : kOutputDeviceId)) return kErrorUnavailable;
  if (b.direction == kDirectionInput) {
    if (b.sample_rate > 192000) return kErrorInvalidRate;  // capture engine limit
    if (b.channels > 8) return kErrorUnavailable;
  }
  return kOk;
}

aaudio_result_t ConfigureBuffers(AAudioStream* s, int64_t burst, int periods) {
  if (burst <= 0 || burst > std::numeric_limits<int32_t>::max()) return kErrorOutOfRange;
  s->burst = static_cast<int32_t>(burst);
  if (s->b.data_cb != nullptr && s->b.frames_per_callback == 0) s->b.frames_per_callback = s->burst;
  const int64_t capacity = std::max<int64_t>(s->b.capacity,
      std::max<int64_t>(burst, s->b.frames_per_callback) * periods);
  if (capacity > std::numeric_limits<int32_t>::max()) return kErrorOutOfRange;
  // A bounded ring prevents valid-looking huge counts from terminating the
  // no-exceptions audio backend during vector allocation. This is a host
  // resource limit, not an advertised Android hardware capability.
  constexpr uint64_t kMaxBufferBytes = 64u * 1024u * 1024u;
  if (s->frame_size == 0 || static_cast<uint64_t>(capacity) > kMaxBufferBytes / s->frame_size) {
    return kErrorNoMemory;
  }
  s->b.capacity = static_cast<int32_t>(capacity);
  s->b.sharing = kSharingShared;
  s->b.performance = kPerformanceNone;
  return kOk;
}

aaudio_result_t OpenInputStream(Builder* b, AAudioStream** out) {
  if (!RecordingAllowed()) {
    MEDIA_LOGE("AAudio: the app does not have android.permission.RECORD_AUDIO");
    return kErrorUnavailable;
  }
  std::unique_ptr<AAudioStream> s(new (std::nothrow) AAudioStream);
  if (s == nullptr) return kErrorNoMemory;
  s->b = *b;
  InputInfo in = GetInputInfo();
  int32_t rate = b->sample_rate > 0 ? b->sample_rate : static_cast<int32_t>(in.sample_rate);
  int32_t channels = b->channels > 0 ? b->channels : 1;
  switch (b->format) {
    case kFormatUnspecified:
    case kFormatFloat: s->pcm = PcmFormat::kFloat; s->b.format = kFormatFloat; break;
    case kFormatI16: s->pcm = PcmFormat::kS16; break;
    case kFormatI24: s->pcm = PcmFormat::kS24Packed; break;
    case kFormatI32: s->pcm = PcmFormat::kS32; break;
    default: return kErrorInvalidFormat;
  }
  s->b.sample_rate = rate;
  s->b.channels = channels;
  if (b->channels == 0 && b->channel_mask == 0) s->b.channel_mask = channels == 1 ? 0x1 : 0x3;
  s->frame_size = BytesPerSample(s->pcm) * static_cast<size_t>(channels);
  // A burst is the capture engine's period (10 ms) at the stream's rate.
  aaudio_result_t configured = ConfigureBuffers(s.get(), std::max<int32_t>(rate / 100, 64), 8);
  if (configured != kOk) return configured;
  int32_t capacity = s->b.capacity;
  if (s->b.session_id == 0) s->b.session_id = NewSessionId();
  CaptureParams p;
  p.sample_rate = static_cast<uint32_t>(rate);
  p.channels = static_cast<uint32_t>(channels);
  p.format = s->pcm;
  p.capacity_frames = static_cast<size_t>(capacity);
  p.name = "AAudio input";
  std::string error;
  s->capture = OpenCapture(p, &error);
  if (s->capture == nullptr) {
    MEDIA_LOGE("AAudio: %s", error.c_str());
    return kErrorUnavailable;
  }
  s->b.capacity = static_cast<int32_t>(s->capture->capacity_frames());
  const char* trace = getenv("GESHER_MEDIA_TRACE_RECORD");
  if (trace != nullptr && trace[0] == '1') {
    MEDIA_LOGI("AAudio input open trace: %d Hz, requested capacity %d, effective capacity %d",
               rate, b->capacity, s->b.capacity);
  }
  AAudioStream* raw = s.release();
  raw->callback_thread = std::thread([raw] { CallbackLoop(raw); });
  *out = raw;
  return kOk;
}

aaudio_result_t SYSV OpenStream(Builder* b, AAudioStream** out) {
  if (out == nullptr) return kErrorNull;
  *out = nullptr;
  if (b == nullptr) return kErrorNull;
  const aaudio_result_t validation = ValidateBuilder(*b);
  if (validation != kOk) return validation;
  if (b->direction == kDirectionInput) return OpenInputStream(b, out);
  std::unique_ptr<AAudioStream> s(new (std::nothrow) AAudioStream);
  if (s == nullptr) return kErrorNoMemory;
  s->b = *b;
  OutputInfo out_info = GetOutputInfo();
  int32_t rate = b->sample_rate > 0 ? b->sample_rate : static_cast<int32_t>(out_info.sample_rate);
  int32_t channels = b->channels > 0 ? b->channels : 2;
  switch (b->format) {
    case kFormatUnspecified:
    case kFormatFloat: s->pcm = PcmFormat::kFloat; s->b.format = kFormatFloat; break;
    case kFormatI16: s->pcm = PcmFormat::kS16; break;
    case kFormatI24: s->pcm = PcmFormat::kS24Packed; break;
    case kFormatI32: s->pcm = PcmFormat::kS32; break;
    default: return kErrorInvalidFormat;
  }
  s->b.sample_rate = rate;
  s->b.channels = channels;
  if (b->channels == 0 && b->channel_mask == 0) s->b.channel_mask = channels == 1 ? 0x1 : 0x3;
  s->frame_size = BytesPerSample(s->pcm) * static_cast<size_t>(channels);
  // A burst is the mixer's period at the stream's rate.
  const int64_t burst = std::max<int64_t>(
      static_cast<int64_t>(out_info.period_frames) * rate / std::max<uint32_t>(out_info.sample_rate, 1), 64);
  aaudio_result_t configured = ConfigureBuffers(s.get(), burst, 4);
  if (configured != kOk) return configured;
  int32_t capacity = s->b.capacity;
  if (s->b.session_id == 0) s->b.session_id = NewSessionId();  // AAUDIO_SESSION_ID_ALLOCATE
  TrackParams p;
  p.sample_rate = static_cast<uint32_t>(rate);
  p.channels = static_cast<uint32_t>(channels);
  p.format = s->pcm;
  p.stream_type = StreamForUsage(b->usage);
  p.session_id = s->b.session_id > 0 ? s->b.session_id : 0;
  p.capacity_frames = static_cast<size_t>(capacity);
  p.name = "AAudio";
  std::string error;
  s->track = CreateTrack(p, &error);
  if (s->track == nullptr) {
    MEDIA_LOGE("AAudio: %s", error.c_str());
    return kErrorInternal;
  }
  s->b.capacity = static_cast<int32_t>(s->track->buffer_capacity_frames());
  AAudioStream* raw = s.release();
  if (raw->b.data_cb != nullptr) raw->callback_thread = std::thread([raw] { CallbackLoop(raw); });
  *out = raw;
  return kOk;
}

// --- AAudioStream ---------------------------------------------------------------------

aaudio_result_t ReleaseStream(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (t_callback_stream == s) return kErrorInvalidState;
  // Serialize repeated releases while the worker is joined without mu held.
  // Queries retain their immutable format and the stopped backend object.
  std::lock_guard<std::mutex> release_lock(s->release_mu);
  {
    std::lock_guard<std::mutex> lock(s->mu);
    if (s->state == kStateClosing) return kOk;
    s->quit = true;
    ++s->generation;
    s->state = kStateClosing;
    if (s->track != nullptr) s->track->Stop();
    if (s->capture != nullptr) s->capture->Stop();
  }
  s->cv.notify_all();
  if (s->callback_thread.joinable()) s->callback_thread.join();
  if (s->track != nullptr) s->track->Release();
  if (s->capture != nullptr) s->capture->Release();
  return kOk;
}
aaudio_result_t SYSV Close(AAudioStream* s) {
  const aaudio_result_t result = ReleaseStream(s);
  if (result != kOk) return result;
  s->state = kStateClosed;
  delete s;
  return kOk;
}
aaudio_result_t SYSV Release(AAudioStream* s) {
  return ReleaseStream(s);
}
aaudio_result_t SYSV RequestStart(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (t_callback_stream == s) return kErrorInvalidState;
  {
    std::lock_guard<std::mutex> lock(s->mu);
    switch (s->state.load()) {
      case kStateDisconnected: return kErrorDisconnected;
      case kStateOpen: case kStatePausing: case kStatePaused:
      case kStateFlushing: case kStateFlushed: case kStateStopping: case kStateStopped:
        break;
      default:
        return kErrorInvalidState;
    }
    const int result = s->capture != nullptr ? s->capture->Start() : s->track->Start();
    if (result != 0) {
      if (s->capture != nullptr && result == kCaptureDeadObject) return kErrorDisconnected;
      if (s->capture != nullptr && result == kCaptureUnavailable) return kErrorUnavailable;
      return kErrorInternal;
    }
    ++s->generation;
    s->state = kStateStarted;
  }
  s->cv.notify_all();
  return kOk;
}
aaudio_result_t SYSV RequestPause(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return kErrorUnimplemented;  // AAudio has no paused inputs
  if (t_callback_stream == s) return kErrorInvalidState;
  std::lock_guard<std::mutex> lock(s->mu);
  switch (s->state.load()) {
    case kStateStarting: case kStateStarted:
      s->track->Pause();
      break;
    case kStateOpen: case kStateStopped: case kStateFlushed:
      break;
    case kStatePausing: case kStatePaused:
      return kOk;
    default:
      return kErrorInvalidState;
  }
  ++s->generation;
  s->state = kStatePaused;
  return kOk;
}
aaudio_result_t SYSV RequestFlush(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return kErrorUnimplemented;
  if (t_callback_stream == s) return kErrorInvalidState;
  std::lock_guard<std::mutex> lock(s->mu);
  if (s->state != kStatePaused && s->state != kStateOpen && s->state != kStateStopped
      && s->state != kStateFlushed) {
    return kErrorInvalidState;
  }
  s->track->Flush();
  ++s->generation;
  s->state = kStateFlushed;
  return kOk;
}
aaudio_result_t SYSV RequestStop(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (t_callback_stream == s) return kErrorInvalidState;
  std::lock_guard<std::mutex> lock(s->mu);
  switch (s->state.load()) {
    case kStateStarting: case kStateStarted:
      if (s->track != nullptr) s->track->Stop();
      if (s->capture != nullptr) s->capture->Stop();
      break;
    case kStateOpen: case kStatePaused: case kStateFlushed:
      break;
    case kStateStopping: case kStateStopped:
      return kOk;
    default:
      return kErrorInvalidState;
  }
  TraceInput(s, "stop", 0, 0);
  ++s->generation;
  s->state = kStateStopped;
  return kOk;
}
int32_t SYSV GetState(AAudioStream* s) { return s != nullptr ? s->state.load() : kStateUninitialized; }

// A timeout is a duration, including values close to INT64_MAX. Do not let
// adding the monotonic clock turn a long wait into an already-expired one.
int64_t StreamDeadline(int64_t timeout_ns) {
  const int64_t now = Now();
  if (timeout_ns <= 0) return now;
  return now > INT64_MAX - timeout_ns ? INT64_MAX : now + timeout_ns;
}

aaudio_result_t SYSV WaitForStateChange(AAudioStream* s, int32_t input, int32_t* next, int64_t timeout_ns) {
  if (s == nullptr) return kErrorNull;
  const int64_t end = StreamDeadline(timeout_ns);
  while (s->state == input && Now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (next != nullptr) *next = s->state;
  return s->state == input ? kErrorTimeout : kOk;
}

int32_t SYSV Write(AAudioStream* s, const void* data, int32_t frames, int64_t timeout_ns) {
  if (s == nullptr || data == nullptr) return kErrorNull;
  if (s->b.data_cb != nullptr) return kErrorInvalidState;
  if (frames < 0) return kErrorIllegalArgument;
  if (frames == 0) return 0;
  if (s->state == kStateClosing || s->state == kStateClosed) return kErrorInvalidState;
  if (s->track == nullptr) return kErrorInvalidState;
  const int64_t end = StreamDeadline(timeout_ns);
  const uint8_t* p = static_cast<const uint8_t*>(data);
  int32_t done = 0;
  for (;;) {
    ssize_t n = s->track->Write(p + static_cast<size_t>(done) * s->frame_size,
                                static_cast<size_t>(frames - done) * s->frame_size, false);
    if (n < 0) return kErrorInternal;
    done += static_cast<int32_t>(static_cast<size_t>(n) / s->frame_size);
    if (done >= frames || timeout_ns <= 0 || Now() >= end) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  s->written += done;
  return done;
}

int32_t SYSV Read(AAudioStream* s, void* data, int32_t frames, int64_t timeout_ns) {
  if (s == nullptr || data == nullptr) return kErrorNull;
  if (frames < 0) return kErrorIllegalArgument;
  if (frames == 0) return 0;
  if (s->state == kStateClosing || s->state == kStateClosed) return kErrorInvalidState;
  if (s->capture == nullptr || s->b.data_cb != nullptr) return kErrorInvalidState;
  const int64_t end = StreamDeadline(timeout_ns);
  uint8_t* p = static_cast<uint8_t*>(data);
  int32_t done = 0;
  for (;;) {
    ssize_t n = s->capture->Read(p + static_cast<size_t>(done) * s->frame_size,
                                 static_cast<size_t>(frames - done) * s->frame_size, false);
    if (n < 0) return done > 0 ? done : (n == kCaptureDeadObject ? kErrorDisconnected : kErrorInternal);
    done += static_cast<int32_t>(static_cast<size_t>(n) / s->frame_size);
    if (done >= frames || timeout_ns <= 0) break;
    const int64_t left = end - Now();
    if (left <= 0) break;
    // A caller can request more than the ring's capacity. Drain each burst
    // instead of waiting for an impossible full request while capture overruns.
    s->capture->WaitForData(static_cast<size_t>(std::min(frames - done, s->burst)),
                            static_cast<int>(std::min<int64_t>(left / 1000000 + 1, 100)));
  }
  TraceInput(s, "read", frames, done);
  return done;
}

aaudio_result_t SYSV SetBufferSizeInFrames(AAudioStream* s, int32_t frames) {
  if (s == nullptr) return kErrorNull;
  if (frames < 0) return kErrorOutOfRange;
  if (s->capture != nullptr) return s->b.capacity;
  return s->track->SetBufferSizeFrames(static_cast<uint32_t>(std::max(frames, 1)));
}
int32_t SYSV GetBufferSizeInFrames(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return s->b.capacity;
  return static_cast<int32_t>(s->track->buffer_size_frames());
}
int32_t SYSV GetFramesPerBurst(AAudioStream* s) { return s != nullptr ? s->burst : kErrorNull; }
int32_t SYSV GetBufferCapacityInFrames(AAudioStream* s) { return s != nullptr ? s->b.capacity : kErrorNull; }
int32_t SYSV GetFramesPerDataCallback(AAudioStream* s) {
  return s != nullptr ? s->b.frames_per_callback : kErrorNull;
}
int32_t SYSV GetXRunCount(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return static_cast<int32_t>(s->capture->overrun_count());
  return static_cast<int32_t>(s->track->underrun_count());
}
int32_t SYSV GetSampleRate(AAudioStream* s) { return s != nullptr ? s->b.sample_rate : kErrorNull; }
int32_t SYSV GetChannelCount(AAudioStream* s) { return s != nullptr ? s->b.channels : kErrorNull; }
uint32_t SYSV GetChannelMask(AAudioStream* s) {
  return s != nullptr ? s->b.channel_mask : static_cast<uint32_t>(kErrorNull);
}
// The output's, or the input's, AudioDeviceInfo id.
int32_t SYSV GetDeviceId(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  return s->capture != nullptr ? kInputDeviceId : kOutputDeviceId;
}
aaudio_result_t SYSV GetDeviceIds(AAudioStream* s, int32_t* ids, int32_t* count) {
  if (count == nullptr) return kErrorIllegalArgument;
  if (s == nullptr) return kErrorNull;
  // The virtual stream has exactly the same single device as GetDeviceId.
  // A sizing call reports the required capacity before validating the array.
  if (*count < 1) {
    *count = 1;
    return kErrorOutOfRange;
  }
  if (ids == nullptr) return kErrorIllegalArgument;
  ids[0] = GetDeviceId(s);
  *count = 1;
  return kOk;
}
int32_t SYSV GetFormat(AAudioStream* s) { return s != nullptr ? s->b.format : kErrorNull; }
int32_t SYSV GetSharingMode(AAudioStream* s) { return s != nullptr ? s->b.sharing : kErrorNull; }
int32_t SYSV GetPerformanceMode(AAudioStream* s) { return s != nullptr ? s->b.performance : kErrorNull; }
int32_t SYSV GetDirection(AAudioStream* s) { return s != nullptr ? s->b.direction : kErrorNull; }
int64_t SYSV GetFramesWritten(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return static_cast<int64_t>(s->capture->frames_captured());
  return s->written.load();
}
int64_t SYSV GetFramesRead(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  if (s->capture != nullptr) return static_cast<int64_t>(s->capture->frames_read());
  return s->track->Position();
}
int32_t SYSV GetSessionId(AAudioStream* s) { return s != nullptr ? s->b.session_id : kErrorNull; }
int32_t SYSV GetUsage(AAudioStream* s) { return s != nullptr ? s->b.usage : kErrorNull; }
int32_t SYSV GetContentType(AAudioStream* s) { return s != nullptr ? s->b.content_type : kErrorNull; }
int32_t SYSV GetInputPreset(AAudioStream* s) { return s != nullptr ? s->b.input_preset : kErrorNull; }
int32_t SYSV GetZero(AAudioStream*) { return 0; }
bool SYSV GetFalse(AAudioStream*) { return false; }

aaudio_result_t SYSV GetTimestamp(AAudioStream* s, int32_t clockid, int64_t* frames, int64_t* ns) {
  if (s == nullptr || frames == nullptr || ns == nullptr) return kErrorNull;
  // Guest Linux IDs. The media engine timestamps use Android MONOTONIC. A
  // suspend-aware BOOTTIME translation is not yet available; do not silently
  // return a timestamp from a different clock domain.
  if (clockid == 7) return kErrorUnimplemented;
  if (clockid != 1) return kErrorIllegalArgument;
  if (s->state != kStateStarted) return kErrorInvalidState;
  int64_t position = 0, timestamp = 0;
  const bool have = s->capture != nullptr ? s->capture->Timestamp(&position, &timestamp)
                                        : s->track->Timestamp(&position, &timestamp);
  if (!have) return kErrorInvalidState;
  *frames = position;
  *ns = timestamp;
  return kOk;
}

const char* SYSV ResultToText(aaudio_result_t r) {
  switch (r) {
    case kOk: return "AAUDIO_OK";
    case kErrorDisconnected: return "AAUDIO_ERROR_DISCONNECTED";
    case kErrorIllegalArgument: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case kErrorInternal: return "AAUDIO_ERROR_INTERNAL";
    case kErrorInvalidState: return "AAUDIO_ERROR_INVALID_STATE";
    case kErrorUnimplemented: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case kErrorUnavailable: return "AAUDIO_ERROR_UNAVAILABLE";
    case kErrorNoMemory: return "AAUDIO_ERROR_NO_MEMORY";
    case kErrorNull: return "AAUDIO_ERROR_NULL";
    case kErrorTimeout: return "AAUDIO_ERROR_TIMEOUT";
    case kErrorInvalidFormat: return "AAUDIO_ERROR_INVALID_FORMAT";
    case kErrorOutOfRange: return "AAUDIO_ERROR_OUT_OF_RANGE";
    case kErrorInvalidRate: return "AAUDIO_ERROR_INVALID_RATE";
    default: return "Unrecognized AAudio error";
  }
}

const char* SYSV StateToText(int32_t state) {
  static const char* const kNames[] = {
      "AAUDIO_STREAM_STATE_UNINITIALIZED", "AAUDIO_STREAM_STATE_UNKNOWN", "AAUDIO_STREAM_STATE_OPEN",
      "AAUDIO_STREAM_STATE_STARTING", "AAUDIO_STREAM_STATE_STARTED", "AAUDIO_STREAM_STATE_PAUSING",
      "AAUDIO_STREAM_STATE_PAUSED", "AAUDIO_STREAM_STATE_FLUSHING", "AAUDIO_STREAM_STATE_FLUSHED",
      "AAUDIO_STREAM_STATE_STOPPING", "AAUDIO_STREAM_STATE_STOPPED", "AAUDIO_STREAM_STATE_CLOSING",
      "AAUDIO_STREAM_STATE_CLOSED", "AAUDIO_STREAM_STATE_DISCONNECTED"};
  return state >= 0 && state < 14 ? kNames[state] : "Unrecognized AAudio state";
}

void Register() {
  using gesher::bionic::AddBionicSymbol;
#define E(name, fn) AddBionicSymbol(name, reinterpret_cast<void*>(&fn))
  E("AAudio_createStreamBuilder", CreateStreamBuilder);
  E("AAudio_convertResultToText", ResultToText);
  E("AAudio_convertStreamStateToText", StateToText);
  E("AAudioStreamBuilder_setDeviceId", SetDeviceId);
  E("AAudioStreamBuilder_setSampleRate", SetSampleRate);
  E("AAudioStreamBuilder_setChannelCount", SetChannelCount);
  E("AAudioStreamBuilder_setSamplesPerFrame", SetChannelCount);
  E("AAudioStreamBuilder_setChannelMask", SetChannelMask);
  E("AAudioStreamBuilder_setFormat", SetFormat);
  E("AAudioStreamBuilder_setSharingMode", SetSharingMode);
  E("AAudioStreamBuilder_setDirection", SetDirection);
  E("AAudioStreamBuilder_setBufferCapacityInFrames", SetBufferCapacityInFrames);
  E("AAudioStreamBuilder_setPerformanceMode", SetPerformanceMode);
  E("AAudioStreamBuilder_setUsage", SetUsage);
  E("AAudioStreamBuilder_setContentType", SetContentType);
  E("AAudioStreamBuilder_setInputPreset", SetInputPreset);
  E("AAudioStreamBuilder_setSessionId", SetSessionId);
  E("AAudioStreamBuilder_setFramesPerDataCallback", SetFramesPerDataCallback);
  E("AAudioStreamBuilder_setAllowedCapturePolicy", SetIgnored);
  E("AAudioStreamBuilder_setPrivacySensitive", SetIgnored);
  E("AAudioStreamBuilder_setSpatializationBehavior", SetIgnored);
  E("AAudioStreamBuilder_setIsContentSpatialized", SetIgnored);
  E("AAudioStreamBuilder_setPackageName", SetIgnoredString);
  E("AAudioStreamBuilder_setAttributionTag", SetIgnoredString);
  E("AAudioStreamBuilder_setDataCallback", SetDataCallback);
  E("AAudioStreamBuilder_setErrorCallback", SetErrorCallback);
  E("AAudioStreamBuilder_openStream", OpenStream);
  E("AAudioStreamBuilder_delete", BuilderDelete);
  E("AAudioStream_release", Release);
  E("AAudioStream_close", Close);
  E("AAudioStream_requestStart", RequestStart);
  E("AAudioStream_requestPause", RequestPause);
  E("AAudioStream_requestFlush", RequestFlush);
  E("AAudioStream_requestStop", RequestStop);
  E("AAudioStream_getState", GetState);
  E("AAudioStream_waitForStateChange", WaitForStateChange);
  E("AAudioStream_read", Read);
  E("AAudioStream_write", Write);
  E("AAudioStream_setBufferSizeInFrames", SetBufferSizeInFrames);
  E("AAudioStream_getBufferSizeInFrames", GetBufferSizeInFrames);
  E("AAudioStream_getFramesPerBurst", GetFramesPerBurst);
  E("AAudioStream_getBufferCapacityInFrames", GetBufferCapacityInFrames);
  E("AAudioStream_getFramesPerDataCallback", GetFramesPerDataCallback);
  E("AAudioStream_getXRunCount", GetXRunCount);
  E("AAudioStream_getSampleRate", GetSampleRate);
  E("AAudioStream_getChannelCount", GetChannelCount);
  E("AAudioStream_getSamplesPerFrame", GetChannelCount);
  E("AAudioStream_getDeviceId", GetDeviceId);
  E("AAudioStream_getDeviceIds", GetDeviceIds);
  E("AAudioStream_getFormat", GetFormat);
  E("AAudioStream_getSharingMode", GetSharingMode);
  E("AAudioStream_getPerformanceMode", GetPerformanceMode);
  E("AAudioStream_getDirection", GetDirection);
  E("AAudioStream_getFramesWritten", GetFramesWritten);
  E("AAudioStream_getFramesRead", GetFramesRead);
  E("AAudioStream_getSessionId", GetSessionId);
  E("AAudioStream_getTimestamp", GetTimestamp);
  E("AAudioStream_getUsage", GetUsage);
  E("AAudioStream_getContentType", GetContentType);
  E("AAudioStream_getInputPreset", GetInputPreset);
  E("AAudioStream_getAllowedCapturePolicy", GetZero);
  E("AAudioStream_getSpatializationBehavior", GetZero);
  E("AAudioStream_getChannelMask", GetChannelMask);
  E("AAudioStream_isContentSpatialized", GetFalse);
  E("AAudioStream_isPrivacySensitive", GetFalse);
#undef E
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
