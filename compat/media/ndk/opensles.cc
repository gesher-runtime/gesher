// Gesher: the NDK's OpenSL ES (libOpenSLES.so) for an app's ELF libraries
// (docs/media.md), the part Android's implementation (frameworks/wilhelm)
// gives apps that games and audio libraries use:
//
//  - the engine object and SLEngineItf (CreateOutputMix, CreateAudioPlayer;
//    CreateAudioRecorder answers SL_RESULT_FEATURE_UNSUPPORTED until stage 3);
//  - the output mix object (realized; no environmental reverb);
//  - audio players from an Android simple buffer queue or a buffer queue of
//    PCM (SLDataFormat_PCM, SLAndroidDataFormat_PCM_EX: 8/16/24/32-bit, float),
//    an engine track fed from the queued buffers, the queue's callback called
//    as each buffer is taken (as wilhelm does from AudioTrack's callback);
//  - audio players from an Android file descriptor or a URI (a path or
//    http(s)), through the player under MediaPlayer;
//  - SLPlayItf (states, position, duration, head-at-end), SLVolumeItf (level in
//    millibels, mute), SLSeekItf (position, loop), SLAndroidConfigurationItf
//    (the stream type), SLPrefetchStatusItf (always filled).
//
// Every function is a System V entry the .so calls through its vtables or its
// imports (SLAPIENTRY is empty on Android); the interface IDs are data symbols
// the .so's imports bind to, compared by address. Callbacks run on threads set
// up for loaded code.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gesher/bionic_shim.h>

#include "../audio/engine.h"
#include "../audio/log.h"
#include "../player/data_source.h"
#include "../player/player.h"
#include "jni_bridge.h"

#define SYSV __attribute__((sysv_abi))

namespace gesher::media {
namespace {

// --- OpenSLES.h, OpenSLES_Android.h ----------------------------------------------------

using SLuint32 = uint32_t;
using SLint32 = int32_t;
using SLresult = uint32_t;
using SLboolean = uint32_t;
using SLmillibel = int16_t;
using SLmillisecond = uint32_t;
using SLpermille = int16_t;

constexpr SLresult kSuccess = 0;
constexpr SLresult kPreconditionsViolated = 1;
constexpr SLresult kParameterInvalid = 2;
constexpr SLresult kBufferInsufficient = 7;
constexpr SLresult kContentUnsupported = 9;
constexpr SLresult kFeatureUnsupported = 12;

constexpr SLuint32 kObjectUnrealized = 1, kObjectRealized = 2;
constexpr SLuint32 kPlayStopped = 1, kPlayPaused = 2, kPlayPlaying = 3;
constexpr SLuint32 kPlayEventHeadAtEnd = 0x1;
constexpr SLuint32 kLocatorUri = 1, kLocatorOutputMix = 4, kLocatorBufferQueue = 6,
                   kLocatorAndroidFd = 0x800007BC, kLocatorAndroidSimpleBufferQueue = 0x800007BD;
constexpr SLuint32 kFormatPcm = 2, kFormatPcmEx = 4;
constexpr SLuint32 kPcmRepresentationFloat = 3;
constexpr SLuint32 kPrefetchStatusSufficientData = 3;

struct SLInterfaceID_ {
  SLuint32 time_low;
  uint16_t time_mid, time_hi_and_version, clock_seq;
  uint8_t node[6];
};
using SLInterfaceID = const SLInterfaceID_*;

// The interface IDs, by address (their contents only tell them apart in dumps).
#define IID(var, n) const SLInterfaceID_ var##_data = {0x67657300u + n, 0x6d65, 0x6469, 0x6100, {0, 0, 0, 0, 0, n}}; \
  const SLInterfaceID var = &var##_data
IID(kIidNull, 0);
IID(kIidObject, 1);
IID(kIidEngine, 2);
IID(kIidPlay, 3);
IID(kIidBufferQueue, 4);
IID(kIidAndroidSimpleBufferQueue, 5);
IID(kIidVolume, 6);
IID(kIidSeek, 7);
IID(kIidAndroidConfiguration, 8);
IID(kIidPrefetchStatus, 9);
IID(kIidPlaybackRate, 10);
IID(kIidEffectSend, 11);
IID(kIidEnvironmentalReverb, 12);
IID(kIidRecord, 13);
IID(kIidEngineCapabilities, 14);
IID(kIidMetadataExtraction, 15);
IID(kIidMuteSolo, 16);
IID(kIidAndroidEffect, 17);
IID(kIidDynamicInterfaceManagement, 18);
#undef IID

struct DataLocatorHeader { SLuint32 type; };
struct DataLocatorUri { SLuint32 type; const char* uri; };
struct DataLocatorAndroidFd { SLuint32 type; SLint32 fd; int64_t offset; int64_t length; };
struct DataLocatorQueue { SLuint32 type; SLuint32 num_buffers; };
struct DataFormatPcm {
  SLuint32 format_type, num_channels, samples_per_sec /* milliHz */, bits_per_sample, container_size,
      channel_mask, endianness;
};
struct DataFormatPcmEx {
  SLuint32 format_type, num_channels, sample_rate, bits_per_sample, container_size, channel_mask,
      endianness, representation;
};
struct SLDataSource { void* locator; void* format; };
struct SLDataSink { void* locator; void* format; };

struct Object;
using Itf = const void* const*;  // a pointer to a pointer to a vtable

// Every interface an object hands out: its vtable pointer first, then its object.
struct ItfSlot {
  const void* vtable;
  Object* object;
};

Object* ObjectOf(const void* self) {
  return reinterpret_cast<const ItfSlot*>(self)->object;
}

// --- Objects ----------------------------------------------------------------------------

enum class Kind { kEngine, kOutputMix, kPlayer };

struct Object {
  Kind kind;
  ItfSlot object_itf;
  ItfSlot engine_itf;
  ItfSlot play_itf;
  ItfSlot queue_itf;  // both buffer queue kinds share one vtable
  ItfSlot volume_itf;
  ItfSlot seek_itf;
  ItfSlot config_itf;
  ItfSlot prefetch_itf;
  SLuint32 state = kObjectUnrealized;

