// Gesher: the capture engine, its streams, the file and silent sources
// (capture.h; docs/media.md "Recording", ADR 0059).

#include "capture.h"

#include <time.h>
#include <gesher/clocks.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "log.h"

namespace gesher::media {

namespace {

constexpr int kPeriodMs = 10;

int64_t MonotonicNs() {
  timespec ts;
  gesher::AndroidClockGettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// --- Sources paced by the clock -------------------------------------------------------

class PacedSource : public CaptureSource {
 public:
  bool Start() override {
    start_ = MonotonicNs();
    produced_ = 0;
    return true;
  }
  void Stop() override {}
  bool Pull(std::vector<float>* out) override {
    const int64_t elapsed = MonotonicNs() - start_;
    const uint64_t due = static_cast<uint64_t>(
        static_cast<__int128>(elapsed) * rate() / 1000000000);
    if (due <= produced_) return true;
    const size_t frames = static_cast<size_t>(due - produced_);
    const size_t at = out->size();
    out->resize(at + frames * channels());
    Fill(out->data() + at, frames);
    produced_ = due;
    return true;
  }

 protected:
  // Writes `frames` frames of what the source says now.
  virtual void Fill(float* dst, size_t frames) = 0;

 private:
  int64_t start_ = 0;
  uint64_t produced_ = 0;
};

class SilentSource : public PacedSource {
 public:
  explicit SilentSource(std::string why) : why_(std::move(why)) {}
  uint32_t rate() const override { return 48000; }
  uint32_t channels() const override { return 1; }
  std::string Describe() const override { return "none: " + why_ + " (silence)"; }
  std::string DeviceName() const override { return ""; }

 protected:
  void Fill(float* dst, size_t frames) override { std::fill(dst, dst + frames, 0.0f); }

 private:
  std::string why_;
};

// A WAV file (PCM 8/16/24/32-bit or float, any rate and channels), as float, looping.
class FileSource : public PacedSource {
 public:
  bool Load(const std::string& path, std::string* error) {
    path_ = path;
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
      *error = "cannot open " + path;
      return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    fclose(f);
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(bytes[o] | (bytes[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (bytes.size() < 12 || memcmp(bytes.data(), "RIFF", 4) != 0 || memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
      *error = path + " is not a WAV file";
      return false;
    }
    uint32_t fmt_tag = 0, bits = 0;
    size_t data_at = 0, data_size = 0;
    for (size_t o = 12; o + 8 <= bytes.size();) {
      const uint32_t size = u32(o + 4);
      if (memcmp(bytes.data() + o, "fmt ", 4) == 0) {
        if (size < 16 || size > bytes.size() - o - 8) {
          *error = path + ": incomplete WAV format";
          return false;
        }
        fmt_tag = u16(o + 8);
        channels_ = u16(o + 10);
        rate_ = u32(o + 12);
        bits = u16(o + 22);
        if (fmt_tag == 0xFFFE) {
          if (size < 40) {
            *error = path + ": incomplete WAV extended format";
            return false;
          }
          fmt_tag = u16(o + 8 + 24);  // the subformat
        }
      } else if (memcmp(bytes.data() + o, "data", 4) == 0) {
        data_at = o + 8;
        data_size = std::min<size_t>(size, bytes.size() - data_at);
      }
      o += size_t{8} + size + (size & 1);
    }
    const bool supported = (fmt_tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
                           (fmt_tag == 3 && bits == 32);
    if (data_at == 0 || channels_ == 0 || rate_ == 0 || !supported) {
      *error = path + ": no PCM data";
      return false;
    }
    const size_t bps = bits / 8;
    const size_t count = data_size / bps;
    samples_.resize(count);
    const uint8_t* p = bytes.data() + data_at;
    for (size_t i = 0; i < count; i++, p += bps) {
      float v = 0;
      if (fmt_tag == 3 && bits == 32) {
        memcpy(&v, p, 4);
      } else if (bits == 8) {
        v = (static_cast<int>(p[0]) - 128) / 128.0f;
      } else if (bits == 16) {
        v = static_cast<int16_t>(p[0] | (p[1] << 8)) / 32768.0f;
      } else if (bits == 24) {
        const uint32_t encoded = (static_cast<uint32_t>(p[0]) << 8) |
                                 (static_cast<uint32_t>(p[1]) << 16) |
                                 (static_cast<uint32_t>(p[2]) << 24);
        int32_t s;
        memcpy(&s, &encoded, sizeof(s));
        v = static_cast<float>(s / 2147483648.0);
      } else if (bits == 32) {
        int32_t s;
        memcpy(&s, p, 4);
        v = static_cast<float>(s / 2147483648.0);
      }
      samples_[i] = v;
    }
    frames_ = samples_.size() / channels_;
    if (frames_ == 0) {
      *error = path + ": no frames";
      return false;
    }
    return true;
  }

  uint32_t rate() const override { return rate_; }
  uint32_t channels() const override { return channels_; }
  std::string Describe() const override {
    return "file " + path_ + " (" + std::to_string(rate_) + " Hz, " + std::to_string(channels_) +
           " channels, " + std::to_string(frames_ * 1000 / rate_) + " ms, looping)";
  }
  std::string DeviceName() const override { return "Gesher audio source"; }
  bool Start() override {
    pos_ = 0;
    return PacedSource::Start();
  }

 protected:
  void Fill(float* dst, size_t frames) override {
    while (frames > 0) {
      const size_t take = std::min(frames, frames_ - pos_);
      memcpy(dst, samples_.data() + pos_ * channels_, take * channels_ * sizeof(float));
      dst += take * channels_;
      frames -= take;
      pos_ += take;
      if (pos_ == frames_) pos_ = 0;
    }
  }

 private:
  std::string path_;
  uint32_t rate_ = 0, channels_ = 0;
  std::vector<float> samples_;
  size_t frames_ = 0, pos_ = 0;
};

// The source this machine has (capture.h's order).
std::unique_ptr<CaptureSource> OpenSource(std::string* error) {
  const char* env = getenv("GESHER_AUDIO_SOURCE");
  if (env != nullptr && *env != '\0') {
#ifdef GESHER_CAPTURE_TEST_SOURCES
    MEDIA_LOGI("Gesher isolated capture fixture sources enabled");
    if (strcmp(env, "null") == 0) return std::make_unique<SilentSource>("explicit test source");
    if (strcmp(env, "loopback") == 0) return OpenWasapiCapture(true, error);
    auto file = std::make_unique<FileSource>();
    if (file->Load(env, error)) return file;
    return nullptr;
#else
    *error = "GESHER_AUDIO_SOURCE requires the isolated capture-test build";
    return nullptr;
#endif
  }
  return OpenWasapiCapture(false, error);
}

}  // namespace

// --- The engine ------------------------------------------------------------------------

class CaptureEngine {
 public:
  static CaptureEngine& Get() {
    static CaptureEngine* e = new CaptureEngine();
    return *e;
  }

  void Add(const std::shared_ptr<CaptureStream>& s) {
    std::lock_guard<std::mutex> lock(mu_);
    streams_.push_back(s);
  }

  // A stream stopped or was released. No stream lock is held by callers.
  void Wake() {
    // Destruction of the final weak-lock snapshot can occur while mu_ is held.
    // Never reacquire that mutex here; the worker owns the idle decision.
    restart_.store(true);
    cv_.notify_all();
  }

  int Prepare(std::string* error) { return Submit(nullptr, 0, error); }
  int StartStream(CaptureStream* stream, uint64_t generation) {
    return Submit(stream, generation, nullptr);
  }

  InputInfo Info() {
#ifdef GESHER_CAPTURE_TEST_SOURCES
    const char* fixture = getenv("GESHER_AUDIO_SOURCE");
    if (fixture != nullptr && *fixture != '\0') {
      std::string error;
      Prepare(&error); // Validate the explicit fixture before advertising it.
    }
#endif
    // This query describes availability without manufacturing a capture source.
    std::lock_guard<std::mutex> lock(mu_);
    if (source_ != nullptr) return info_;
    InputInfo result;
    result.device = false;
    const char* env = getenv("GESHER_AUDIO_SOURCE");
    if (env != nullptr && *env != '\0') {
#ifdef GESHER_CAPTURE_TEST_SOURCES
      result.description = "explicit capture test source (not opened)";
#else
      result.description = "test capture source unavailable in production";
#endif
      return result;
    }
    std::string name;
    if (HasWasapiCaptureEndpoint(&name)) {
      result.device = true;
      result.device_name = name;
      result.description = "WASAPI shared mode capture on " + name;
    } else {
      result.description = "no audio capture endpoint";
    }
    return result;
  }

 private:
  // With mu_ held.
  void Describe(const CaptureSource& s) {
    info_.sample_rate = s.rate();
    info_.channels = s.channels();
    info_.description = s.Describe();
    info_.device_name = s.DeviceName();
    info_.device = !info_.device_name.empty();
  }

  // With mu_ held: prune dead streams and optionally collect the started ones.
  // Presence checks do not need an owning snapshot or its allocation.
  bool Started(std::vector<std::shared_ptr<CaptureStream>>* out = nullptr) {
    bool any = false;
    for (auto it = streams_.begin(); it != streams_.end();) {
      std::shared_ptr<CaptureStream> s = it->lock();
      if (s == nullptr) {
        it = streams_.erase(it);
        continue;
      }
      if (s->started()) {
        any = true;
        if (out != nullptr) out->push_back(std::move(s));
      }
      ++it;
    }
    return any;
  }

  struct Command {
    CaptureStream* stream;
    uint64_t generation;
    bool done = false;
    int status = kCaptureUnavailable;
    std::string error;
  };

  void ResetIdle() {
    while (restart_.exchange(false)) {
      if (!Started()) {
        if (running_ && source_ != nullptr) source_->Stop();
        running_ = false;
      }
      // Dropping a weak-lock snapshot in Started may itself retire the last
      // owner. Its nonlocking Wake requests another check before activation.
    }
  }

  // On the COM-owning worker, with mu_ held. Activation acquires the stream
  // mutex in the same engine->stream order as Started(). No callbacks run here.
  void Process(Command& command) {
    ResetIdle();
    if (source_ == nullptr) {
      source_ = OpenSource(&command.error);
      if (source_ != nullptr) {
        Describe(*source_);
        MEDIA_LOGI("capture: %s", source_->Describe().c_str());
      }
    }
    command.status = source_ != nullptr ? kOk : kCaptureUnavailable;
    if (command.status == kOk && command.stream != nullptr) {
      if (!running_) {
        if (source_->Start()) running_ = true;
        else {
          command.error = source_->Describe() + " did not start";
          source_.reset();
          info_ = InputInfo{};
          command.status = kCaptureUnavailable;
        }
      }
      if (command.status == kOk) command.status = command.stream->Activate(command.generation);
    }
    if (command.status != kOk && !command.error.empty()) MEDIA_LOGE("capture: %s", command.error.c_str());
    command.done = true;
    cv_.notify_all();
  }

  int Submit(CaptureStream* stream, uint64_t generation, std::string* error) {
    Command command{stream, generation, false, kCaptureUnavailable, {}};
    std::unique_lock<std::mutex> lock(mu_);
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    if (thread_.get_id() == std::this_thread::get_id()) {
      // A capture callback may synchronously start another recorder. Push
      // invokes callbacks only after releasing both engine and stream locks.
      Process(command);
    } else {
      commands_.push_back(&command);
      cv_.notify_all();
      cv_.wait(lock, [&] { return command.done; });
    }
    if (error != nullptr) *error = command.error;
    return command.status;
  }

  void Run() {
    CoInitializeMta();
    RaiseAudioThreadPriority(true);
    std::vector<float> buf;
    std::vector<uint64_t> generations;
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
      ResetIdle();
      while (!commands_.empty()) {
        Command* command = commands_.front();
        commands_.erase(commands_.begin());
        Process(*command);
      }
      if (!Started()) {
        if (running_ && source_ != nullptr) source_->Stop();
        running_ = false;
        cv_.wait(lock, [this] { return !commands_.empty() || Started() || restart_; });
        continue;
      }
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
      buf.clear();
      const bool ok = source_->Pull(&buf);
      const int64_t now = MonotonicNs();
      lock.lock();
      if (restart_) {
        ResetIdle();
        if (!running_) continue;
      }
      std::vector<std::shared_ptr<CaptureStream>> targets;
      Started(&targets);
      if (!ok) {
        MEDIA_LOGE("capture: %s was disconnected", source_->Describe().c_str());
        source_->Stop();
        source_.reset();
        running_ = false;
        info_ = InputInfo{};
        // Terminal errors are state, not synthetic audio. Fail wakes readers;
        // API-specific notification runs on the API's own callback thread.
        for (auto& stream : targets) stream->Fail(kCaptureDeadObject);
        continue;
      }
      const uint32_t ch = source_->channels(), rate = source_->rate();
      if (ch == 0 || buf.empty()) continue;
      generations.resize(targets.size());
      for (size_t i = 0; i < targets.size(); ++i) {
        std::lock_guard<std::mutex> stream_lock(targets[i]->mu_);
        generations[i] = targets[i]->generation_;
      }
      lock.unlock();
      for (size_t i = 0; i < targets.size(); ++i) {
        targets[i]->Push(buf.data(), buf.size() / ch, rate, ch, now, generations[i]);
      }
      targets.clear();
      lock.lock();
    }
  }

  static void CoInitializeMta();

  std::mutex mu_;
  std::condition_variable cv_;
  std::thread thread_;
  std::vector<std::weak_ptr<CaptureStream>> streams_;
  std::unique_ptr<CaptureSource> source_;
  InputInfo info_;
  std::atomic<bool> restart_{false};
  bool running_ = false;
  std::vector<Command*> commands_;
};

void CaptureEngine::CoInitializeMta() { CaptureComInit(); }

// --- Streams ---------------------------------------------------------------------------

CaptureStream::CaptureStream(CaptureEngine* engine, const CaptureParams& params)
    : engine_(engine), params_(params) {
  frame_size_ = BytesPerSample(params_.format) * params_.channels;
  if (params_.capacity_frames == 0) params_.capacity_frames = params_.sample_rate / 2;
  ring_.assign(params_.capacity_frames * frame_size_, 0);
  last_.assign(params_.channels, 0.0f);
}

CaptureStream::~CaptureStream() { Release(); }

void CaptureStream::SetEvents(CaptureEvents events) {
  std::lock_guard<std::mutex> lock(mu_);
  events_ = std::move(events);
}

int CaptureStream::Start() {
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) return kInvalidOperation;
    if (error_ != kOk) return error_;
    if (started_) return kOk;
    generation = ++generation_;
  }
  return engine_->StartStream(this, generation);
}

int CaptureStream::Activate(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mu_);
  if (released_ || generation_ != generation) return kInvalidOperation;
  if (error_ != kOk) return error_;
  started_ = true;
  read_ = written_;
  primed_ = false;
  phase_ = 0;
  was_overrun_ = false;
  return kOk;
}

void CaptureStream::Fail(int error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_ || error_ != kOk) return;
    error_ = error;
    started_ = false;
    ++generation_;
  }
  cv_.notify_all();
}

