// Gesher: the audio output of an app process (engine.h).

#include "engine.h"

#include <windows.h>
#include <objbase.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <thread>

#include "log.h"
#include "shared_state.h"
#include "sink.h"

namespace gesher::media {

size_t BytesPerSample(PcmFormat f) {
  switch (f) {
    case PcmFormat::kU8: return 1;
    case PcmFormat::kS16: return 2;
    case PcmFormat::kS24Packed: return 3;
    case PcmFormat::kS32: return 4;
    case PcmFormat::kFloat: return 4;
  }
  return 2;
}

// --- Stream volume groups ----------------------------------------------------------------

namespace {

// STREAM_VOICE_CALL 0, SYSTEM 1, RING 2, MUSIC 3, ALARM 4, NOTIFICATION 5,
// BLUETOOTH_SCO 6, SYSTEM_ENFORCED 7, DTMF 8, TTS 9, ACCESSIBILITY 10, ASSISTANT 11:
// AudioService's STREAM_VOLUME_ALIAS_DEFAULT.
constexpr int kAlias[kNumStreamTypes] = {0, 2, 2, 3, 4, 5, 0, 2, 2, 3, 3, 3};
constexpr int kGroupStream[kNumGroups] = {3, 2, 4, 5, 0};
constexpr const char* kGroupName[kNumGroups] = {"Media", "Ringtone", "Alarm", "Notifications",
                                                "Calls"};

int64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

}  // namespace

int StreamAlias(int stream) {
  return stream >= 0 && stream < kNumStreamTypes ? kAlias[stream] : 3;
}

int GroupOfStream(int stream) {
  int a = StreamAlias(stream);
  for (int g = 0; g < kNumGroups; g++) {
    if (kGroupStream[g] == a) return g;
  }
  return 0;
}

int StreamOfGroup(int group) { return group >= 0 && group < kNumGroups ? kGroupStream[group] : 3; }

const char* GroupName(int group) {
  return group >= 0 && group < kNumGroups ? kGroupName[group] : "Media";
}

// --- The resampler -----------------------------------------------------------------------

namespace {

// A windowed sinc (Blackman-Harris) of kHalfTaps zero crossings each side,
// tabulated at kTableRes points per zero crossing.
constexpr int kHalfTaps = 12;
constexpr int kTableRes = 512;

const float* KernelTable() {
  static const std::vector<float> table = [] {
    std::vector<float> t(kHalfTaps * kTableRes + 2);
    for (size_t i = 0; i < t.size(); i++) {
      double x = static_cast<double>(i) / kTableRes;
      if (x >= kHalfTaps) {
        t[i] = 0;
        continue;
      }
      double sinc = x == 0 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
      double n = 0.5 + x / (2.0 * kHalfTaps);  // window over [-H, H] mapped to [0, 1]
      double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * n) + 0.14128 * std::cos(4 * M_PI * n) -
                 0.01168 * std::cos(6 * M_PI * n);
      t[i] = static_cast<float>(sinc * w);
    }
    return t;
  }();
  return table.data();
}

inline float Kernel(const float* table, double x) {
  x = std::fabs(x);
  if (x >= kHalfTaps) return 0;
  double p = x * kTableRes;
  size_t i = static_cast<size_t>(p);
  float f = static_cast<float>(p - static_cast<double>(i));
  return table[i] + (table[i + 1] - table[i]) * f;
}

// Converts `frames` frames of `fmt` to float.
void ToFloat(const uint8_t* src, PcmFormat fmt, size_t samples, float* dst) {
  switch (fmt) {
    case PcmFormat::kU8:
      for (size_t i = 0; i < samples; i++) dst[i] = (static_cast<int>(src[i]) - 128) / 128.0f;
      break;
    case PcmFormat::kS16: {
      for (size_t i = 0; i < samples; i++) {
        int16_t v;
        memcpy(&v, src + 2 * i, 2);
        dst[i] = v / 32768.0f;
      }
      break;
    }
    case PcmFormat::kS24Packed:
      for (size_t i = 0; i < samples; i++) {
        const uint8_t* p = src + 3 * i;
        int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) |
                                         (static_cast<uint32_t>(p[1]) << 16) |
                                         (static_cast<uint32_t>(p[2]) << 24)) >> 8;
        dst[i] = v / 8388608.0f;
      }
      break;
    case PcmFormat::kS32:
      for (size_t i = 0; i < samples; i++) {
        int32_t v;
        memcpy(&v, src + 4 * i, 4);
        dst[i] = static_cast<float>(v / 2147483648.0);
      }
      break;
    case PcmFormat::kFloat:
      memcpy(dst, src, samples * 4);
      break;
  }
}

}  // namespace

