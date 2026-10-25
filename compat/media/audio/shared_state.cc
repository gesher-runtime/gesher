// Gesher: the audio state shared by a device's processes (shared_state.h).

#include "shared_state.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>

#include "engine.h"
#include "log.h"

namespace gesher::media {

struct SharedAudioState {
  uint32_t magic;
  uint32_t version;
  volatile LONG generation;
  volatile LONG next_session;
  volatile LONG lock;  // a spin lock for the multi-field updates
  int32_t index[kNumStreamTypes];
  int32_t min_index[kNumStreamTypes];
  int32_t max_index[kNumStreamTypes];
  int32_t muted[kNumStreamTypes];
  volatile LONG64 active_until_ms[kNumStreamTypes];
  float master_volume;
  int32_t master_mute;
};

namespace {

constexpr uint32_t kMagic = 0x47415544;  // 'GAUD'
constexpr uint32_t kVersion = 1;

// AudioService's default maximum indexes (MAX_STREAM_VOLUME) and minimums
// (MIN_STREAM_VOLUME), by stream type.
constexpr int kMaxIndex[kNumStreamTypes] = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
constexpr int kMinIndex[kNumStreamTypes] = {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0};

// A device's name for its Windows objects: ANDROID_DATA's Java hashCode, as
// com.gesher.server.Device names its mutex (here over the variable itself).
std::wstring MappingName() {
  const wchar_t* data = _wgetenv(L"ANDROID_DATA");
  std::wstring d = data != nullptr && *data != 0 ? data : L"/data";
  int32_t h = 0;
  for (wchar_t c : d) {
    wchar_t l = static_cast<wchar_t>(std::towlower(c == L'\\' ? L'/' : c));
    h = static_cast<int32_t>(31u * static_cast<uint32_t>(h) + static_cast<uint32_t>(l));
  }
  wchar_t buf[64];
  swprintf(buf, 64, L"Local\\gesher-%08x-audio", static_cast<uint32_t>(h));
  return buf;
}

void InitDefaults(SharedAudioState* s) {
  memset(s, 0, sizeof(*s));
  s->version = kVersion;
  s->next_session = 1;
  for (int i = 0; i < kNumStreamTypes; i++) {
    s->min_index[i] = kMinIndex[i];
    s->max_index[i] = kMaxIndex[i];
    // A Windows app's sessions start at full volume; so do Gesher's streams.
    s->index[i] = kMaxIndex[i];
  }
  s->master_volume = 1.0f;
  MemoryBarrier();
  s->magic = kMagic;
}

SharedAudioState* Open() {
  std::wstring name = MappingName();
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                sizeof(SharedAudioState), name.c_str());
  bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
  void* view = h != nullptr ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedAudioState))
                            : nullptr;
  if (view == nullptr) {
    MEDIA_LOGW("audio state: no shared mapping (error %lu); volumes are this process's",
               GetLastError());
    auto* s = new SharedAudioState;
    InitDefaults(s);
    return s;
  }
  // The handle stays open for the process's life: the mapping lives as long
  // as some process of the device has it.
  auto* s = static_cast<SharedAudioState*>(view);
  if (!existed) {
    InitDefaults(s);
  } else {
    // Another process is initializing it: wait for its magic.
    for (int i = 0; i < 1000 && s->magic != kMagic; i++) Sleep(1);
    if (s->magic != kMagic || s->version != kVersion) InitDefaults(s);
  }
  return s;
}

class SpinLock {
 public:
  explicit SpinLock(SharedAudioState* s) : s_(s) {
    for (int spins = 0; InterlockedCompareExchange(&s_->lock, 1, 0) != 0; spins++) {
      if (spins > 64) Sleep(0);
    }
  }
  ~SpinLock() { InterlockedExchange(&s_->lock, 0); }

 private:
  SharedAudioState* s_;
};

bool Valid(int stream) { return stream >= 0 && stream < kNumStreamTypes; }

void Bump(SharedAudioState* s) { InterlockedIncrement(&s->generation); }

}  // namespace

SharedAudioState* State() {
  static SharedAudioState* state = Open();
  return state;
}