int CaptureStream::error() const {
  std::lock_guard<std::mutex> lock(mu_);
  return error_;
}

void CaptureStream::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    started_ = false;
    ++generation_;
  }
  cv_.notify_all();
  engine_->Wake();
}

bool CaptureStream::started() const {
  std::lock_guard<std::mutex> lock(mu_);
  return started_;
}

void CaptureStream::Push(const float* src, size_t frames, uint32_t src_rate, uint32_t src_channels,
                         int64_t now_ns, std::optional<uint64_t> generation) {
  if (frames == 0 || src_channels == 0) return;
  std::vector<std::function<void()>> calls;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_ || released_ || (generation && *generation != generation_)) return;
    const uint32_t ch = params_.channels;
    // Linear interpolation from src_rate to the stream's rate; channels: mono
    // sources are copied to every channel, otherwise channel c takes c (or the
    // mean of the source for a mono stream).
    const double step = static_cast<double>(src_rate) / params_.sample_rate;
    auto source_frame = [&](size_t i, float* out) {
      const float* f = src + i * src_channels;
      for (uint32_t c = 0; c < ch; c++) {
        if (ch == 1 && src_channels > 1) {
          float sum = 0;
          for (uint32_t k = 0; k < src_channels; k++) sum += f[k];
          out[c] = sum / src_channels;
        } else {
          out[c] = f[std::min(c, src_channels - 1)];
        }
      }
    };
    const size_t cap = params_.capacity_frames;
    const size_t room = cap - static_cast<size_t>(written_ - read_);
    auto& out = converted_;
    out.clear();
    out.reserve(std::min(static_cast<size_t>(frames / step + 2), room) * ch);
    if (!primed_ && frames > 0) {
      source_frame(0, last_.data());
      primed_ = true;
    }
    // phase_: the position of the next output frame, in source frames, relative
    // to the frame before src[0] (last_).
    auto& cur = current_frame_;
    auto& prev = previous_frame_;
    size_t n = 0;
    while (true) {
      const double pos = phase_;
      const size_t i = static_cast<size_t>(pos);  // between frame i-1 (last_ when 0) and i
      if (i >= frames) break;
      const float t = static_cast<float>(pos - static_cast<double>(i));
      source_frame(i, cur.data());
      if (i == 0) {
        std::copy(last_.begin(), last_.end(), prev.begin());
      } else {
        source_frame(i - 1, prev.data());
      }
      // Advance through discarded frames too: phase/history and lost-frame
      // accounting must not depend on how much room the consumer left.
      if (n < room) {
        for (uint32_t c = 0; c < ch; c++) out.push_back(prev[c] + (cur[c] - prev[c]) * t);
      }
      ++n;
      phase_ += step;
    }
    phase_ -= static_cast<double>(frames);
    source_frame(frames - 1, last_.data());
    size_t take = std::min(n, room);
    if (take < n) {
      overruns_ += static_cast<uint32_t>(n - take);
      if (!was_overrun_) ++overrun_count_;
      was_overrun_ = true;
    } else {
      was_overrun_ = false;
    }
    if (take > 0) {
      const size_t slot = static_cast<size_t>(written_ % cap);
      const size_t first = std::min(take, cap - slot);
      FloatToPcm(out.data(), first * ch, params_.format, ring_.data() + slot * frame_size_);
      if (take > first) {
        FloatToPcm(out.data() + first * ch, (take - first) * ch, params_.format, ring_.data());
      }
    }
    const uint64_t before = written_;
    written_ += take;
    last_ns_ = now_ns;
    // Markers and periods count captured frames, as AudioRecord's do.
    if (events_.marker && marker_ > 0 && !marker_reached_ && before < marker_ && written_ >= marker_) {
      marker_reached_ = true;
      auto cb = events_.marker;
      uint32_t m = marker_;
      calls.push_back([cb, m] { cb(m); });
    }
    if (events_.new_position && update_period_ > 0) {
      while (written_ >= next_update_) {
        auto cb = events_.new_position;
        uint32_t p = static_cast<uint32_t>(next_update_);
        calls.push_back([cb, p] { cb(p); });
        next_update_ += update_period_;
      }
    }
    if (events_.data && take > 0) calls.push_back(events_.data);
  }
  cv_.notify_all();
  for (auto& c : calls) c();
}