// Source frames (float, the track's channels) in, output frames out. The
// history starts with kHalfTaps-ish zeros, so the first output frame is
// centered on the first source frame.
struct Track::Resampler {
  explicit Resampler(uint32_t ch) : channels(ch) { Reset(); }

  void Reset() {
    hist.clear();
    width = 0;
    t = 0;
    start_index = 0;
    primed = false;
    real_end = -1;
    played_index = 0;
  }

  uint32_t channels;
  std::vector<float> hist;  // interleaved; hist frame 0 is source frame start_index
  size_t width;             // half-width in source frames of the current filter
  double t;                 // the next output frame's center, in hist frames
  int64_t start_index;      // source index of hist frame 0 (negative while primed zeros)
  bool primed;
  int64_t real_end = -1;    // source index one past the last real frame, once ended
  int64_t played_index = 0; // source frames passed since the last Reset
  size_t frames() const { return hist.size() / channels; }
};

// --- Engine ------------------------------------------------------------------------------

class Engine : public Mixer {
 public:
  static Engine* Get() {
    static Engine* engine = new Engine();  // never destroyed: threads run until exit
    return engine;
  }

  void Add(const std::shared_ptr<Track>& t) {
    std::lock_guard<std::mutex> lock(mu_);
    tracks_.push_back(t);
    StartLocked();
  }

