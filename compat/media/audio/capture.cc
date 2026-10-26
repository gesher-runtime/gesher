// Gesher: the capture engine, its streams, the file and silent sources
// (capture.h; docs/media.md "Recording", ADR 0059).

#include "capture.h"

#include <time.h>

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
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// --- Sources paced by the clock -------------------------------------------------------

class PacedSource : public CaptureSource {
 public:
  bool Start() override {
    start_ = std::chrono::steady_clock::now();
    produced_ = 0;
    return true;
  }
  void Stop() override {}
  bool Pull(std::vector<float>* out) override {
    auto elapsed = std::chrono::steady_clock::now() - start_;
    const uint64_t due = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() * rate() / 1000000);
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
  std::chrono::steady_clock::time_point start_;
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
      if (memcmp(bytes.data() + o, "fmt ", 4) == 0 && o + 8 + 16 <= bytes.size()) {
        fmt_tag = u16(o + 8);
        channels_ = u16(o + 10);
        rate_ = u32(o + 12);
        bits = u16(o + 22);
        if (fmt_tag == 0xFFFE && size >= 40) fmt_tag = u16(o + 8 + 24);  // the subformat
      } else if (memcmp(bytes.data() + o, "data", 4) == 0) {
        data_at = o + 8;
        data_size = std::min<size_t>(size, bytes.size() - data_at);
      }
      o += 8 + size + (size & 1);
    }
    if (data_at == 0 || channels_ == 0 || rate_ == 0 || (fmt_tag != 1 && fmt_tag != 3)) {
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
        int32_t s = (p[0] << 8) | (p[1] << 16) | (p[2] << 24);
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
    for (size_t i = 0; i < frames; i++) {
      memcpy(dst + i * channels_, samples_.data() + pos_ * channels_, channels_ * sizeof(float));
      pos_ = (pos_ + 1) % frames_;
    }
  }

 private:
  std::string path_;
  uint32_t rate_ = 0, channels_ = 0;
  std::vector<float> samples_;
  size_t frames_ = 0, pos_ = 0;
};

// The source this machine has (capture.h's order).
std::unique_ptr<CaptureSource> OpenSource() {
  const char* env = getenv("GESHER_AUDIO_SOURCE");
  std::string error;
  if (env != nullptr && *env != '\0' && strcmp(env, "loopback") != 0 && strcmp(env, "null") != 0) {
    auto f = std::make_unique<FileSource>();
    if (f->Load(env, &error)) return f;
    MEDIA_LOGE("capture: GESHER_AUDIO_SOURCE: %s", error.c_str());
    return std::make_unique<SilentSource>(error);
  }
  if (env != nullptr && strcmp(env, "null") == 0) return std::make_unique<SilentSource>("GESHER_AUDIO_SOURCE=null");
  const bool loopback = env != nullptr && strcmp(env, "loopback") == 0;
  auto w = OpenWasapiCapture(loopback, &error);
  if (w != nullptr) return w;
  return std::make_unique<SilentSource>(error);
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

  // A stream started or stopped: the source runs while any stream is started.
  void Wake() {
    std::lock_guard<std::mutex> lock(mu_);
    // Remember the idle transition even if the next stream starts before
    // Run wakes. File sources restart at that boundary, just as after a wait.
    if (Started().empty()) restart_ = true;
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    cv_.notify_all();
  }

  InputInfo Info() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!info_known_) {
      // Described without capturing (the capture thread opens the source).
      const char* env = getenv("GESHER_AUDIO_SOURCE");
      std::string name;
      if (env != nullptr && *env != '\0' && strcmp(env, "loopback") != 0 && strcmp(env, "null") != 0) {
        FileSource f;
        std::string error;
        if (f.Load(env, &error)) {
          Describe(f);
        } else {
          Describe(SilentSource(error));
        }
      } else if (env != nullptr && strcmp(env, "null") == 0) {
        Describe(SilentSource("GESHER_AUDIO_SOURCE=null"));
      } else if (env != nullptr && strcmp(env, "loopback") == 0) {
        info_.description = "WASAPI loopback of the default output";
        info_.device_name = "Gesher audio loopback";
        info_.device = true;
        info_.sample_rate = 48000;
        info_.channels = 2;
        info_.device_type = 0x80000000 | 0x100000;  // AUDIO_DEVICE_IN_REMOTE_SUBMIX-like
        info_known_ = true;
      } else if (HasWasapiCaptureEndpoint(&name)) {
        info_.description = "WASAPI shared mode capture on " + name;
        info_.device_name = name;
        info_.device = true;
        info_known_ = true;
      } else {
        Describe(SilentSource("no audio capture endpoint"));
      }
    }
    return info_;
  }

 private:
  // With mu_ held.
  void Describe(const CaptureSource& s) {
    info_.sample_rate = s.rate();
    info_.channels = s.channels();
    info_.description = s.Describe();
    info_.device_name = s.DeviceName();
    info_.device = !info_.device_name.empty();
    info_known_ = true;
  }

  // With mu_ held: the streams alive and started.
  std::vector<std::shared_ptr<CaptureStream>> Started() {
    std::vector<std::shared_ptr<CaptureStream>> out;
    for (auto it = streams_.begin(); it != streams_.end();) {
      std::shared_ptr<CaptureStream> s = it->lock();
      if (s == nullptr) {
        it = streams_.erase(it);
        continue;
      }
      if (s->started()) out.push_back(std::move(s));
      ++it;
    }
    return out;
  }

  void Run() {
    // The source's COM objects live on this thread.
    CoInitializeMta();
    RaiseAudioThreadPriority(true);
    std::vector<float> buf;
    std::unique_lock<std::mutex> lock(mu_);
    bool running = false;
    for (;;) {
      if (restart_) {
        if (running && source_ != nullptr) source_->Stop();
        running = false;
        restart_ = false;
      }
      if (Started().empty()) {
        if (running && source_ != nullptr) source_->Stop();
        running = false;
        cv_.wait(lock, [this] { return !Started().empty(); });
      }
      if (!running) {
        if (source_ == nullptr) {
          source_ = OpenSource();
          MEDIA_LOGI("capture: %s", source_->Describe().c_str());
        }
        if (!source_->Start()) {
          MEDIA_LOGE("capture: %s did not start; silence", source_->Describe().c_str());
          source_ = std::make_unique<SilentSource>(source_->Describe() + " did not start");
          source_->Start();
        }
        running = true;
      }
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(kPeriodMs));
      buf.clear();
      const bool ok = source_->Pull(&buf);
      const int64_t now = MonotonicNs();
      lock.lock();
      // Stop/start may have happened during Pull. Do not give a new stream
      // samples left over from the recording that just stopped.
      if (restart_) continue;
      if (!ok) {
        MEDIA_LOGW("capture: %s is gone; silence", source_->Describe().c_str());
        source_ = std::make_unique<SilentSource>("the input was removed");
        source_->Start();
        continue;
      }
      const uint32_t ch = source_->channels(), rate = source_->rate();
      if (ch == 0 || buf.empty()) continue;
      std::vector<std::shared_ptr<CaptureStream>> targets = Started();
      // Pushed without the engine's lock: a stream's events may start or stop streams.
      lock.unlock();
      for (auto& s : targets) s->Push(buf.data(), buf.size() / ch, rate, ch, now);
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
  bool info_known_ = false;
  bool restart_ = false;
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
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (released_) return kInvalidOperation;
    if (started_) return kOk;
    started_ = true;
    // A recording starts from now: what an earlier one left is dropped.
    read_ = written_;
    primed_ = false;
    phase_ = 0;
  }
  engine_->Wake();
  return kOk;
}