ssize_t CaptureStream::Read(void* dst, size_t bytes, bool blocking) {
  const size_t want = bytes / frame_size_;
  if (want == 0) return 0;
  uint8_t* d = static_cast<uint8_t*>(dst);
  size_t done = 0;
  std::unique_lock<std::mutex> lock(mu_);
  while (done < want) {
    if (released_) break;
    size_t avail = static_cast<size_t>(written_ - read_);
    if (avail == 0) {
      if (error_ != kOk || !blocking || !started_) break;
      cv_.wait_for(lock, std::chrono::milliseconds(100));
      continue;
    }
    const size_t cap = params_.capacity_frames;
    size_t n = std::min(avail, want - done);
    if (read_ + n - 1 < read_) {
      // Preserve the frame counter's unsigned wrap before applying ring modulo.
      // The ordinary path below copies an admitted block in at most two spans.
      for (size_t k = 0; k < n; ++k) {
        const size_t slot = static_cast<size_t>((read_ + k) % cap);
        memcpy(d + (done + k) * frame_size_, ring_.data() + slot * frame_size_, frame_size_);
      }
    } else {
      const size_t slot = static_cast<size_t>(read_ % cap);
      const size_t first = std::min(n, cap - slot);
      memcpy(d + done * frame_size_, ring_.data() + slot * frame_size_, first * frame_size_);
      if (n > first) {
        memcpy(d + (done + first) * frame_size_, ring_.data(), (n - first) * frame_size_);
      }
    }
    read_ += n;
    done += n;
  }
  return done > 0 || error_ == kOk ? static_cast<ssize_t>(done * frame_size_) : error_;
}