  void Remove(Track* t) {
    std::lock_guard<std::mutex> lock(mu_);
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                 [t](const std::weak_ptr<Track>& w) {
                                   auto s = w.lock();
                                   return s == nullptr || s.get() == t;
                                 }),
                  tracks_.end());
  }

  void Post(std::function<void()> fn) {
    {
      std::lock_guard<std::mutex> lock(ev_mu_);
      events_.push_back(std::move(fn));
      if (!ev_thread_started_) {
        ev_thread_started_ = true;
        std::thread([this] { EventLoop(); }).detach();
      }
    }
    ev_cv_.notify_one();
  }

  OutputInfo Info() {
    std::unique_lock<std::mutex> lock(mu_);
    StartLocked();
    ready_cv_.wait(lock, [this] { return ready_; });
    return info_;
  }

  void SetAppName(const std::u16string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    app_name_ = name;
  }

  uint32_t latency_frames() const { return latency_frames_.load(); }
  uint32_t out_rate() const { return out_rate_.load(); }

  // Mixer.
  bool MixGroup(int group, float* out, size_t frames) override {
    uint32_t ch = info_.channels;
    std::fill(out, out + frames * ch, 0.0f);
    std::vector<std::shared_ptr<Track>> tracks;
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (auto& w : tracks_) {
        auto t = w.lock();
        if (t != nullptr && GroupOfStream(t->params().stream_type) == group) tracks.push_back(t);
      }
    }
    bool played = false;
    for (auto& t : tracks) {
      if (t->MixInto(out, frames, info_.sample_rate, ch)) {
        played = true;
        group_last_ms_[group] = GetTickCount64();
        MarkStreamActive(t->params().stream_type, GetTickCount64() + 50);
      }
    }
    float master = GetMasterMute() ? 0.0f : GetMasterVolume();
    if (master != 1.0f) {
      for (size_t i = 0; i < frames * ch; i++) out[i] *= master;
    }
    return played;
  }

  bool GroupWanted(int group) override {
    if (GetTickCount64() < group_last_ms_[group] + 1000) return true;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& w : tracks_) {
      auto t = w.lock();
      if (t == nullptr || GroupOfStream(t->params().stream_type) != group) continue;
      Track::State s = t->state();
      if (s == Track::State::kPlaying || s == Track::State::kStopping) return true;
    }
    return false;
  }

 private:
  Engine() = default;

  void StartLocked() {
    if (started_) return;
    started_ = true;
    std::thread([this] { Run(); }).detach();
  }

  std::unique_ptr<Sink> OpenSink(bool first) {
    std::string error;
    const wchar_t* capture = _wgetenv(L"GESHER_AUDIO_CAPTURE");
    if (capture != nullptr && *capture != 0) {
      auto s = OpenCaptureSink(capture, &error);
      if (s != nullptr) return s;
      MEDIA_LOGE("audio capture: %s", error.c_str());
    }
    const char* mode = getenv("GESHER_AUDIO");
    if (mode != nullptr && strcmp(mode, "null") == 0) return OpenNullSink("GESHER_AUDIO=null");
    std::u16string name;
    {
      std::lock_guard<std::mutex> lock(mu_);
      name = app_name_;
    }
    auto s = OpenWasapiSink(name, &error);
    if (s != nullptr) return s;
    if (first || error != last_error_) MEDIA_LOGW("audio output: %s", error.c_str());
    last_error_ = error;
    return OpenNullSink(error);
  }

  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::unique_ptr<Sink> sink = OpenSink(true);
    bool wasapi_wanted = true;
    {
      const wchar_t* capture = _wgetenv(L"GESHER_AUDIO_CAPTURE");
      const char* mode = getenv("GESHER_AUDIO");
      if ((capture != nullptr && *capture != 0) || (mode != nullptr && strcmp(mode, "null") == 0)) {
        wasapi_wanted = false;
      }
    }
    PublishInfo(sink.get());
    MEDIA_LOGI("audio output: %s", sink->Describe().c_str());

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (timer == nullptr) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(kPeriodMs) * 10000;
    SetWaitableTimer(timer, &due, kPeriodMs, nullptr, nullptr, FALSE);

    uint32_t generation = Generation() - 1;
    uint64_t last_retry = GetTickCount64();
    bool null_sink = sink->Describe().rfind("none", 0) == 0;
    for (;;) {
      WaitForSingleObject(timer, 100);
      uint32_t g = Generation();
      if (g != generation) {
        generation = g;
        sink->OnVolumesChanged();
      }
      bool ok = sink->Tick(this);
      uint64_t now = GetTickCount64();
      if (!ok || (null_sink && wasapi_wanted && now - last_retry > 5000)) {
        last_retry = now;
        auto next = OpenSink(false);
        bool next_null = next->Describe().rfind("none", 0) == 0;
        if (!ok || !next_null) {
          // The device changed: the new one may have another rate. Tracks
          // keep their positions; the resamplers follow the new rate.
          sink = std::move(next);
          null_sink = next_null;
          PublishInfo(sink.get());
          MEDIA_LOGI("audio output: %s", sink->Describe().c_str());
          sink->OnVolumesChanged();
        }
      }
    }
  }

  void PublishInfo(Sink* sink) {
    std::lock_guard<std::mutex> lock(mu_);
    info_.sample_rate = sink->rate();
    info_.channels = sink->channels();
    info_.period_frames = sink->rate() * kPeriodMs / 1000;
    info_.latency_frames = sink->latency_frames();
    info_.description = sink->Describe();
    info_.device_name = sink->DeviceName();
    info_.device_type = sink->DeviceType();
    info_.generation++;
    latency_frames_ = sink->latency_frames();
    out_rate_ = sink->rate();
    ready_ = true;
    ready_cv_.notify_all();
  }

  void EventLoop() {
    for (;;) {
      std::function<void()> fn;
      {
        std::unique_lock<std::mutex> lock(ev_mu_);
        ev_cv_.wait(lock, [this] { return !events_.empty(); });
        fn = std::move(events_.front());
        events_.pop_front();
      }
      fn();
    }
  }

  static constexpr uint32_t kPeriodMs = 10;

  std::mutex mu_;
  std::condition_variable ready_cv_;
  bool started_ = false;
  bool ready_ = false;
  OutputInfo info_{48000, 2, 480, 0, "", "", 0x2, 0};
  std::vector<std::weak_ptr<Track>> tracks_;
  std::u16string app_name_;
  std::string last_error_;
  std::atomic<uint32_t> latency_frames_{0};
  std::atomic<uint32_t> out_rate_{48000};
  std::atomic<uint64_t> group_last_ms_[kNumGroups] = {};

  std::mutex ev_mu_;
  std::condition_variable ev_cv_;
  std::deque<std::function<void()>> events_;
  bool ev_thread_started_ = false;
};

