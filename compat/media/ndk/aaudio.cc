// Gesher: the NDK's AAudio (libaaudio.so) for an app's ELF libraries
// (docs/media.md), over the audio engine: an output stream is an engine
// track; a stream with a data callback has a thread that calls it whenever
// the track has room for a burst. Input streams wait for stage 3 (capture):
// opening one answers AAUDIO_ERROR_UNAVAILABLE.
//
// The functions are System V entries the .so's imports bind to, added to the
// bionic shim's table by a static initializer (as libandroid's are,
// docs/native-libs.md); the callbacks the .so passes are System V too, called
// on threads set up for loaded code (EnterLoadedCode).

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <gesher/bionic_shim.h>

#include "../audio/engine.h"
#include "../audio/log.h"
#include "../audio/shared_state.h"
#include "jni_bridge.h"

#define SYSV __attribute__((sysv_abi))

namespace gesher::media {

struct AAudioStream;

namespace {

// aaudio/AAudio.h
using aaudio_result_t = int32_t;
constexpr aaudio_result_t kOk = 0;
constexpr aaudio_result_t kErrorIllegalArgument = -898;
constexpr aaudio_result_t kErrorInternal = -896;
constexpr aaudio_result_t kErrorInvalidState = -895;
constexpr aaudio_result_t kErrorUnimplemented = -890;
constexpr aaudio_result_t kErrorUnavailable = -889;
constexpr aaudio_result_t kErrorNull = -886;
constexpr aaudio_result_t kErrorTimeout = -885;
constexpr aaudio_result_t kErrorInvalidFormat = -883;

enum : int32_t { kDirectionOutput = 0, kDirectionInput = 1 };
enum : int32_t { kFormatUnspecified = 0, kFormatI16 = 1, kFormatFloat = 2, kFormatI24 = 3, kFormatI32 = 4 };
enum : int32_t {
  kStateUninitialized = 0, kStateOpen = 2, kStateStarting = 3, kStateStarted = 4,
  kStatePausing = 5, kStatePaused = 6, kStateFlushing = 7, kStateFlushed = 8,
  kStateStopping = 9, kStateStopped = 10, kStateClosing = 11, kStateClosed = 12,
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

}  // namespace

struct AAudioStream {
  Builder b;
  std::shared_ptr<Track> track;
  PcmFormat pcm;
  size_t frame_size;
  int32_t burst;
  std::atomic<int32_t> state{kStateOpen};
  std::atomic<int64_t> written{0};
  std::mutex mu;
  std::condition_variable cv;
  std::thread callback_thread;
  bool quit = false;
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
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

void CallbackLoop(AAudioStream* s) {
  gesher::bionic::EnterLoadedCode();
  std::vector<uint8_t> buf(static_cast<size_t>(s->burst) * s->frame_size);
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(s->mu);
      s->cv.wait(lock, [s] { return s->quit || s->state == kStateStarted; });
      if (s->quit) return;
    }
    size_t room = s->track->buffer_capacity_frames() - std::min<size_t>(s->track->Queued(),
                                                                          s->track->buffer_capacity_frames());
    if (room < static_cast<size_t>(s->burst)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    int32_t r = s->b.data_cb(s, s->b.data_user, buf.data(), s->burst);
    s->track->Write(buf.data(), buf.size(), false);
    s->written += s->burst;
    if (r != kCallbackContinue) {
      s->track->Stop();
      s->state = kStateStopped;
    }
  }
}

// --- AAudioStreamBuilder --------------------------------------------------------------

aaudio_result_t SYSV CreateStreamBuilder(Builder** out) {
  if (out == nullptr) return kErrorNull;
  *out = new Builder();
  return kOk;
}
#define SETTER(Name, field)                                          \
  void SYSV Name(Builder* b, int32_t v) {                           \
    if (b != nullptr) b->field = v;                                 \
  }
SETTER(SetDeviceId, device_id)
SETTER(SetSampleRate, sample_rate)
SETTER(SetChannelCount, channels)
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
void SYSV SetChannelMask(Builder* b, uint32_t mask) {
  if (b != nullptr) b->channels = __builtin_popcount(mask);
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

aaudio_result_t SYSV OpenStream(Builder* b, AAudioStream** out) {
  if (b == nullptr || out == nullptr) return kErrorNull;
  if (b->direction == kDirectionInput) {
    MEDIA_LOGW("AAudio: no input streams yet (audio capture is stage 3)");
    return kErrorUnavailable;
  }
  auto s = std::make_unique<AAudioStream>();
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
  s->frame_size = BytesPerSample(s->pcm) * static_cast<size_t>(channels);
  // A burst is the mixer's period at the stream's rate.
  s->burst = b->frames_per_callback > 0
                 ? b->frames_per_callback
                 : std::max<int32_t>(static_cast<int32_t>(static_cast<int64_t>(out_info.period_frames) *
                                                          rate / std::max<uint32_t>(out_info.sample_rate, 1)),
                                     64);
  int32_t capacity = std::max(b->capacity > 0 ? b->capacity : 0, s->burst * 4);
  if (s->b.session_id == 0) s->b.session_id = NewSessionId();  // AAUDIO_SESSION_ID_ALLOCATE
  TrackParams p;
  p.sample_rate = static_cast<uint32_t>(rate);
  p.channels = static_cast<uint32_t>(channels);
  p.format = s->pcm;
  p.stream_type = StreamForUsage(b->usage);
  p.capacity_frames = static_cast<size_t>(capacity);
  p.name = "AAudio";
  std::string error;
  s->track = CreateTrack(p, &error);
  if (s->track == nullptr) {
    MEDIA_LOGE("AAudio: %s", error.c_str());
    return kErrorInternal;
  }
  s->b.capacity = capacity;
  AAudioStream* raw = s.release();
  if (raw->b.data_cb != nullptr) raw->callback_thread = std::thread([raw] { CallbackLoop(raw); });
  *out = raw;
  return kOk;
}

// --- AAudioStream ---------------------------------------------------------------------

aaudio_result_t SYSV Close(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  {
    std::lock_guard<std::mutex> lock(s->mu);
    s->quit = true;
    s->state = kStateClosed;
  }
  s->cv.notify_all();
  if (s->callback_thread.joinable()) s->callback_thread.join();
  s->track->Release();
  delete s;
  return kOk;
}
aaudio_result_t SYSV Release(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  s->track->Stop();
  s->state = kStateClosing;
  return kOk;
}
aaudio_result_t SYSV RequestStart(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  {
    std::lock_guard<std::mutex> lock(s->mu);
    s->track->Start();
    s->state = kStateStarted;
  }
  s->cv.notify_all();
  return kOk;
}
aaudio_result_t SYSV RequestPause(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  std::lock_guard<std::mutex> lock(s->mu);
  s->track->Pause();
  s->state = kStatePaused;
  return kOk;
}
aaudio_result_t SYSV RequestFlush(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  std::lock_guard<std::mutex> lock(s->mu);
  if (s->state != kStatePaused && s->state != kStateOpen && s->state != kStateStopped) {
    return kErrorInvalidState;
  }
  s->track->Flush();
  s->state = kStateFlushed;
  return kOk;
}
aaudio_result_t SYSV RequestStop(AAudioStream* s) {
  if (s == nullptr) return kErrorNull;
  std::lock_guard<std::mutex> lock(s->mu);
  s->track->Stop();
  s->state = kStateStopped;
  return kOk;
}
int32_t SYSV GetState(AAudioStream* s) { return s != nullptr ? s->state.load() : kStateUninitialized; }

aaudio_result_t SYSV WaitForStateChange(AAudioStream* s, int32_t input, int32_t* next, int64_t timeout_ns) {
  if (s == nullptr) return kErrorNull;
  int64_t end = Now() + timeout_ns;
  while (s->state == input && Now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  if (next != nullptr) *next = s->state;
  return s->state == input ? kErrorTimeout : kOk;
}

int32_t SYSV Write(AAudioStream* s, const void* data, int32_t frames, int64_t timeout_ns) {
  if (s == nullptr) return kErrorNull;
  if (s->b.data_cb != nullptr) return kErrorInvalidState;
  int64_t end = Now() + timeout_ns;
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

int32_t SYSV Read(AAudioStream*, void*, int32_t, int64_t) { return kErrorUnimplemented; }

aaudio_result_t SYSV SetBufferSizeInFrames(AAudioStream* s, int32_t frames) {
  if (s == nullptr) return kErrorNull;
  return s->track->SetBufferSizeFrames(static_cast<uint32_t>(std::max(frames, 1)));
}
int32_t SYSV GetBufferSizeInFrames(AAudioStream* s) {
  return s != nullptr ? static_cast<int32_t>(s->track->buffer_size_frames()) : kErrorNull;
}
int32_t SYSV GetFramesPerBurst(AAudioStream* s) { return s != nullptr ? s->burst : kErrorNull; }
int32_t SYSV GetBufferCapacityInFrames(AAudioStream* s) { return s != nullptr ? s->b.capacity : kErrorNull; }
int32_t SYSV GetFramesPerDataCallback(AAudioStream* s) {
  return s != nullptr ? s->b.frames_per_callback : kErrorNull;
}
int32_t SYSV GetXRunCount(AAudioStream* s) {
  return s != nullptr && s->track->underrun_frames() > 0 ? 1 : 0;
}
int32_t SYSV GetSampleRate(AAudioStream* s) { return s != nullptr ? s->b.sample_rate : kErrorNull; }
int32_t SYSV GetChannelCount(AAudioStream* s) { return s != nullptr ? s->b.channels : kErrorNull; }
int32_t SYSV GetDeviceId(AAudioStream*) { return 2; }  // the output's AudioDeviceInfo id
int32_t SYSV GetFormat(AAudioStream* s) { return s != nullptr ? s->b.format : kErrorNull; }
int32_t SYSV GetSharingMode(AAudioStream*) { return kSharingShared; }
int32_t SYSV GetPerformanceMode(AAudioStream* s) { return s != nullptr ? s->b.performance : kErrorNull; }
int32_t SYSV GetDirection(AAudioStream* s) { return s != nullptr ? s->b.direction : kErrorNull; }
int64_t SYSV GetFramesWritten(AAudioStream* s) { return s != nullptr ? s->written.load() : kErrorNull; }
int64_t SYSV GetFramesRead(AAudioStream* s) { return s != nullptr ? s->track->Position() : kErrorNull; }
int32_t SYSV GetSessionId(AAudioStream* s) { return s != nullptr ? s->b.session_id : kErrorNull; }
int32_t SYSV GetUsage(AAudioStream* s) { return s != nullptr ? s->b.usage : kErrorNull; }
int32_t SYSV GetContentType(AAudioStream* s) { return s != nullptr ? s->b.content_type : kErrorNull; }
int32_t SYSV GetInputPreset(AAudioStream* s) { return s != nullptr ? s->b.input_preset : kErrorNull; }
int32_t SYSV GetZero(AAudioStream*) { return 0; }
bool SYSV GetFalse(AAudioStream*) { return false; }

aaudio_result_t SYSV GetTimestamp(AAudioStream* s, int32_t /*clockid*/, int64_t* frames, int64_t* ns) {
  if (s == nullptr || frames == nullptr || ns == nullptr) return kErrorNull;
  if (s->state != kStateStarted) return kErrorInvalidState;
  return s->track->Timestamp(frames, ns) ? kOk : kErrorInvalidState;
}

const char* SYSV ResultToText(aaudio_result_t r) {
  switch (r) {
    case kOk: return "AAUDIO_OK";
    case kErrorIllegalArgument: return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case kErrorInternal: return "AAUDIO_ERROR_INTERNAL";
    case kErrorInvalidState: return "AAUDIO_ERROR_INVALID_STATE";
    case kErrorUnimplemented: return "AAUDIO_ERROR_UNIMPLEMENTED";
    case kErrorUnavailable: return "AAUDIO_ERROR_UNAVAILABLE";
    case kErrorNull: return "AAUDIO_ERROR_NULL";
    case kErrorTimeout: return "AAUDIO_ERROR_TIMEOUT";
    case kErrorInvalidFormat: return "AAUDIO_ERROR_INVALID_FORMAT";
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
  E("AAudioStream_getChannelMask", GetZero);
  E("AAudioStream_isContentSpatialized", GetFalse);
  E("AAudioStream_isPrivacySensitive", GetFalse);
#undef E
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