size_t CaptureStream::Available() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<size_t>(written_ - read_);
}

bool CaptureStream::WaitForData(size_t frames, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return released_ || error_ != kOk || !started_ || static_cast<size_t>(written_ - read_) >= frames;
  }) && static_cast<size_t>(written_ - read_) >= frames;
}

uint64_t CaptureStream::frames_read() const {
  std::lock_guard<std::mutex> lock(mu_);
  return read_;
}

uint64_t CaptureStream::frames_captured() const {
  std::lock_guard<std::mutex> lock(mu_);
  return written_;
}

bool CaptureStream::Timestamp(int64_t* frames, int64_t* time_ns) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (last_ns_ == 0 || error_ != kOk) return false;
  *frames = static_cast<int64_t>(written_);
  *time_ns = last_ns_;
  return true;
}

uint32_t CaptureStream::overrun_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return overruns_;
}

uint32_t CaptureStream::overrun_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return overrun_count_;
}

CaptureStats CaptureStream::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return {written_, read_, overruns_, overrun_count_, last_ns_};
}

int CaptureStream::SetMarker(uint32_t frame) {
  std::lock_guard<std::mutex> lock(mu_);
  marker_ = frame;
  marker_reached_ = false;
  return kOk;
}

uint32_t CaptureStream::marker() const {
  std::lock_guard<std::mutex> lock(mu_);
  return marker_;
}