OutputInfo GetOutputInfo() { return Engine::Get()->Info(); }

void RaiseAudioThreadPriority(bool urgent) {
  SetThreadPriority(GetCurrentThread(),
                    urgent ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL);
}

void PostEvent(std::function<void()> fn) { Engine::Get()->Post(std::move(fn)); }

bool IsStreamActive(int stream, int in_past_ms) { return StreamActive(stream, in_past_ms); }

void SetAppName(const std::u16string& name) { Engine::Get()->SetAppName(name); }

// --- Track -------------------------------------------------------------------------------

std::shared_ptr<Track> CreateTrack(const TrackParams& params, std::string* error) {
  if (params.sample_rate < 1 || params.sample_rate > 1600000) {
    *error = "sample rate " + std::to_string(params.sample_rate);
    return nullptr;
  }
  if (params.channels < 1 || params.channels > 24) {
    *error = "channel count " + std::to_string(params.channels);
    return nullptr;
  }
  if (params.capacity_frames == 0 || params.capacity_frames > (256u << 20)) {
    *error = "buffer of " + std::to_string(params.capacity_frames) + " frames";
    return nullptr;
  }
  std::shared_ptr<Track> t(new Track(Engine::Get(), params));
  Engine::Get()->Add(t);
  return t;
}

Track::Track(Engine* engine, const TrackParams& params)
    : engine_(engine),
      params_(params),
      frame_size_(BytesPerSample(params.format) * params.channels),
      rate_(params.sample_rate),
      rs_(std::make_unique<Resampler>(params.channels)) {
  data_.resize(params.capacity_frames * frame_size_);
  buffer_frames_ = params.capacity_frames;
}

Track::~Track() = default;

void Track::SetEvents(TrackEvents events) {
  std::lock_guard<std::mutex> lock(mu_);
  events_ = std::move(events);
}

int Track::Start() {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_) return kInvalidOperation;
  if (state_ == State::kPlaying) return kOk;
  if (state_ == State::kStopped || (state_ == State::kStopping && drained_)) {
    // As android::AudioTrack::start: from stopped, the position starts at 0.
    position_base_ = played_;
    next_update_ = update_period_;
    marker_reached_ = false;
    if (params_.is_static && state_ == State::kStopped) {
      // A stopped static track was rewound by stop().
    }
    rs_->Reset();
  } else if (state_ == State::kStopping) {
    // start() while draining: keep playing what is queued.
  }
  state_ = State::kPlaying;
  drained_ = false;
  was_underrun_ = false;
  end_signalled_ = false;
  room_.notify_all();
  return kOk;
}

void Track::Pause() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == State::kPlaying || state_ == State::kStopping) state_ = State::kPaused;
  room_.notify_all();
}

void Track::Stop() {
  std::lock_guard<std::mutex> lock(mu_);
  if (params_.is_static) {
    state_ = State::kStopped;
    static_pos_ = 0;
    position_base_ = played_;  // position 0
    rs_->Reset();
  } else if (state_ == State::kPlaying) {
    state_ = State::kStopping;
    drained_ = written_ == read_ && rs_->frames() == 0;
    if (drained_) state_ = State::kStopped;
  } else if (state_ == State::kPaused) {
    // Paused data is dropped at stop (android::AudioTrack: stop from paused
    // flushes what was not played).
    state_ = State::kStopped;
    read_ = written_;
    rs_->Reset();
  }
  room_.notify_all();
}