  // Players.
  bool queue_player = false;
  SLuint32 play_state = kPlayStopped;
  std::shared_ptr<Track> track;
  size_t frame_size = 1;
  uint32_t rate = 0;
  int stream = 3;
  SLuint32 queue_capacity = 0;
  struct Buffer { const uint8_t* data; size_t size; size_t done; };
  std::deque<Buffer> queue;
  SLuint32 queue_index = 0;  // buffers taken so far
  void(SYSV* queue_cb)(Itf, void*) = nullptr;
  void* queue_ctx = nullptr;
  void(SYSV* play_cb)(Itf, void*, SLuint32) = nullptr;
  void* play_ctx = nullptr;
  SLuint32 event_mask = 0;
  SLmillibel level = 0;
  bool mute = false;
  std::shared_ptr<DataSource> source;  // fd/URI players
  std::shared_ptr<Player> player;
  std::atomic<bool> at_end{false};
  std::mutex mu;
  std::condition_variable cv;
  std::thread feeder;
  bool quit = false;
};

void ApplyVolume(Object* o) {
  float gain = o->mute ? 0.0f : static_cast<float>(std::pow(10.0, o->level / 2000.0));
  if (o->track != nullptr) o->track->SetVolume(gain, gain);
  if (o->player != nullptr) o->player->SetVolume(gain, gain);
}

// The feeder of a buffer-queue player: copies queued buffers into the track,
// calls the queue's callback as each is taken.
void FeedLoop(Object* o) {
  gesher::bionic::EnterLoadedCode();
  RaiseAudioThreadPriority(true);
  for (;;) {
    void(SYSV* cb)(Itf, void*) = nullptr;
    void* ctx = nullptr;
    {
      std::unique_lock<std::mutex> lock(o->mu);
      o->cv.wait_for(lock, std::chrono::milliseconds(2),
                     [o] { return o->quit || (o->play_state == kPlayPlaying && !o->queue.empty()); });
      if (o->quit) return;
      if (o->play_state != kPlayPlaying || o->queue.empty()) continue;
      Object::Buffer& b = o->queue.front();
      ssize_t n = o->track->Write(b.data + b.done, b.size - b.done, false);
      if (n > 0) b.done += static_cast<size_t>(n);
      if (b.done >= b.size) {
        o->queue.pop_front();
        o->queue_index++;
        cb = o->queue_cb;
        ctx = o->queue_ctx;
      }
    }
    if (cb != nullptr) {
      cb(reinterpret_cast<Itf>(&o->queue_itf), ctx);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

// --- SLObjectItf ------------------------------------------------------------------------

SLresult SYSV ObjRealize(const void* self, SLboolean) {
  Object* o = ObjectOf(self);
  o->state = kObjectRealized;
  if (o->kind == Kind::kPlayer && o->queue_player && !o->feeder.joinable()) {
    o->feeder = std::thread([o] { FeedLoop(o); });
  }
  return kSuccess;
}
SLresult SYSV ObjResume(const void*, SLboolean) { return kSuccess; }
SLresult SYSV ObjGetState(const void* self, SLuint32* state) {
  if (state == nullptr) return kParameterInvalid;
  *state = ObjectOf(self)->state;
  return kSuccess;
}

SLresult SYSV ObjGetInterface(const void* self, SLInterfaceID iid, void* out) {
  Object* o = ObjectOf(self);
  if (out == nullptr) return kParameterInvalid;
  if (o->state != kObjectRealized) return kPreconditionsViolated;
  ItfSlot* slot = nullptr;
  if (iid == kIidObject) slot = &o->object_itf;
  if (o->kind == Kind::kEngine && iid == kIidEngine) slot = &o->engine_itf;
  if (o->kind == Kind::kPlayer) {
    if (iid == kIidPlay) slot = &o->play_itf;
    if (iid == kIidVolume) slot = &o->volume_itf;
    if (iid == kIidAndroidConfiguration) slot = &o->config_itf;
    if (o->queue_player && (iid == kIidBufferQueue || iid == kIidAndroidSimpleBufferQueue)) {
      slot = &o->queue_itf;
    }
    if (!o->queue_player && iid == kIidSeek) slot = &o->seek_itf;
    if (!o->queue_player && iid == kIidPrefetchStatus) slot = &o->prefetch_itf;
  }
  if (slot == nullptr) return kFeatureUnsupported;
  *static_cast<Itf*>(out) = reinterpret_cast<Itf>(slot);
  return kSuccess;
}

SLresult SYSV ObjRegisterCallback(const void*, void*, void*) { return kSuccess; }
void SYSV ObjAbort(const void*) {}

void SYSV ObjDestroy(const void* self) {
  Object* o = ObjectOf(self);
  {
    std::lock_guard<std::mutex> lock(o->mu);
    o->quit = true;
  }
  o->cv.notify_all();
  if (o->feeder.joinable()) o->feeder.join();
  if (o->track != nullptr) o->track->Release();
  if (o->player != nullptr) o->player->Release();
  delete o;
}

SLresult SYSV ObjSetPriority(const void*, SLint32, SLboolean) { return kSuccess; }
SLresult SYSV ObjGetPriority(const void*, SLint32* p, SLboolean* pre) {
  if (p != nullptr) *p = 0;
  if (pre != nullptr) *pre = 0;
  return kSuccess;
}
SLresult SYSV ObjSetLossOfControl(const void*, SLint32, const SLInterfaceID*, SLboolean) {
  return kSuccess;
}

const void* const kObjectVtable[] = {
    reinterpret_cast<const void*>(&ObjRealize),     reinterpret_cast<const void*>(&ObjResume),
    reinterpret_cast<const void*>(&ObjGetState),    reinterpret_cast<const void*>(&ObjGetInterface),
    reinterpret_cast<const void*>(&ObjRegisterCallback), reinterpret_cast<const void*>(&ObjAbort),
    reinterpret_cast<const void*>(&ObjDestroy),     reinterpret_cast<const void*>(&ObjSetPriority),
    reinterpret_cast<const void*>(&ObjGetPriority), reinterpret_cast<const void*>(&ObjSetLossOfControl),
};

Object* NewObject(Kind kind);

// --- SLPlayItf --------------------------------------------------------------------------

SLresult SYSV PlaySetState(const void* self, SLuint32 state) {
  Object* o = ObjectOf(self);
  if (state < kPlayStopped || state > kPlayPlaying) return kParameterInvalid;
  std::lock_guard<std::mutex> lock(o->mu);
  if (o->queue_player) {
    if (state == kPlayPlaying) {
      o->track->Start();
    } else if (state == kPlayPaused) {
      o->track->Pause();
    } else {
      // Stopped: the queue is cleared (wilhelm), the track starts over.
      o->track->Pause();
      o->track->Flush();
      o->queue.clear();
    }
  } else if (o->player != nullptr) {
    if (state == kPlayPlaying) {
      if (o->at_end.exchange(false)) o->player->SeekTo(0, 0);
      o->player->Start();
    } else if (state == kPlayPaused) {
      o->player->Pause();
    } else {
      o->player->Pause();
      o->player->SeekTo(0, 0);
    }
  }
  o->play_state = state;
  o->cv.notify_all();
  return kSuccess;
}

SLresult SYSV PlayGetState(const void* self, SLuint32* state) {
  if (state == nullptr) return kParameterInvalid;
  *state = ObjectOf(self)->play_state;
  return kSuccess;
}

SLresult SYSV PlayGetDuration(const void* self, SLmillisecond* msec) {
  if (msec == nullptr) return kParameterInvalid;
  Object* o = ObjectOf(self);
  int d = -1;
  if (o->player != nullptr) o->player->GetDuration(&d);
  *msec = d >= 0 ? static_cast<SLmillisecond>(d) : 0xFFFFFFFF;  // SL_TIME_UNKNOWN
  return kSuccess;
}

SLresult SYSV PlayGetPosition(const void* self, SLmillisecond* msec) {
  if (msec == nullptr) return kParameterInvalid;
  Object* o = ObjectOf(self);
  if (o->queue_player) {
    *msec = o->rate != 0 ? static_cast<SLmillisecond>(static_cast<uint64_t>(o->track->Position()) * 1000 / o->rate) : 0;
  } else {
    int p = 0;
    if (o->player != nullptr) o->player->GetCurrentPosition(&p);
    *msec = static_cast<SLmillisecond>(std::max(p, 0));
  }
  return kSuccess;
}

SLresult SYSV PlayRegisterCallback(const void* self, void(SYSV* cb)(Itf, void*, SLuint32), void* ctx) {
  Object* o = ObjectOf(self);
  std::lock_guard<std::mutex> lock(o->mu);
  o->play_cb = cb;
  o->play_ctx = ctx;
  return kSuccess;
}
SLresult SYSV PlaySetEventsMask(const void* self, SLuint32 mask) {
  ObjectOf(self)->event_mask = mask;
  return kSuccess;
}
SLresult SYSV PlayGetEventsMask(const void* self, SLuint32* mask) {
  if (mask == nullptr) return kParameterInvalid;
  *mask = ObjectOf(self)->event_mask;
  return kSuccess;
}
SLresult SYSV PlayMarker(const void*, SLmillisecond) { return kSuccess; }
SLresult SYSV PlayClearMarker(const void*) { return kSuccess; }
SLresult SYSV PlayGetMarker(const void*, SLmillisecond* m) {
  if (m != nullptr) *m = 0;
  return kPreconditionsViolated;
}
SLresult SYSV PlaySetPeriod(const void*, SLmillisecond) { return kSuccess; }
SLresult SYSV PlayGetPeriod(const void*, SLmillisecond* m) {
  if (m != nullptr) *m = 1000;
  return kSuccess;
}

const void* const kPlayVtable[] = {
    reinterpret_cast<const void*>(&PlaySetState),     reinterpret_cast<const void*>(&PlayGetState),
    reinterpret_cast<const void*>(&PlayGetDuration),  reinterpret_cast<const void*>(&PlayGetPosition),
    reinterpret_cast<const void*>(&PlayRegisterCallback), reinterpret_cast<const void*>(&PlaySetEventsMask),
    reinterpret_cast<const void*>(&PlayGetEventsMask), reinterpret_cast<const void*>(&PlayMarker),
    reinterpret_cast<const void*>(&PlayClearMarker),  reinterpret_cast<const void*>(&PlayGetMarker),
    reinterpret_cast<const void*>(&PlaySetPeriod),    reinterpret_cast<const void*>(&PlayGetPeriod),
};

// --- Buffer queues ----------------------------------------------------------------------

SLresult SYSV QueueEnqueue(const void* self, const void* buffer, SLuint32 size) {
  Object* o = ObjectOf(self);
  if (buffer == nullptr || size == 0) return kParameterInvalid;
  std::lock_guard<std::mutex> lock(o->mu);
  if (o->queue.size() >= o->queue_capacity) return kBufferInsufficient;
  o->queue.push_back({static_cast<const uint8_t*>(buffer), size / o->frame_size * o->frame_size, 0});
  o->cv.notify_all();
  return kSuccess;
}
SLresult SYSV QueueClear(const void* self) {
  Object* o = ObjectOf(self);
  std::lock_guard<std::mutex> lock(o->mu);
  o->queue.clear();
  return kSuccess;
}
SLresult SYSV QueueGetState(const void* self, SLuint32* state /* {count, index} */) {
  Object* o = ObjectOf(self);
  if (state == nullptr) return kParameterInvalid;
  std::lock_guard<std::mutex> lock(o->mu);
  state[0] = static_cast<SLuint32>(o->queue.size());
  state[1] = o->queue_index;
  return kSuccess;
}
SLresult SYSV QueueRegisterCallback(const void* self, void(SYSV* cb)(Itf, void*), void* ctx) {
  Object* o = ObjectOf(self);
  std::lock_guard<std::mutex> lock(o->mu);
  o->queue_cb = cb;
  o->queue_ctx = ctx;
  return kSuccess;
}

const void* const kQueueVtable[] = {
    reinterpret_cast<const void*>(&QueueEnqueue), reinterpret_cast<const void*>(&QueueClear),
    reinterpret_cast<const void*>(&QueueGetState), reinterpret_cast<const void*>(&QueueRegisterCallback),
};

// --- SLVolumeItf ------------------------------------------------------------------------

SLresult SYSV VolSetLevel(const void* self, SLmillibel level) {
  Object* o = ObjectOf(self);
  o->level = std::min<SLmillibel>(level, 0);
  ApplyVolume(o);
  return kSuccess;
}
SLresult SYSV VolGetLevel(const void* self, SLmillibel* level) {
  if (level == nullptr) return kParameterInvalid;
  *level = ObjectOf(self)->level;
  return kSuccess;
}
SLresult SYSV VolGetMax(const void*, SLmillibel* level) {
  if (level == nullptr) return kParameterInvalid;
  *level = 0;
  return kSuccess;
}
SLresult SYSV VolSetMute(const void* self, SLboolean mute) {
  Object* o = ObjectOf(self);
  o->mute = mute != 0;
  ApplyVolume(o);
  return kSuccess;
}
SLresult SYSV VolGetMute(const void* self, SLboolean* mute) {
  if (mute == nullptr) return kParameterInvalid;
  *mute = ObjectOf(self)->mute ? 1 : 0;
  return kSuccess;
}
SLresult SYSV VolEnableStereo(const void*, SLboolean) { return kSuccess; }
SLresult SYSV VolIsStereo(const void*, SLboolean* e) {
  if (e != nullptr) *e = 0;
  return kSuccess;
}
SLresult SYSV VolSetStereo(const void*, SLpermille) { return kSuccess; }
SLresult SYSV VolGetStereo(const void*, SLpermille* p) {
  if (p != nullptr) *p = 0;
  return kSuccess;
}

const void* const kVolumeVtable[] = {
    reinterpret_cast<const void*>(&VolSetLevel), reinterpret_cast<const void*>(&VolGetLevel),
    reinterpret_cast<const void*>(&VolGetMax),   reinterpret_cast<const void*>(&VolSetMute),
    reinterpret_cast<const void*>(&VolGetMute),  reinterpret_cast<const void*>(&VolEnableStereo),
    reinterpret_cast<const void*>(&VolIsStereo), reinterpret_cast<const void*>(&VolSetStereo),
    reinterpret_cast<const void*>(&VolGetStereo),
};

// --- SLSeekItf (fd and URI players) ------------------------------------------------------

SLresult SYSV SeekSetPosition(const void* self, SLmillisecond pos, SLuint32) {
  Object* o = ObjectOf(self);
  if (o->player == nullptr) return kPreconditionsViolated;
  o->at_end = false;
  o->player->SeekTo(static_cast<int>(pos), 0);
  return kSuccess;
}
SLresult SYSV SeekSetLoop(const void* self, SLboolean loop, SLmillisecond, SLmillisecond) {
  Object* o = ObjectOf(self);
  if (o->player != nullptr) o->player->SetLooping(loop != 0);
  return kSuccess;
}
SLresult SYSV SeekGetLoop(const void* self, SLboolean* loop, SLmillisecond* start, SLmillisecond* end) {
  Object* o = ObjectOf(self);
  if (loop != nullptr) *loop = o->player != nullptr && o->player->IsLooping() ? 1 : 0;
  if (start != nullptr) *start = 0;
  if (end != nullptr) *end = 0xFFFFFFFF;
  return kSuccess;
}

const void* const kSeekVtable[] = {
    reinterpret_cast<const void*>(&SeekSetPosition), reinterpret_cast<const void*>(&SeekSetLoop),
    reinterpret_cast<const void*>(&SeekGetLoop),
};

// --- SLAndroidConfigurationItf ----------------------------------------------------------

SLresult SYSV ConfigSet(const void* self, const char* key, const void* value, SLuint32 size) {
  Object* o = ObjectOf(self);
  // "androidPlaybackStreamType": SL_ANDROID_STREAM_* (= AudioSystem's stream types)
  if (key != nullptr && strcmp(key, "androidPlaybackStreamType") == 0 && value != nullptr &&
      size >= sizeof(SLint32)) {
    o->stream = *static_cast<const SLint32*>(value);
  }
  return kSuccess;
}
SLresult SYSV ConfigGet(const void*, const char*, SLuint32*, void*) { return kParameterInvalid; }
SLresult SYSV ConfigAcquireJavaProxy(const void*, SLuint32, void**) { return kFeatureUnsupported; }
SLresult SYSV ConfigReleaseJavaProxy(const void*, SLuint32) { return kFeatureUnsupported; }

const void* const kConfigVtable[] = {
    reinterpret_cast<const void*>(&ConfigSet), reinterpret_cast<const void*>(&ConfigGet),
    reinterpret_cast<const void*>(&ConfigAcquireJavaProxy),
    reinterpret_cast<const void*>(&ConfigReleaseJavaProxy),
};

// --- SLPrefetchStatusItf (fd and URI players: everything is there) ----------------------

SLresult SYSV PrefetchGetStatus(const void*, SLuint32* status) {
  if (status != nullptr) *status = kPrefetchStatusSufficientData;
  return kSuccess;
}
SLresult SYSV PrefetchGetFill(const void*, SLpermille* level) {
  if (level != nullptr) *level = 1000;
  return kSuccess;
}
SLresult SYSV PrefetchOk3(const void*, void*, void*) { return kSuccess; }
SLresult SYSV PrefetchOk1(const void*, SLuint32) { return kSuccess; }

const void* const kPrefetchVtable[] = {
    reinterpret_cast<const void*>(&PrefetchGetStatus), reinterpret_cast<const void*>(&PrefetchGetFill),
    reinterpret_cast<const void*>(&PrefetchOk3),     reinterpret_cast<const void*>(&PrefetchOk1),
    reinterpret_cast<const void*>(&PrefetchOk1),     reinterpret_cast<const void*>(&PrefetchOk1),
    reinterpret_cast<const void*>(&PrefetchOk1),
};

// --- SLEngineItf ------------------------------------------------------------------------

SLresult SYSV EngUnsupported() { return kFeatureUnsupported; }

SLresult SYSV EngCreateOutputMix(const void*, Itf* out, SLuint32, const SLInterfaceID*,
                                 const SLboolean*) {
  if (out == nullptr) return kParameterInvalid;
  Object* o = NewObject(Kind::kOutputMix);
  *out = reinterpret_cast<Itf>(&o->object_itf);
  return kSuccess;
}

SLresult SYSV EngCreateAudioPlayer(const void*, Itf* out, const SLDataSource* src, const SLDataSink* sink,
                                   SLuint32, const SLInterfaceID*, const SLboolean*) {
  if (out == nullptr || src == nullptr || src->locator == nullptr) return kParameterInvalid;
  if (sink == nullptr || sink->locator == nullptr ||
      static_cast<DataLocatorHeader*>(sink->locator)->type != kLocatorOutputMix) {
    return kContentUnsupported;
  }
  SLuint32 type = static_cast<DataLocatorHeader*>(src->locator)->type;
  std::unique_ptr<Object> o(NewObject(Kind::kPlayer));
  if (type == kLocatorBufferQueue || type == kLocatorAndroidSimpleBufferQueue) {
    if (src->format == nullptr) return kParameterInvalid;
    auto* pcm = static_cast<DataFormatPcm*>(src->format);
    if (pcm->format_type != kFormatPcm && pcm->format_type != kFormatPcmEx) return kContentUnsupported;
    PcmFormat f;
    bool is_float = pcm->format_type == kFormatPcmEx &&
                    static_cast<DataFormatPcmEx*>(src->format)->representation == kPcmRepresentationFloat;
    switch (pcm->bits_per_sample) {
      case 8: f = PcmFormat::kU8; break;
      case 16: f = PcmFormat::kS16; break;
      case 24: f = PcmFormat::kS24Packed; break;
      case 32: f = is_float ? PcmFormat::kFloat : PcmFormat::kS32; break;
      default: return kContentUnsupported;
    }
    o->queue_player = true;
    o->queue_capacity = std::max<SLuint32>(static_cast<DataLocatorQueue*>(src->locator)->num_buffers, 1);
    o->rate = pcm->samples_per_sec / 1000;
    TrackParams p;
    p.sample_rate = o->rate;
    p.channels = std::max<SLuint32>(pcm->num_channels, 1);
    p.format = f;
    p.stream_type = o->stream;
    p.capacity_frames = std::max<size_t>(o->rate / 10, 512);  // 100 ms: the queue holds the rest
    p.name = "OpenSL ES";
    std::string error;
    o->track = CreateTrack(p, &error);
    if (o->track == nullptr) {
      MEDIA_LOGE("OpenSL ES: %s", error.c_str());
      return kContentUnsupported;
    }
    o->frame_size = BytesPerSample(f) * p.channels;
  } else if (type == kLocatorAndroidFd || type == kLocatorUri) {
    std::string error;
    if (type == kLocatorAndroidFd) {
      auto* l = static_cast<DataLocatorAndroidFd*>(src->locator);
      // SL_DATALOCATOR_ANDROIDFD_USE_FILE_SIZE is -1: to the end.
      o->source = OpenFdSource(l->fd, l->offset, l->length, &error);
    } else {
      std::string uri = static_cast<DataLocatorUri*>(src->locator)->uri;
      o->source = uri.rfind("http", 0) == 0 ? OpenHttpSource(uri, {}, &error) : OpenFileSource(uri, &error);
    }
    if (o->source == nullptr) {
      MEDIA_LOGE("OpenSL ES: %s", error.c_str());
      return kContentUnsupported;
    }
    o->player = std::make_shared<Player>();
    Object* raw = o.get();
    o->player->SetListener([raw](int what, int, int) {
      if (what != kMediaPlaybackComplete) return;
      raw->at_end = true;
      raw->play_state = kPlayPaused;  // wilhelm pauses at the end
      void(SYSV* cb)(Itf, void*, SLuint32);
      void* ctx;
      {
        std::lock_guard<std::mutex> lock(raw->mu);
        cb = raw->play_cb;
        ctx = raw->play_ctx;
      }
      if (cb != nullptr && (raw->event_mask & kPlayEventHeadAtEnd) != 0) {
        gesher::bionic::EnterLoadedCode();
        cb(reinterpret_cast<Itf>(&raw->play_itf), ctx, kPlayEventHeadAtEnd);
      }
    });
    if (o->player->SetDataSource(o->source) != kPlayerOk) return kContentUnsupported;
  } else {
    return kContentUnsupported;
  }
  *out = reinterpret_cast<Itf>(&o.release()->object_itf);
  return kSuccess;
}

// The players of fd and URI sources are prepared when realized.
SLresult SYSV PlayerRealize(const void* self, SLboolean async) {
  Object* o = ObjectOf(self);
  if (o->player != nullptr && o->state != kObjectRealized) {
    o->player->SetAudioStreamType(o->stream);
    if (o->player->Prepare() != kPlayerOk) return kContentUnsupported;
  }
  return ObjRealize(self, async);
}

SLresult SYSV EngCreateAudioRecorder(const void*, Itf*, const SLDataSource*, const SLDataSink*, SLuint32,
                                     const SLInterfaceID*, const SLboolean*) {
  MEDIA_LOGW("OpenSL ES: no audio recorder yet (audio capture is stage 3)");
  return kFeatureUnsupported;
}

SLresult SYSV EngQueryNum(const void*, SLuint32* n) {
  if (n != nullptr) *n = 0;
  return kSuccess;
}
SLresult SYSV EngQueryNumExt(const void*, SLuint32* n) {
  if (n != nullptr) *n = 0;
  return kSuccess;
}
SLresult SYSV EngIsExtensionSupported(const void*, const char*, SLboolean* s) {
  if (s != nullptr) *s = 0;
  return kSuccess;
}

const void* const kEngineVtable[] = {
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateLEDDevice
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateVibraDevice
    reinterpret_cast<const void*>(&EngCreateAudioPlayer),  // CreateAudioPlayer
    reinterpret_cast<const void*>(&EngCreateAudioRecorder),
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateMidiPlayer
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateListener
    reinterpret_cast<const void*>(&EngUnsupported),        // Create3DGroup
    reinterpret_cast<const void*>(&EngCreateOutputMix),
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateMetadataExtractor
    reinterpret_cast<const void*>(&EngUnsupported),        // CreateExtensionObject
    reinterpret_cast<const void*>(&EngQueryNum),           // QueryNumSupportedInterfaces
    reinterpret_cast<const void*>(&EngUnsupported),        // QuerySupportedInterfaces
    reinterpret_cast<const void*>(&EngQueryNumExt),        // QueryNumSupportedExtensions
    reinterpret_cast<const void*>(&EngUnsupported),        // QuerySupportedExtension
    reinterpret_cast<const void*>(&EngIsExtensionSupported),
};

const void* const kPlayerObjectVtable[] = {
    reinterpret_cast<const void*>(&PlayerRealize),  reinterpret_cast<const void*>(&ObjResume),
    reinterpret_cast<const void*>(&ObjGetState),    reinterpret_cast<const void*>(&ObjGetInterface),
    reinterpret_cast<const void*>(&ObjRegisterCallback), reinterpret_cast<const void*>(&ObjAbort),
    reinterpret_cast<const void*>(&ObjDestroy),     reinterpret_cast<const void*>(&ObjSetPriority),
    reinterpret_cast<const void*>(&ObjGetPriority), reinterpret_cast<const void*>(&ObjSetLossOfControl),
};

Object* NewObject(Kind kind) {
  auto* o = new Object();
  o->kind = kind;
  o->object_itf = {kind == Kind::kPlayer ? kPlayerObjectVtable : kObjectVtable, o};
  o->engine_itf = {kEngineVtable, o};
  o->play_itf = {kPlayVtable, o};
  o->queue_itf = {kQueueVtable, o};
  o->volume_itf = {kVolumeVtable, o};
  o->seek_itf = {kSeekVtable, o};
  o->config_itf = {kConfigVtable, o};
  o->prefetch_itf = {kPrefetchVtable, o};
  return o;
}

// slCreateEngine and slQueryNumSupportedEngineInterfaces / slQuerySupportedEngineInterfaces.
SLresult SYSV CreateEngine(Itf* out, SLuint32, const void*, SLuint32, const SLInterfaceID*,
                           const SLboolean*) {
  if (out == nullptr) return kParameterInvalid;
  Object* o = NewObject(Kind::kEngine);
  *out = reinterpret_cast<Itf>(&o->object_itf);
  return kSuccess;
}
SLresult SYSV QueryNumEngineInterfaces(SLuint32* n) {
  if (n == nullptr) return kParameterInvalid;
  *n = 1;
  return kSuccess;
}
SLresult SYSV QueryEngineInterfaces(SLuint32 index, SLInterfaceID* id) {
  if (id == nullptr || index != 0) return kParameterInvalid;
  *id = kIidEngine;
  return kSuccess;
}

void Register() {
  using gesher::bionic::AddBionicSymbol;
  AddBionicSymbol("slCreateEngine", reinterpret_cast<void*>(&CreateEngine));
  AddBionicSymbol("slQueryNumSupportedEngineInterfaces", reinterpret_cast<void*>(&QueryNumEngineInterfaces));
  AddBionicSymbol("slQuerySupportedEngineInterfaces", reinterpret_cast<void*>(&QueryEngineInterfaces));
  struct {
    const char* name;
    const SLInterfaceID* id;
  } const ids[] = {
      {"SL_IID_NULL", &kIidNull},
      {"SL_IID_OBJECT", &kIidObject},
      {"SL_IID_ENGINE", &kIidEngine},
      {"SL_IID_PLAY", &kIidPlay},
      {"SL_IID_BUFFERQUEUE", &kIidBufferQueue},
      {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &kIidAndroidSimpleBufferQueue},
      {"SL_IID_VOLUME", &kIidVolume},
      {"SL_IID_SEEK", &kIidSeek},
      {"SL_IID_ANDROIDCONFIGURATION", &kIidAndroidConfiguration},
      {"SL_IID_PREFETCHSTATUS", &kIidPrefetchStatus},
      {"SL_IID_PLAYBACKRATE", &kIidPlaybackRate},
      {"SL_IID_EFFECTSEND", &kIidEffectSend},
      {"SL_IID_ENVIRONMENTALREVERB", &kIidEnvironmentalReverb},
      {"SL_IID_RECORD", &kIidRecord},
      {"SL_IID_ENGINECAPABILITIES", &kIidEngineCapabilities},
      {"SL_IID_METADATAEXTRACTION", &kIidMetadataExtraction},
      {"SL_IID_MUTESOLO", &kIidMuteSolo},
      {"SL_IID_ANDROIDEFFECT", &kIidAndroidEffect},
      {"SL_IID_DYNAMICINTERFACEMANAGEMENT", &kIidDynamicInterfaceManagement},
  };
  // Data symbols: each .so import of SL_IID_X binds to the variable holding the ID.
  for (const auto& i : ids) AddBionicSymbol(i.name, const_cast<SLInterfaceID*>(i.id));
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
