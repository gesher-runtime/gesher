// Gesher media: the sinks paced by the clock (sink.h): a WAV capture of the
// mixed output (GESHER_AUDIO_CAPTURE), and nothing at all.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine.h"
#include "log.h"
#include "shared_state.h"
#include "sink.h"

namespace gesher::media {
namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;

// Frames due by the clock since the sink opened.
class Clock {
 public:
  Clock() {
    QueryPerformanceFrequency(&freq_);
    QueryPerformanceCounter(&start_);
  }
  uint64_t Due(uint32_t rate) const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(static_cast<double>(now.QuadPart - start_.QuadPart) * rate /
                                 static_cast<double>(freq_.QuadPart));
  }

 private:
  LARGE_INTEGER freq_, start_;
};

class ClockedSink : public Sink {
 public:
  uint32_t rate() const override { return kRate; }
  uint32_t channels() const override { return kChannels; }
  uint32_t latency_frames() const override { return kRate / 50; }  // 20 ms, as a device's

  bool Tick(Mixer* mixer) override {
    uint64_t due = clock_.Due(kRate);
    if (due <= done_) return true;
    size_t frames = static_cast<size_t>(due - done_);
    // After a stall (the machine was busy), catch up in pieces: the timeline
    // stays continuous, as a capture should be.
    while (frames > 0) {
      size_t n = std::min<size_t>(frames, 4800);
      Render(mixer, n);
      frames -= n;
      done_ += n;
    }
    return true;
  }

 protected:
  virtual void Render(Mixer* mixer, size_t frames) = 0;

  Clock clock_;
  uint64_t done_ = 0;
};

class NullSink : public ClockedSink {
 public:
  explicit NullSink(std::string reason) : reason_(std::move(reason)) {}
  std::string Describe() const override { return "none (" + reason_ + "); nothing is heard"; }

 protected:
  void Render(Mixer* mixer, size_t frames) override {
    buf_.resize(frames * kChannels);
    for (int g = 0; g < kNumGroups; g++) mixer->MixGroup(g, buf_.data(), frames);
  }

 private:
  std::string reason_;
  std::vector<float> buf_;
};

class CaptureSink : public ClockedSink {
 public:
  CaptureSink(FILE* f, std::string path) : f_(f), path_(std::move(path)) { WriteHeader(); }
  ~CaptureSink() override {
    WriteHeader();
    fclose(f_);
  }

  std::string DeviceName() const override { return "Gesher audio capture"; }
  std::string Describe() const override {
    return "capture to " + path_ + " (48000 Hz, 2 channels, 16-bit; the stream volumes applied)";
  }

 protected:
  void Render(Mixer* mixer, size_t frames) override {
    sum_.assign(frames * kChannels, 0.0f);
    buf_.resize(frames * kChannels);
    for (int g = 0; g < kNumGroups; g++) {
      if (!mixer->MixGroup(g, buf_.data(), frames)) continue;
      // What the group's Windows session volume would do.
      float gain = StreamGain(StreamOfGroup(g));
      for (size_t i = 0; i < buf_.size(); i++) sum_[i] += buf_[i] * gain;
    }
    pcm_.resize(sum_.size());
    for (size_t i = 0; i < sum_.size(); i++) {
      float v = std::clamp(sum_[i], -1.0f, 1.0f);
      pcm_[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }
    fwrite(pcm_.data(), sizeof(int16_t), pcm_.size(), f_);
    data_bytes_ += static_cast<uint32_t>(pcm_.size() * sizeof(int16_t));
    // The sizes, every time: the file is complete whenever the process ends.
    WriteHeader();
  }

 private:
  void WriteHeader() {
    long pos = ftell(f_);
    uint8_t h[44];
    auto u32 = [&h](int off, uint32_t v) { memcpy(h + off, &v, 4); };
    auto u16 = [&h](int off, uint16_t v) { memcpy(h + off, &v, 2); };
    memcpy(h, "RIFF", 4);
    u32(4, 36 + data_bytes_);
    memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);  // PCM
    u16(22, kChannels);
    u32(24, kRate);
    u32(28, kRate * kChannels * 2);
    u16(32, kChannels * 2);
    u16(34, 16);
    memcpy(h + 36, "data", 4);
    u32(40, data_bytes_);
    fseek(f_, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f_);
    if (pos > 0) fseek(f_, pos, SEEK_SET);
    fflush(f_);
  }

  FILE* f_;
  std::string path_;
  uint32_t data_bytes_ = 0;
  std::vector<float> sum_, buf_;
  std::vector<int16_t> pcm_;
};

std::string Narrow(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  return s;
}

}  // namespace

std::unique_ptr<Sink> OpenCaptureSink(const std::wstring& where, std::string* error) {
  // A directory (existing, or ending in a separator): one file per process.
  std::wstring path = where;
  DWORD attrs = GetFileAttributesW(where.c_str());
  bool dir = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) ||
             where.back() == L'/' || where.back() == L'\\';
  if (dir) {
    CreateDirectoryW(where.c_str(), nullptr);
    path = where;
    if (path.back() != L'/' && path.back() != L'\\') path += L'/';
    path += L"audio-" + std::to_wstring(GetCurrentProcessId()) + L".wav";
  }
  FILE* f = _wfopen(path.c_str(), L"w+b");
  if (f == nullptr) {
    *error = "cannot write " + Narrow(path);
    return nullptr;
  }
  return std::make_unique<CaptureSink>(f, Narrow(path));
}

std::unique_ptr<Sink> OpenNullSink(const std::string& reason) {
  return std::make_unique<NullSink>(reason);
}

}  // namespace gesher::media