void Track::Flush() {
  std::lock_guard<std::mutex> lock(mu_);
  if (params_.is_static || state_ == State::kPlaying) return;
  read_ = written_;
  rs_->Reset();
  position_base_ = played_;
  if (state_ == State::kStopping) state_ = State::kStopped;
  room_.notify_all();
}

Track::State Track::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

bool Track::stopped() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_ == State::kStopped || (state_ == State::kStopping && drained_);
}

ssize_t Track::Write(const void* data, size_t bytes, bool blocking) {
  const uint8_t* src = static_cast<const uint8_t*>(data);
  size_t frames = bytes / frame_size_;
  std::unique_lock<std::mutex> lock(mu_);
  if (released_) return kInvalidOperation;
  if (params_.is_static) {
    size_t n = std::min(frames, params_.capacity_frames);
    memcpy(data_.data(), src, n * frame_size_);
    static_frames_ = n;
    static_pos_ = 0;
    return static_cast<ssize_t>(n * frame_size_);
  }
  size_t done = 0;
  while (done < frames) {
    size_t limit = std::min(buffer_frames_, params_.capacity_frames);
    size_t queued = static_cast<size_t>(written_ - read_);
    size_t room = queued < limit ? limit - queued : 0;
    if (room == 0) {
      if (!blocking || released_) break;
      // Blocking writes wait while the track plays (or is stopped: data written
      // before play() fills the buffer, then waits for it).
      room_.wait_for(lock, std::chrono::milliseconds(100));
      if (released_ || state_ == State::kPaused) {
        if (released_) break;
      }
      continue;
    }
    size_t n = std::min(room, frames - done);
    // Copy into the ring, in up to two pieces.
    size_t cap = params_.capacity_frames;
    size_t pos = static_cast<size_t>(written_ % cap);
    size_t first = std::min(n, cap - pos);
    memcpy(data_.data() + pos * frame_size_, src + done * frame_size_, first * frame_size_);
    if (n > first) {
      memcpy(data_.data(), src + (done + first) * frame_size_, (n - first) * frame_size_);
    }
    written_ += n;
    done += n;
    if (state_ == State::kStopping) drained_ = false;
  }
  return static_cast<ssize_t>(done * frame_size_);
}

int Track::SetLoop(uint32_t start, uint32_t end, int count) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!params_.is_static) return kInvalidOperation;
  if (count == 0) {
    loop_count_ = 0;
    loop_start_ = loop_end_ = 0;
    return kOk;
  }
  if (start >= end || end > params_.capacity_frames) return kBadValue;
  loop_start_ = start;
  loop_end_ = end;
  loop_count_ = count;
  return kOk;
}

int Track::Reload() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!params_.is_static) return kInvalidOperation;
  if (state_ == State::kPlaying) return kInvalidOperation;
  static_pos_ = 0;
  position_base_ = played_;
  rs_->Reset();
  return kOk;
}

int Track::SetPosition(uint32_t frame) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!params_.is_static || state_ == State::kPlaying) return kInvalidOperation;
  if (frame > static_frames_) return kBadValue;
  static_pos_ = frame;
  position_base_ = played_ - frame;
  rs_->Reset();
  return kOk;
}

size_t Track::static_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_frames_;
}

uint32_t Track::Position() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<uint32_t>(played_ - position_base_);
}

bool Track::Timestamp(int64_t* frames, int64_t* time_ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (played_time_ns_ == 0) return false;
  *frames = static_cast<int64_t>(played_ - position_base_);
  // What was mixed last is heard after the sink's latency.
  uint32_t out_rate = engine_->out_rate();
  *time_ns = played_time_ns_ +
             (out_rate != 0 ? static_cast<int64_t>(engine_->latency_frames()) * 1000000000 / out_rate
                            : 0);
  return true;
}

int Track::SetMarker(uint32_t frame) {
  std::lock_guard<std::mutex> lock(mu_);
  marker_ = frame;
  marker_reached_ = frame == 0;
  return kOk;
}