void CaptureStream::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    started_ = false;
  }
  cv_.notify_all();
  engine_->Wake();
}

bool CaptureStream::started() const {
  std::lock_guard<std::mutex> lock(mu_);
  return started_;
}

void CaptureStream::Push(const float* src, size_t frames, uint32_t src_rate, uint32_t src_channels,
                         int64_t now_ns) {
  if (frames == 0 || src_channels == 0) return;
  std::vector<std::function<void()>> calls;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const uint32_t ch = params_.channels;
    // Linear interpolation from src_rate to the stream's rate; channels: mono
    // sources are copied to every channel, otherwise channel c takes c (or the
    // mean of the source for a mono stream).
    const double step = static_cast<double>(src_rate) / params_.sample_rate;
    std::vector<float> in_frame(ch);
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
    std::vector<float> out;
    out.reserve(static_cast<size_t>(frames / step + 2) * ch);
    if (!primed_ && frames > 0) {
      source_frame(0, last_.data());
      primed_ = true;
    }
    // phase_: the position of the next output frame, in source frames, relative
    // to the frame before src[0] (last_).
    std::vector<float> cur(ch);
    while (true) {
      const double pos = phase_;
      const size_t i = static_cast<size_t>(pos);  // between frame i-1 (last_ when 0) and i
      if (i >= frames) break;
      const float t = static_cast<float>(pos - static_cast<double>(i));
      source_frame(i, cur.data());
      std::vector<float> prev(ch);
      if (i == 0) {
        prev = last_;
      } else {
        source_frame(i - 1, prev.data());
      }
      for (uint32_t c = 0; c < ch; c++) out.push_back(prev[c] + (cur[c] - prev[c]) * t);
      phase_ += step;
    }
    phase_ -= static_cast<double>(frames);
    source_frame(frames - 1, last_.data());
    const size_t n = out.size() / ch;
    const size_t cap = params_.capacity_frames;
    size_t room = cap - static_cast<size_t>(written_ - read_);
    size_t take = std::min(n, room);
    if (take < n) overruns_ += static_cast<uint32_t>(n - take);
    std::vector<uint8_t> pcm(take * frame_size_);
    FloatToPcm(out.data(), take * ch, params_.format, pcm.data());
    for (size_t k = 0; k < take; k++) {
      const size_t slot = static_cast<size_t>((written_ + k) % cap);
      memcpy(ring_.data() + slot * frame_size_, pcm.data() + k * frame_size_, frame_size_);
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
  uint8_t* d = static_cast<uint8_t*>(dst);
  size_t done = 0;
  std::unique_lock<std::mutex> lock(mu_);
  while (done < want) {
    if (released_) break;
    size_t avail = static_cast<size_t>(written_ - read_);
    if (avail == 0) {
      if (!blocking || !started_) break;
      cv_.wait_for(lock, std::chrono::milliseconds(100));
      continue;
    }
    const size_t cap = params_.capacity_frames;
    size_t n = std::min(avail, want - done);
    for (size_t k = 0; k < n; k++) {
      const size_t slot = static_cast<size_t>((read_ + k) % cap);
      memcpy(d + (done + k) * frame_size_, ring_.data() + slot * frame_size_, frame_size_);
    }
    read_ += n;
    done += n;
  }
  return static_cast<ssize_t>(done * frame_size_);
}

size_t CaptureStream::Available() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<size_t>(written_ - read_);
}

bool CaptureStream::WaitForData(size_t frames, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mu_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
    return released_ || !started_ || static_cast<size_t>(written_ - read_) >= frames;
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
  if (last_ns_ == 0) return false;
  *frames = static_cast<int64_t>(written_);
  *time_ns = last_ns_;
  return true;
}

uint32_t CaptureStream::overrun_frames() const {
  std::lock_guard<std::mutex> lock(mu_);
  return overruns_;
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