int CaptureStream::SetUpdatePeriod(uint32_t frames) {
  std::lock_guard<std::mutex> lock(mu_);
  update_period_ = frames;
  next_update_ = written_ + frames;
  return kOk;
}

uint32_t CaptureStream::update_period() const {
  std::lock_guard<std::mutex> lock(mu_);
  return update_period_;
}

void CaptureStream::Release() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) return;
    released_ = true;
    started_ = false;
    std::vector<float>().swap(converted_);
    ++generation_;
  }
  cv_.notify_all();
  engine_->Wake();
}

std::shared_ptr<CaptureStream> OpenCapture(const CaptureParams& params, std::string* error) {
  if (params.sample_rate < 4000 || params.sample_rate > 192000 || params.channels < 1 ||
      params.channels > 8) {
    *error = "unsupported capture format: " + std::to_string(params.sample_rate) + " Hz, " +
             std::to_string(params.channels) + " channels";
    return nullptr;
  }
  CaptureEngine& engine = CaptureEngine::Get();
  if (engine.Prepare(error) != kOk) return nullptr;
  std::shared_ptr<CaptureStream> s(new CaptureStream(&engine, params));
  engine.Add(s);
  return s;
}

InputInfo GetInputInfo() { return CaptureEngine::Get().Info(); }

