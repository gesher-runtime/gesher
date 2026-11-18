// Host controls for the capture and encoder APIs. Recorder's worker state and
// the production MP4 muxer build independently decodable files from AAC packets.
#include "compat/media/record/recorder.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <algorithm>
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
static std::atomic<bool> fail_capture_restart = false;
static std::atomic<bool> hold_video_limit = false;
static std::atomic<bool> video_limit_waiting = false;
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
  if (fail_capture_restart) {
    hold_video_limit = false;
    hold_video_limit.notify_all();
    return -32;
  }
  if (released_) return -32;
  started_ = capture_running = true;
  read_ = written_;
  return 0;
}
bool CaptureStream::started() const {
  std::lock_guard lock(mu_);
  return started_;
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
    if (frame.time_us >= 120000 && hold_video_limit) {
      video_limit_waiting = true;
      hold_video_limit.wait(true);
    }
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
    return cv.wait_for(lock, 5s, [this] {
      return std::any_of(values.begin(), values.end(), [](const auto& event) {
        return event.first == 1 || (event.first == 2 && (event.second == 800 || event.second == 801));
      });
    });
  }
  void Check(int what, int extra) {
    std::lock_guard lock(mu);
    unsigned terminal = 0, approaching = 0;
    for (const auto& event : values) {
      if (event == std::make_pair(2, 802)) {
        assert(((what == 2 && extra == 801) || what == 1) && terminal == 0);
        ++approaching;
      } else {
        assert(event == std::make_pair(what, extra));
        ++terminal;
      }
    }
    assert(terminal == 1 && approaching <= 1);
    // The bounded AAC packets in this fixture cross 90% before the terminal
    // admission failure; a missing production 802 path must fail this gate.
    if (what == 2 && extra == 801) assert(approaching == 1);
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
  assert(capture_released && !capture_running && recorder.Resume() && recorder.Pause() && recorder.Resume());
  assert(!capture_running);  // public RECORDING state cannot reopen an automatic stop
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
  assert(!recorder.CanConfigure() && recorder.InError() && !recorder.Pause());
  assert(!recorder.Prepare(&error));
  recorder.Reset();
  assert(recorder.CanConfigure() && !recorder.InError());

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
  Configure(recorder, path, 0, 2048);
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  feed_audio(50);
  assert(events.Wait());
  auto size = Size(path);
  assert(size > 587 && size <= 2048);
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
  config.video_encoder = 2;
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
  assert(recorder.input_surface() == surface);
  surface->Submit(ns + 180000000);
  assert(surface->frames() == 3);
  auto size = Size(path);
  assert(recorder.Stop(&error) && Size(path) == size);
  assert(recorder.input_surface() == nullptr);
}

void ResumeFailureAtVideoLimit(const std::string& path) {
  Events events;
  Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
  std::string error;
  Configure(recorder, path, 120, 0);
  auto& config = recorder.config();
  config.video_source = 2;
  config.video_encoder = 2;
  config.width = 32;
  config.height = 32;
  assert(recorder.Prepare(&error) && recorder.Start(&error));
  auto* surface = static_cast<HostSurface*>(recorder.input_surface());
  assert(surface != nullptr);
  feed_audio(3);
  WaitForEncodes(3);
  timespec now {};
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  const int64_t ns = now.tv_sec * 1000000000LL + now.tv_nsec;

  // The worker already owns a frame that reaches the duration limit when
  // the paused recorder encounters a capture restart failure.
  hold_video_limit = true;
  video_limit_waiting = false;
  surface->Submit(ns);
  surface->Submit(ns + 150000000);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!video_limit_waiting && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  assert(video_limit_waiting && recorder.Pause());
  fail_capture_restart = true;
  assert(!recorder.Resume(&error) && error == "audio capture restart failed");
  fail_capture_restart = false;
  assert(capture_released && surface_abandoned && recorder.InError());
  assert(events.Wait());
  events.Check(1, 1);
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
  assert(recorder.InError() && !recorder.CanConfigure());
  recorder.Reset();
  assert(recorder.CanConfigure());
}

