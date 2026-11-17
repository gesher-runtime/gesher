// Host controls for the capture and encoder APIs. Recorder's worker state and
// the production MP4 muxer build independently decodable files from AAC packets.
#include "compat/media/record/recorder.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>

using namespace std::chrono_literals;

struct AMediaFormat {
  std::string mime;
  std::map<std::string, int32_t> integers;
};

void AMediaFormat_setString(AMediaFormat* format, const char*, const char* value) {
  format->mime = value;
}
void AMediaFormat_setInt32(AMediaFormat* format, const char* key, int32_t value) {
  format->integers[key] = value;
}

namespace gesher::media {

Format::Format() : f_(new AMediaFormat) {}
Format::~Format() { delete f_; }
std::string Format::Mime() const { return f_->mime; }
bool Format::GetInt32(const char* key, int32_t* out) const {
  auto it = f_->integers.find(key);
  if (it == f_->integers.end()) return false;
  *out = it->second;
  return true;
}
bool Format::GetBuffer(const char*, std::vector<uint8_t>*) const { return false; }

static std::function<void(unsigned)> feed_audio;
static std::atomic<unsigned> capture_reads = 0;
static std::atomic<unsigned> encoded_batches = 0;
static std::atomic<bool> capture_running = false;
static std::atomic<bool> capture_released = false;
static std::vector<std::vector<uint8_t>> access_units;
static std::vector<uint8_t> avc_access_unit;
static std::atomic<bool> surface_abandoned = false;

CaptureStream::CaptureStream(CaptureEngine* engine, const CaptureParams& params)
    : engine_(engine), params_(params), frame_size_(2 * params.channels) {}
CaptureStream::~CaptureStream() { Release(); }
std::shared_ptr<CaptureStream> OpenCapture(const CaptureParams& p, std::string*) {
  std::shared_ptr<CaptureStream> stream(new CaptureStream(nullptr, p));
  capture_released = false;
  capture_reads = 0;
  encoded_batches = 0;
  feed_audio = [owner = std::weak_ptr<CaptureStream>(stream)](unsigned batches) {
    if (auto capture = owner.lock()) {
      {
        std::lock_guard lock(capture->mu_);
        capture->written_ += batches;
      }
      capture->cv_.notify_all();
    }
  };
  return stream;
}
int CaptureStream::Start() {
  std::lock_guard lock(mu_);
  if (released_) return -1;
  started_ = capture_running = true;
  read_ = written_;
  return 0;
}
void CaptureStream::Stop() {
  {
    std::lock_guard lock(mu_);
    started_ = capture_running = false;
  }
  cv_.notify_all();
}
void CaptureStream::Release() {
  {
    std::lock_guard lock(mu_);
    started_ = capture_running = false;
    released_ = capture_released = true;
  }
  cv_.notify_all();
}
ssize_t CaptureStream::Read(void* dst, size_t bytes, bool) {
  std::unique_lock lock(mu_);
  cv_.wait(lock, [this] { return !started_ || read_ != written_; });
  if (!started_) return 0;
  ++read_;
  ++capture_reads;
  auto* pcm = static_cast<int16_t*>(dst);
  std::fill_n(pcm, bytes / sizeof(int16_t), 1234);
  return static_cast<ssize_t>(bytes);
}
void RaiseAudioThreadPriority(bool) {}

class HostEncoder : public AudioEncoder {
 public:
  std::string Name() const override { return "fixture-aac"; }
  bool Encode(const int16_t*, size_t, int64_t pts, std::vector<EncodedPacket>* out) override {
    if (next_ == 0) out->push_back({{0x11, 0x88}, 0, false, true});  // LC, 48000 Hz, mono
    out->push_back({access_units[next_++ % access_units.size()], pts, true, false});
    ++encoded_batches;
    return true;
  }
  bool Drain(std::vector<EncodedPacket>*) override { return true; }
  size_t frame_size() const override { return 1024; }