size_t MinCaptureFrames(uint32_t rate) {
  return std::max<size_t>(static_cast<size_t>(rate) * 2 * kPeriodMs / 1000, 256);
}

void FloatToPcm(const float* src, size_t count, PcmFormat format, void* dst) {
  for (size_t i = 0; i < count; i++) {
    const float v = std::clamp(src[i], -1.0f, 1.0f);
    switch (format) {
      case PcmFormat::kU8:
        static_cast<uint8_t*>(dst)[i] = static_cast<uint8_t>(std::lround(v * 127.0f) + 128);
        break;
      case PcmFormat::kS16:
        static_cast<int16_t*>(dst)[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
        break;
      case PcmFormat::kS24Packed: {
        int32_t s = static_cast<int32_t>(std::lround(v * 8388607.0f));
        uint8_t* p = static_cast<uint8_t*>(dst) + 3 * i;
        p[0] = static_cast<uint8_t>(s);
        p[1] = static_cast<uint8_t>(s >> 8);
        p[2] = static_cast<uint8_t>(s >> 16);
        break;
      }
      case PcmFormat::kS32:
        static_cast<int32_t*>(dst)[i] = static_cast<int32_t>(std::llround(v * 2147483647.0));
        break;
      case PcmFormat::kFloat:
        static_cast<float*>(dst)[i] = v;
        break;
    }
  }
}

}  // namespace gesher::media