uint32_t Track::marker() const {
  std::lock_guard<std::mutex> lock(mu_);
  return marker_;
}

int Track::SetUpdatePeriod(uint32_t frames) {
  std::lock_guard<std::mutex> lock(mu_);
  update_period_ = frames;
  uint64_t pos = played_ - position_base_;
  next_update_ = frames == 0 ? 0 : (pos / frames + 1) * frames;
  return kOk;
}

uint32_t Track::update_period() const {
  std::lock_guard<std::mutex> lock(mu_);
  return update_period_;
}

void Track::SetVolume(float left, float right) {
  std::lock_guard<std::mutex> lock(mu_);
  vol_left_ = std::clamp(left, 0.0f, 1.0f);
  vol_right_ = std::clamp(right, 0.0f, 1.0f);
}

int Track::SetSampleRate(uint32_t rate) {
  std::lock_guard<std::mutex> lock(mu_);
  if (rate == 0 || rate > params_.sample_rate * 2 * 8) return kBadValue;
  rate_ = rate;
  return kOk;
}

uint32_t Track::sample_rate() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rate_;
}

void Track::SetSpeed(float speed) {
  std::lock_guard<std::mutex> lock(mu_);
  speed_ = std::clamp(speed, 0.01f, 16.0f);
}

float Track::speed() const {
  std::lock_guard<std::mutex> lock(mu_);
  return speed_;
}

uint32_t Track::buffer_size_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<uint32_t>(params_.is_static ? params_.capacity_frames : buffer_frames_);
}

int Track::SetBufferSizeFrames(uint32_t frames) {
  std::lock_guard<std::mutex> lock(mu_);
  if (params_.is_static) return kInvalidOperation;
  buffer_frames_ = std::clamp<size_t>(frames, 1, params_.capacity_frames);
  room_.notify_all();
  return static_cast<int>(buffer_frames_);
}

uint32_t Track::underrun_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return underrun_frames_;
}

uint32_t Track::LatencyMs() const {
  uint32_t out_rate = engine_->out_rate();
  uint32_t sink_ms = out_rate != 0 ? engine_->latency_frames() * 1000 / out_rate : 0;
  std::lock_guard<std::mutex> lock(mu_);
  uint32_t buffer_ms =
      params_.is_static ? 0 : static_cast<uint32_t>(buffer_frames_ * 1000 / std::max(rate_, 1u));
  return sink_ms + buffer_ms;
}

size_t Track::Queued() const {
  std::lock_guard<std::mutex> lock(mu_);
  return params_.is_static ? 0 : static_cast<size_t>(written_ - read_);
}

void Track::Release() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    released_ = true;
    state_ = State::kStopped;
    room_.notify_all();
  }
  engine_->Remove(this);
}

// Mixing thread, with mu_ held.
size_t Track::ReadSource(float* dst, size_t frames, bool* ended) {
  *ended = false;
  size_t ch = params_.channels;
  size_t bps = BytesPerSample(params_.format);
  size_t done = 0;
  if (!params_.is_static) {
    size_t avail = static_cast<size_t>(written_ - read_);
    size_t n = std::min(avail, frames);
    size_t cap = params_.capacity_frames;
    while (done < n) {
      size_t pos = static_cast<size_t>(read_ % cap);
      size_t piece = std::min(n - done, cap - pos);
      ToFloat(data_.data() + pos * frame_size_, params_.format, piece * ch, dst + done * ch);
      read_ += piece;
      done += piece;
    }
    room_.notify_all();
    if (done == 0 && state_ == State::kStopping) *ended = true;
    return done;
  }
  while (done < frames) {
    bool looping = loop_count_ != 0 && loop_end_ > loop_start_ && static_pos_ < loop_end_;
    size_t end = looping ? loop_end_ : static_frames_;
    if (static_pos_ >= end) {
      if (looping || (loop_count_ != 0 && loop_end_ > loop_start_ && static_pos_ == loop_end_)) {
        static_pos_ = loop_start_;
        if (loop_count_ > 0) loop_count_--;
        continue;
      }
      *ended = true;
      break;
    }
    size_t piece = std::min(frames - done, end - static_pos_);
    ToFloat(data_.data() + static_pos_ * frame_size_, params_.format, piece * ch, dst + done * ch);
    static_pos_ += piece;
    done += piece;
  }
  (void)bps;
  return done;
}