 private:
  size_t next_ = 0;
};

std::unique_ptr<AudioEncoder> AudioEncoder::Create(const std::string&, const AudioEncoderParams&,
                                                    std::string*) {
  return std::make_unique<HostEncoder>();
}
class HostVideoEncoder : public VideoEncoder {
 public:
  std::string Name() const override { return "fixture-avc"; }
  bool Encode(const VideoFrame& frame, bool, std::vector<EncodedPacket>* out) override {
    out->push_back({avc_access_unit, frame.time_us, true, false});
    return true;
  }
  bool Drain(std::vector<EncodedPacket>*) override { return true; }
};

class HostSurface : public InputSurface {
 public:
  jobject NewJavaSurface(JNIEnv*) override { return nullptr; }
  void SetCallback(InputFrameCallback callback) override { callback_ = std::move(callback); }
  uint64_t frames() const override { return count_; }
  void Abandon() override {
    std::lock_guard lock(mu_);
    surface_abandoned = true;
  }
  void Submit(int64_t time_ns) {
    std::lock_guard lock(mu_);
    if (!surface_abandoned) {
      ++count_;
      uint32_t pixel = 0xff0000ff;
      callback_(&pixel, 1, 1, time_ns);
    }
  }

 private:
  std::mutex mu_;
  InputFrameCallback callback_;
  std::atomic<uint64_t> count_{0};
};

std::unique_ptr<VideoEncoder> VideoEncoder::Create(const std::string&, const VideoEncoderParams&,
                                                    std::string*) {
  return std::make_unique<HostVideoEncoder>();
}
InputSurface* CreateInputSurface(int, int) {
  surface_abandoned = false;
  return new HostSurface;
}
void VideoFrame::Allocate(int, int) {}
void RgbaToI420(const uint32_t*, int, int, VideoFrame*) {}
std::unique_ptr<Muxer> CreateWebmMuxer(int, std::string*) { return nullptr; }

struct Events {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::pair<int, int>> values;