void ErrorRecovery(const std::string& path) {
  std::string error;
  bool invalid_state = false;
  {
    Recorder recorder({});
    Configure(recorder, "/dev/full", 0, 0);
    assert(recorder.Prepare(&error));
    assert(!recorder.Start(&error, &invalid_state) && !invalid_state);
    assert(recorder.InError() && !recorder.CanConfigure() && capture_released);
    assert(!recorder.Prepare(&error, &invalid_state) && invalid_state);
    recorder.Reset();
    assert(recorder.CanConfigure());
  }
  {
    Recorder recorder({});
    Configure(recorder, path + ".prepare", 0, 0);
    recorder.config().output_format = 11;  // Ogg/AAC is incompatible, both enum values valid.
    assert(!recorder.Prepare(&error, &invalid_state) && !invalid_state);
    assert(recorder.InError() && !recorder.CanConfigure());
    assert(!recorder.Prepare(&error, &invalid_state) && invalid_state);
    recorder.Reset();
    assert(recorder.CanConfigure());
  }
  {
    Events events;
    Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
    Configure(recorder, path + ".resume", 0, 0);
    assert(recorder.Prepare(&error) && recorder.Start(&error));
    feed_audio(3);
    WaitForEncodes(3);
    assert(recorder.Pause());
    fail_capture_restart = true;
    assert(!recorder.Resume(&error, &invalid_state) && !invalid_state);
    fail_capture_restart = false;
    assert(capture_released && !capture_running && recorder.InError());
    assert(events.Wait());
    events.Check(1, 1);
    assert(!recorder.Stop(&error, &invalid_state) && invalid_state);
    assert(!recorder.CanConfigure());
    recorder.Reset();
    assert(recorder.CanConfigure() && !recorder.InError());
  }
  {
    Events events;
    Recorder recorder([&](int what, int extra) { events.Record(what, extra); });
    Configure(recorder, path + ".missing-track", 0, 2048);
    auto& config = recorder.config();
    config.video_source = 2; config.video_encoder = 2;
    config.width = 32; config.height = 32;
    assert(recorder.Prepare(&error) && recorder.Start(&error));
    feed_audio(50);  // Exhaust the budget before the Surface supplies video.
    assert(events.Wait());
    events.Check(1, 1);
    assert(capture_released && surface_abandoned && Size(path + ".missing-track") <= 2048);
    assert(!recorder.Stop(&error, &invalid_state) && !invalid_state && recorder.InError());
    assert(error.find("configured track") != std::string::npos);
  }
  for (int encoder : {0, 1, 2, 4, 5, 6, 77}) {
    Recorder recorder({});
    Configure(recorder, path + ".codec", 0, 0);
    recorder.config().audio_encoder = encoder;
    assert(!recorder.Prepare(&error, &invalid_state) && !invalid_state);
    assert(recorder.InError() && !recorder.CanConfigure());
  }
  {
    Recorder recorder({});
    assert(recorder.CanSetSource(true) && recorder.CanSetSource(false));
    assert(!recorder.CanSetOutputFormat() && !recorder.CanSetEncoder(true));
    Configure(recorder, path + ".missing-encoder", 0, 0);
    recorder.config().audio_encoder = -1;
    assert(!recorder.Prepare(&error, &invalid_state) && invalid_state);
    assert(!recorder.InError() && recorder.CanSetEncoder(true));
    recorder.config().audio_encoder = 3;
    assert(!recorder.CanSetSource(true) && !recorder.CanSetOutputFormat());
    assert(!recorder.CanSetEncoder(true) && !recorder.CanSetEncoder(false));
    assert(recorder.Prepare(&error));
  }
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
  ResumeFailureAtVideoLimit(output + "/resume-limit.mp4");
  WriteFailure(output + "/write-failure.m4a");
  ErrorRecovery(output + "/error-recovery");
  std::cout << "Recorder worker lifecycle checks passed\n";
}