bool Track::MixInto(float* mix, size_t frames, uint32_t out_rate, uint32_t out_channels) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || (state_ != State::kPlaying && state_ != State::kStopping) ||
      (state_ == State::kStopping && drained_)) {
    return false;
  }
  Resampler& rs = *rs_;
  const size_t ch = params_.channels;
  const double step = static_cast<double>(rate_) * speed_ / out_rate;
  const bool passthrough = step == 1.0;
  const double cutoff = step > 1.0 ? 1.0 / step * 0.97 : 1.0;
  const size_t width = passthrough ? 0 : static_cast<size_t>(std::ceil(kHalfTaps / cutoff));
  const float* table = KernelTable();

  if (!rs.primed || rs.width != width) {
    if (!rs.primed) {
      // Zeros before the first frame, so the filter has history.
      rs.hist.assign(width * ch, 0.0f);
      rs.start_index = -static_cast<int64_t>(width);
      rs.t = static_cast<double>(width);
      rs.real_end = -1;
      rs.primed = true;
    }
    rs.width = width;
  }

  uint64_t played_before = played_;
  std::vector<float> frame(ch);
  std::vector<float> tmp;
  size_t out_done = 0;
  bool ended = false;
  bool starved = false;
  // Volume changes ramp linearly over the block (no clicks).
  const float gl = vol_left_, gr = vol_right_;
  float al = applied_left_, ar = applied_right_;
  const float dl = (gl - al) / static_cast<float>(std::max<size_t>(frames, 1));
  const float dr = (gr - ar) / static_cast<float>(std::max<size_t>(frames, 1));

  for (; out_done < frames; out_done++) {
    int64_t center = static_cast<int64_t>(std::floor(rs.t));
    size_t need = static_cast<size_t>(center + static_cast<int64_t>(width) + 1);
    while (rs.frames() < need) {
      if (rs.real_end >= 0) {
        // Ended: zeros flush the filter.
        rs.hist.insert(rs.hist.end(), (need - rs.frames()) * ch, 0.0f);
        break;
      }
      size_t want = std::max<size_t>(need - rs.frames(), 256);
      tmp.resize(want * ch);
      bool src_ended = false;
      size_t got = ReadSource(tmp.data(), want, &src_ended);
      rs.hist.insert(rs.hist.end(), tmp.begin(), tmp.begin() + got * ch);
      if (got == 0) {
        if (src_ended) {
          rs.real_end = rs.start_index + static_cast<int64_t>(rs.frames());
          continue;
        }
        starved = true;
        break;
      }
    }
    if (starved) break;
    if (rs.real_end >= 0 && rs.start_index + center >= rs.real_end) {
      ended = true;
      break;
    }
    // One output frame.
    if (passthrough) {
      for (size_t c = 0; c < ch; c++) frame[c] = rs.hist[static_cast<size_t>(center) * ch + c];
    } else {
      std::fill(frame.begin(), frame.end(), 0.0f);
      for (int64_t k = center - static_cast<int64_t>(width) + 1;
           k <= center + static_cast<int64_t>(width); k++) {
        float w = Kernel(table, (rs.t - static_cast<double>(k)) * cutoff) * static_cast<float>(cutoff);
        if (w == 0) continue;
        const float* s = &rs.hist[static_cast<size_t>(k) * ch];
        for (size_t c = 0; c < ch; c++) frame[c] += s[c] * w;
      }
    }
    al += dl;
    ar += dr;
    float* o = mix + out_done * out_channels;
    float l, r;
    if (ch == 1) {
      l = r = frame[0];
    } else if (ch == 2) {
      l = frame[0];
      r = frame[1];
    } else {
      // FL FR FC LFE BL BR SL SR ...: the center and the surrounds at -3 dB.
      constexpr float k = 0.7071f;
      l = frame[0];
      r = frame[1];
      if (ch > 2) { l += k * frame[2]; r += k * frame[2]; }
      for (size_t c = 4; c + 1 < ch && c < 8; c += 2) { l += k * frame[c]; r += k * frame[c + 1]; }
    }
    l *= al;
    r *= ar;
    if (out_channels == 1) {
      o[0] += 0.5f * (l + r);
    } else {
      o[0] += l;
      o[1] += r;
    }
    rs.t += passthrough ? 1.0 : step;
  }
  applied_left_ = gl;
  applied_right_ = gr;

  // Played: source frames whose center the filter passed.
  int64_t center_index = rs.start_index + static_cast<int64_t>(std::floor(rs.t));
  if (rs.real_end >= 0) center_index = std::min(center_index, rs.real_end);
  // played_ counts frames since the track was created; the resampler's index
  // restarts at each Reset, so advance by the difference.
  if (center_index > rs.played_index) {
    played_ += static_cast<uint64_t>(center_index - rs.played_index);
    rs.played_index = center_index;
  }
  played_time_ns_ = MonotonicNs();

  // Starved for a while (an underrun, or a writer that stopped writing without
  // stop()): the frames the filter still holds back count as played, and the
  // resampler starts over with the next data. Otherwise the last few frames
  // would wait for data that may never come (AAudio's getFramesRead, a
  // position that must reach what was written).
  if (starved && out_done == 0 && rs.primed) {
    if (++starved_ticks_ >= 2) {
      int64_t end = rs.start_index + static_cast<int64_t>(rs.frames());
      if (end > rs.played_index) played_ += static_cast<uint64_t>(end - rs.played_index);
      rs.Reset();
      starved_ticks_ = 0;
    }
  } else if (!starved) {
    starved_ticks_ = 0;
  }

  // Drop history the filter no longer needs.
  int64_t keep_from = static_cast<int64_t>(std::floor(rs.t)) - static_cast<int64_t>(width) - 1;
  if (keep_from > 4096) {
    rs.hist.erase(rs.hist.begin(), rs.hist.begin() + keep_from * static_cast<int64_t>(ch));
    rs.t -= static_cast<double>(keep_from);
    rs.start_index += keep_from;
  }

  if (starved && out_done < frames && state_ == State::kPlaying && played_ > position_base_) {
    underrun_frames_ += static_cast<uint32_t>((frames - out_done) * rate_ / out_rate);
    if (!was_underrun_) {
      was_underrun_ = true;
      if (events_.underrun) PostEvent(events_.underrun);
    }
  } else if (out_done == frames) {
    was_underrun_ = false;
  }

  bool was_ended = end_signalled_;
  if (ended) {
    if (state_ == State::kStopping) {
      drained_ = true;
      state_ = State::kStopped;
    } else {
      // A static track at its end stays "playing" (as Android's), silent.
      state_ = params_.is_static ? State::kPlaying : state_;
    }
    room_.notify_all();
    if (events_.end && !end_signalled_) {
      end_signalled_ = true;
      PostEvent(events_.end);
    }
  } else {
    end_signalled_ = false;
  }
  QueueEvents(played_before - position_base_, played_ - position_base_);
  return out_done > 0 || (ended && !was_ended);
}

void Track::QueueEvents(uint64_t before, uint64_t after) {
  if (after <= before) return;
  if (!marker_reached_ && marker_ != 0 && before < marker_ && after >= marker_) {
    marker_reached_ = true;
    if (events_.marker) {
      auto fn = events_.marker;
      uint32_t m = marker_;
      PostEvent([fn, m] { fn(m); });
    }
  }
  if (update_period_ != 0 && next_update_ != 0) {
    while (after >= next_update_) {
      if (events_.new_position) {
        auto fn = events_.new_position;
        uint32_t p = static_cast<uint32_t>(next_update_);
        PostEvent([fn, p] { fn(p); });
      }
      next_update_ += update_period_;
    }
  }
}

}  // namespace gesher::media