  void Record(int what, int extra) {
    {
      std::lock_guard lock(mu);
      values.emplace_back(what, extra);
    }
    cv.notify_all();
  }
  bool Wait() {
    std::unique_lock lock(mu);
    return cv.wait_for(lock, 5s, [this] { return !values.empty(); });
  }
  void Check(int what, int extra) {
    std::lock_guard lock(mu);
    assert(values.size() == 1);
    assert(values[0] == std::make_pair(what, extra));
  }
};

void Configure(Recorder& recorder, const std::string& path, int duration, int size) {
  auto& config = recorder.config();
  config.output_format = 2;  // MPEG-4
  config.audio_source = 1;   // microphone
  config.audio_encoder = 3;  // AAC
  config.sample_rate = 48000;
  config.max_duration_ms = duration;
  config.max_file_size = size;
  config.fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  assert(config.fd >= 0);
}

void WaitForEncodes(unsigned expected) {
  auto until = std::chrono::steady_clock::now() + 5s;
  while (encoded_batches < expected && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(1ms);
  }
  assert(encoded_batches == expected);
}

off_t Size(const std::string& path) {
  struct stat info {};
  assert(stat(path.c_str(), &info) == 0);
  return info.st_size;
}

void DurationAndPause(const std::string& path) {
  Events events;
  Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
  std::string error;
  assert(!recorder.Pause() && !recorder.Resume());
  assert(!recorder.Start(&error) && !recorder.Stop(&error));
  Configure(recorder, path, 120, 0);
  assert(recorder.Prepare(&error) && !recorder.CanConfigure());
  assert(!recorder.Prepare(&error) && !recorder.Pause() && !recorder.Resume());
  assert(recorder.Start(&error) && !recorder.Start(&error));
  assert(recorder.Resume());
  feed_audio(3);
  WaitForEncodes(3);
  assert(recorder.Pause() && recorder.Pause());
  assert(!capture_running);
  feed_audio(40);
  std::this_thread::sleep_for(150ms);
  assert(capture_reads == 3);
  assert(recorder.Resume() && recorder.Resume() && capture_running);
  feed_audio(40);
  assert(events.Wait());
  events.Check(2, 800);
  assert(capture_released && !capture_running && !recorder.Resume());
  assert(Size(path) > 0);
  // Snapshot the finalized bytes before any explicit stop.
  std::ifstream in(path, std::ios::binary);
  std::ofstream snapshot(path + ".ready", std::ios::binary);
  snapshot << in.rdbuf();
  assert(in && snapshot);
  assert(recorder.Stop(&error));
  events.Check(2, 800);
  assert(recorder.CanConfigure() && recorder.config().audio_source == -1);
  assert(!recorder.Stop(&error));

  Configure(recorder, path + ".reuse", -1, -1);
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  assert(!recorder.Stop(&error));  // No accepted samples.
  assert(error.find("nothing was recorded") != std::string::npos);
  assert(recorder.CanConfigure() && !recorder.Pause());

  Configure(recorder, path + ".unlimited", -1, -1);
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  feed_audio(10);
  WaitForEncodes(10);
  assert(recorder.Stop(&error));
  events.Check(2, 800);
}

void SizeLimit(const std::string& path) {
  Events events;
  Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
  std::string error;
  Configure(recorder, path, 0, 1000);
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  feed_audio(50);
  assert(events.Wait());
  auto size = Size(path);
  assert(size > 1000 && size < 2400);
  assert(capture_released && !capture_running && capture_reads < 50);
  feed_audio(50);
  assert(recorder.Stop(&error) && Size(path) == size);
  events.Check(2, 801);
}

void MixedSources(const std::string& path) {
  Events events;
  Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
  std::string error;
  Configure(recorder, path, 120, 0);
  auto& config = recorder.config();
  config.video_source = 2;
  config.width = 32;
  config.height = 32;
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  auto* surface = static_cast<HostSurface*>(recorder.input_surface());
  assert(surface != nullptr);
  feed_audio(3);
  WaitForEncodes(3);
  timespec now {};
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  int64_t ns = now.tv_sec * 1000000000LL + now.tv_nsec;
  surface->Submit(ns);
  surface->Submit(ns + 60000000);
  surface->Submit(ns + 150000000);
  // Video reaches the limit while the capture worker is blocked in Read().
  assert(events.Wait());
  events.Check(2, 800);
  assert(capture_released && surface_abandoned);
  surface->Submit(ns + 180000000);
  assert(surface->frames() == 3);
  auto size = Size(path);
  assert(recorder.Stop(&error) && Size(path) == size);
  assert(recorder.input_surface() == nullptr);
}

void WriteFailure(const std::string& path) {
  Events events;
  Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
  std::string error;
  Configure(recorder, path, 0, 0);
  assert(recorder.Prepare(&error));
  rlimit original {};
  assert(getrlimit(RLIMIT_FSIZE, &original) == 0);
  auto restricted = original;
  restricted.rlim_cur = 800;
  assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR);
  assert(setrlimit(RLIMIT_FSIZE, &restricted) == 0);
  assert(recorder.Start(&error));
  feed_audio(50);
  assert(events.Wait());
  assert(setrlimit(RLIMIT_FSIZE, &original) == 0);
  assert(capture_released && !capture_running);
  assert(!recorder.Stop(&error) && error.find("MPEG-4 writer") != std::string::npos);
  events.Check(1, 1);
}

void ReadAccessUnits(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)), {});
  for (size_t offset = 0; offset < data.size();) {
    assert(data.size() - offset >= 7 && data[offset] == 0xff && (data[offset + 1] & 0xf1) == 0xf1);
    size_t length = ((data[offset + 3] & 3u) << 11) | (data[offset + 4] << 3) | (data[offset + 5] >> 5);
    assert(length > 7 && length <= data.size() - offset);
    access_units.emplace_back(data.begin() + offset + 7, data.begin() + offset + length);
    offset += length;
  }
  assert(access_units.size() >= 10);
}
}  // namespace gesher::media

int main(int argc, char** argv) {
  assert(argc == 4);
  using namespace gesher::media;
  ReadAccessUnits(argv[1]);
  std::ifstream avc(argv[2], std::ios::binary);
  avc_access_unit.assign(std::istreambuf_iterator<char>(avc), {});
  assert(!avc_access_unit.empty());
  std::string output = argv[3];
  DurationAndPause(output + "/duration.m4a");
  SizeLimit(output + "/size.m4a");
  MixedSources(output + "/mixed.mp4");
  WriteFailure(output + "/write-failure.m4a");
  std::cout << "Recorder worker lifecycle checks passed\n";
}