void InitStreamVolume(int stream, int min_index, int max_index) {
  if (!Valid(stream) || max_index <= min_index) return;
  SharedAudioState* s = State();
  {
    SpinLock lock(s);
    int old_max = s->max_index[stream];
    if (old_max == max_index && s->min_index[stream] == min_index) return;
    // Keep the level: an index at the old maximum stays at the new one.
    int idx = old_max > 0 ? static_cast<int>(std::lround(
                                static_cast<double>(s->index[stream]) * max_index / old_max))
                          : max_index;
    s->min_index[stream] = min_index;
    s->max_index[stream] = max_index;
    s->index[stream] = std::clamp(idx, min_index, max_index);
  }
  Bump(s);
}

void SetStreamVolume(int stream, int index, bool muted) {
  if (!Valid(stream)) return;
  SharedAudioState* s = State();
  {
    SpinLock lock(s);
    s->index[stream] = std::clamp(index, s->min_index[stream], s->max_index[stream]);
    s->muted[stream] = muted ? 1 : 0;
  }
  Bump(s);
}

int GetStreamVolume(int stream) { return Valid(stream) ? State()->index[stream] : 0; }
int GetStreamMinVolume(int stream) { return Valid(stream) ? State()->min_index[stream] : 0; }
int GetStreamMaxVolume(int stream) { return Valid(stream) ? State()->max_index[stream] : 0; }
bool GetStreamMuted(int stream) { return Valid(stream) && State()->muted[stream] != 0; }

float StreamGain(int stream) {
  if (!Valid(stream)) return 1.0f;
  SharedAudioState* s = State();
  int a = StreamAlias(stream);
  if (s->muted[a] != 0 || s->muted[stream] != 0) return 0.0f;
  int max = s->max_index[a];
  if (max <= 0) return 1.0f;
  return std::clamp(static_cast<float>(s->index[a]) / static_cast<float>(max), 0.0f, 1.0f);
}

void SetStreamGainFromSession(int stream, float level, bool muted) {
  if (!Valid(stream)) return;
  SharedAudioState* s = State();
  int a = StreamAlias(stream);
  bool changed = false;
  {
    SpinLock lock(s);
    for (int i = 0; i < kNumStreamTypes; i++) {
      if (StreamAlias(i) != a) continue;
      int idx = std::clamp(static_cast<int>(std::lround(level * s->max_index[i])),
                           s->min_index[i], s->max_index[i]);
      int m = muted ? 1 : 0;
      if (s->index[i] != idx || s->muted[i] != m) changed = true;
      s->index[i] = idx;
      s->muted[i] = m;
    }
  }
  if (changed) Bump(s);
}

void SetMasterVolume(float v) {
  State()->master_volume = std::clamp(v, 0.0f, 1.0f);
  Bump(State());
}
float GetMasterVolume() { return State()->master_volume; }
void SetMasterMute(bool mute) {
  State()->master_mute = mute ? 1 : 0;
  Bump(State());
}
bool GetMasterMute() { return State()->master_mute != 0; }

uint32_t Generation() { return static_cast<uint32_t>(State()->generation); }

void MarkStreamActive(int stream, uint64_t until_ms) {
  if (!Valid(stream)) return;
  SharedAudioState* s = State();
  LONG64 old = s->active_until_ms[stream];
  while (static_cast<uint64_t>(old) < until_ms) {
    LONG64 prev = InterlockedCompareExchange64(&s->active_until_ms[stream],
                                               static_cast<LONG64>(until_ms), old);
    if (prev == old) break;
    old = prev;
  }
}

bool StreamActive(int stream, int in_past_ms) {
  if (!Valid(stream)) return false;
  uint64_t until = static_cast<uint64_t>(State()->active_until_ms[stream]);
  return until + static_cast<uint64_t>(std::max(in_past_ms, 0)) >= GetTickCount64();
}

int NewSessionId() {
  // As AudioFlinger's unique ids: a counter in steps of AUDIO_UNIQUE_ID_USE_MAX
  // (8) with the use in the low bits (AUDIO_UNIQUE_ID_USE_SESSION = 1).
  LONG n = InterlockedIncrement(&State()->next_session);
  return static_cast<int>((n << 3) | 1);
}

}  // namespace gesher::media
