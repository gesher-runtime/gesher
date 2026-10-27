// Gesher media: the recorder (recorder.h; ADR 0059).

#include "recorder.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <media/NdkMediaFormat.h>

#include "../audio/log.h"

namespace gesher::media {

namespace {

// MediaRecorder's constants.
constexpr int kOutDefault = 0, kOut3gpp = 1, kOutMpeg4 = 2, kOutAdts = 6, kOutWebm = 9, kOutOgg = 11;
constexpr int kAudioDefault = 0, kAudioAac = 3, kAudioHeAac = 4, kAudioAacEld = 5, kAudioVorbis = 6,
              kAudioOpus = 7;
constexpr int kVideoDefault = 0, kVideoH264 = 2;
constexpr int kVideoSourceCamera = 1, kVideoSourceSurface = 2;
constexpr int kInfoMaxDuration = 800, kInfoMaxFileSize = 801;
constexpr int kEventInfo = 2;

}  // namespace

Recorder::~Recorder() { Reset(); }

int64_t Recorder::NowUs() const {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool Recorder::Prepare(std::string* error) {
  RecorderConfig& c = config_;
  if (c.fd < 0) {
    *error = "no output file";
    return false;
  }
  if (c.audio_source < 0 && c.video_source < 0) {
    *error = "no audio or video source";
    return false;
  }
  int out = c.output_format;
  if (out == kOutDefault || out < 0) out = kOut3gpp;
  MuxerFormat mf;
  switch (out) {
    case kOut3gpp: mf = MuxerFormat::kThreeGpp; break;
    case kOutMpeg4: mf = MuxerFormat::kMpeg4; break;
    case kOutAdts: mf = MuxerFormat::kAdts; break;
    case kOutWebm: mf = MuxerFormat::kWebm; break;
    case kOutOgg: mf = MuxerFormat::kOgg; break;
    default:
      *error = "output format " + std::to_string(c.output_format) + " is not supported on Gesher";
      return false;
  }
  const bool xiph = mf == MuxerFormat::kWebm || mf == MuxerFormat::kOgg;
  Format audio_format, video_format;
  if (c.audio_source >= 0) {
    switch (c.audio_encoder) {
      case kAudioAac:
      case kAudioHeAac:
      case kAudioAacEld: audio_mime_ = "audio/mp4a-latm"; break;
      case kAudioOpus:
      case kAudioVorbis: audio_mime_ = "audio/opus"; break;
      default:
        // DEFAULT, AMR_NB, AMR_WB: no AMR encoder here; the container's own codec.
        audio_mime_ = xiph ? "audio/opus" : "audio/mp4a-latm";
        if (c.audio_encoder != kAudioDefault) {
          MEDIA_LOGW("MediaRecorder: audio encoder %d (AMR) is not available; %s instead", c.audio_encoder,
                     audio_mime_.c_str());
        }
    }
    if (c.audio_encoder == kAudioVorbis) MEDIA_LOGW("MediaRecorder: no Vorbis encoder; Opus instead");
    if (xiph && audio_mime_ != "audio/opus") {
      *error = "AAC does not go into WebM or Ogg";
      return false;
    }
    if (mf == MuxerFormat::kAdts && audio_mime_ != "audio/mp4a-latm") {
      *error = "AAC_ADTS holds AAC only";
      return false;
    }
    audio_params_.channels = static_cast<uint32_t>(std::clamp(c.channels, 1, 2));
    audio_params_.bitrate = c.audio_bitrate > 0 ? c.audio_bitrate : 128000;
    uint32_t rate = c.sample_rate > 0 ? static_cast<uint32_t>(c.sample_rate) : 0;
    if (audio_mime_ == "audio/opus") {
      if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000) rate = 48000;
    } else if (rate != 44100 && rate != 48000) {
      if (rate != 0) MEDIA_LOGW("MediaRecorder: AAC at %u Hz is not available; 44100 Hz", rate);
      rate = 44100;
    }
    audio_params_.sample_rate = rate;
    audio_enc_ = AudioEncoder::Create(audio_mime_, audio_params_, error);
    if (audio_enc_ == nullptr) return false;
    CaptureParams p;
    p.sample_rate = rate;
    p.channels = audio_params_.channels;
    p.format = PcmFormat::kS16;
    p.capacity_frames = rate;  // a second
    p.source = c.audio_source;
    p.name = "MediaRecorder";
    capture_ = OpenCapture(p, error);
    if (capture_ == nullptr) return false;
    AMediaFormat_setString(audio_format.get(), "mime", audio_mime_.c_str());
    AMediaFormat_setInt32(audio_format.get(), "sample-rate", static_cast<int32_t>(rate));
    AMediaFormat_setInt32(audio_format.get(), "channel-count", static_cast<int32_t>(audio_params_.channels));
  }
  if (c.video_source >= 0) {
    if (c.video_source == kVideoSourceCamera) {
      *error = "VIDEO_SOURCE_CAMERA (android.hardware.Camera's recording) is not supported on Gesher: "
               "record VIDEO_SOURCE_SURFACE, from a camera session targeting getSurface()";
      return false;
    }
    if (c.video_source != kVideoSourceSurface) {
      *error = "video source " + std::to_string(c.video_source) + " is not supported";
      return false;
    }
    if (c.video_encoder != kVideoDefault && c.video_encoder != kVideoH264) {
      *error = "video encoder " + std::to_string(c.video_encoder) + " is not available on Gesher (H.264 is)";
      return false;
    }
    if (mf != MuxerFormat::kMpeg4 && mf != MuxerFormat::kThreeGpp) {
      *error = "H.264 goes into MPEG_4 or THREE_GPP";
      return false;
    }
    video_mime_ = "video/avc";
    video_params_.width = c.width > 0 ? c.width : 320;
    video_params_.height = c.height > 0 ? c.height : 240;
    video_params_.frame_rate = c.frame_rate > 0 ? c.frame_rate : 30;
    video_params_.bitrate = c.video_bitrate > 0 ? c.video_bitrate : 2000000;
    video_params_.i_frame_interval_s = 1.0f;
    video_enc_ = VideoEncoder::Create(video_mime_, video_params_, error);
    if (video_enc_ == nullptr) return false;
    input_surface_.reset(CreateInputSurface(video_params_.width, video_params_.height));
    input_surface_->SetCallback([this](const uint32_t* rgba, int w, int h, int64_t ts_ns) {
      OnFrame(rgba, w, h, ts_ns);
    });
    AMediaFormat_setString(video_format.get(), "mime", video_mime_.c_str());
    AMediaFormat_setInt32(video_format.get(), "width", video_params_.width);
    AMediaFormat_setInt32(video_format.get(), "height", video_params_.height);
  }
  muxer_ = Muxer::Create(c.fd, mf, error);
  if (muxer_ == nullptr) return false;
  muxer_->SetOrientationHint(c.orientation);
  if (video_enc_ != nullptr) {
    video_track_ = muxer_->AddTrack(video_format, error);
    if (video_track_ < 0) return false;
  }
  if (audio_enc_ != nullptr) {
    audio_track_ = muxer_->AddTrack(audio_format, error);
    if (audio_track_ < 0) return false;
  }
  prepared_ = true;
  MEDIA_LOGI("MediaRecorder: prepared: output %d, audio %s%s, video %s %dx%d", out,
             audio_mime_.empty() ? "none" : audio_mime_.c_str(),
             audio_enc_ != nullptr ? (" " + std::to_string(audio_params_.sample_rate) + " Hz").c_str() : "",
             video_mime_.empty() ? "none" : video_mime_.c_str(), video_params_.width, video_params_.height);
  return true;
}

bool Recorder::Start(std::string* error) {
  if (!prepared_ || started_) {
    *error = "start() in the wrong state";
    return false;
  }
  if (!muxer_->Start(error)) return false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = true;
    start_us_ = NowUs();
  }
  started_ = true;
  if (capture_ != nullptr) {
    capture_->Start();
    audio_thread_ = std::thread([this] { AudioLoop(); });
  }
  if (video_enc_ != nullptr) video_thread_ = std::thread([this] { VideoLoop(); });
  return true;
}

void Recorder::Pause() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_ || paused_) return;
  paused_ = true;
  pause_at_us_ = NowUs();
}

void Recorder::Resume() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!running_ || !paused_) return;
  paused_ = false;
  paused_total_us_ += NowUs() - pause_at_us_;
}

int Recorder::MaxAmplitude() { return amplitude_.exchange(0); }

void Recorder::Finish(int info) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (finished_) return;
    finished_ = true;
  }
  MEDIA_LOGI("MediaRecorder: %s reached", info == kInfoMaxDuration ? "the maximum duration" : "the maximum size");
  if (listener_) listener_(kEventInfo, info);
}

bool Recorder::Write(int track, const std::vector<EncodedPacket>& packets, int64_t* last_us) {
  std::lock_guard<std::mutex> lock(mux_mu_);
  std::string error;
  for (const EncodedPacket& p : packets) {
    int64_t t = p.time_us;
    if (!p.config && last_us != nullptr) {
      t = std::max(t, *last_us + 1);
      *last_us = t;
    }
    const int flags = (p.config ? 2 : 0) | (p.key && !p.config ? 1 : 0);
    if (!muxer_->WriteSample(track, p.data.data(), p.data.size(), t, flags, &error)) {
      MEDIA_LOGE("MediaRecorder: %s", error.c_str());
      return false;
    }
    if (!p.config) {
      samples_written_++;
      if (track == video_track_) ++video_muxed_packets_;
    }
  }
  if (config_.max_file_size > 0 && muxer_->BytesWritten() >= config_.max_file_size) Finish(kInfoMaxFileSize);
  return true;
}

void Recorder::AudioLoop() {
  RaiseAudioThreadPriority(false);
  const uint32_t rate = audio_params_.sample_rate, ch = audio_params_.channels;
  const size_t chunk = rate / 50;  // 20 ms
  std::vector<int16_t> pcm(chunk * ch);
  uint64_t frames = 0;
  int64_t last_us = -1;
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_) break;
    }
    ssize_t n = capture_->Read(pcm.data(), pcm.size() * sizeof(int16_t), true);
    if (n <= 0) continue;
    const size_t got = static_cast<size_t>(n) / (sizeof(int16_t) * ch);
    bool skip;
    {
      std::lock_guard<std::mutex> lock(mu_);
      skip = paused_ || finished_ || !running_;
    }
    if (skip) continue;
    int peak = 0;
    for (size_t i = 0; i < got * ch; i++) peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    int prev = amplitude_.load();
    while (peak > prev && !amplitude_.compare_exchange_weak(prev, peak)) {
    }
    const int64_t time_us = static_cast<int64_t>(frames * 1000000 / rate);
    std::vector<EncodedPacket> packets;
    if (!audio_enc_->Encode(pcm.data(), got, time_us, &packets)) {
      MEDIA_LOGE("MediaRecorder: the audio encoder failed");
      if (listener_) listener_(1, 1);
      break;
    }
    frames += got;
    Write(audio_track_, packets, &last_us);
    if (config_.max_duration_ms > 0 && static_cast<int64_t>(frames * 1000 / rate) >= config_.max_duration_ms) {
      Finish(kInfoMaxDuration);
    }
  }
  capture_->Stop();
  std::vector<EncodedPacket> packets;
  audio_enc_->Drain(&packets);
  Write(audio_track_, packets, &last_us);
}

void Recorder::OnFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns) {
  const int64_t now = NowUs();
  int64_t ts = ts_ns > 0 ? ts_ns / 1000 : now;
  if (ts > now + 10000000 || ts < now - 10000000) ts = now;  // another clock: the arrival time
  std::lock_guard<std::mutex> lock(mu_);
  ++video_callbacks_;
  if (video_first_source_ns_ < 0) video_first_source_ns_ = ts_ns;
  video_last_source_ns_ = ts_ns;
  if (!running_ || paused_ || finished_) {
    ++video_rejected_;
    return;
  }
  const int64_t pts = ts - start_us_ - paused_total_us_;
  if (pts < 0) {
    ++video_rejected_;
    return;
  }
  ++video_accepted_;
  if (video_first_pts_us_ < 0) video_first_pts_us_ = pts;
  auto f = std::make_shared<VideoFrame>();
  f->Allocate(video_params_.width, video_params_.height);
  RgbaToI420(rgba, w, h, f.get());
  f->time_us = std::max(pts, last_video_us_ + 1);
  last_video_us_ = f->time_us;
  while (frames_.size() >= 8) {
    frames_.pop_front();
    ++video_queue_drops_;
  }
  frames_.push_back(std::move(f));
  cv_.notify_all();
}

void Recorder::VideoLoop() {
  int64_t last_us = -1;
  bool first = true;
  for (;;) {
    std::shared_ptr<VideoFrame> f;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return !running_ || !frames_.empty(); });
      if (frames_.empty()) break;
      f = std::move(frames_.front());
      frames_.pop_front();
    }
    std::vector<EncodedPacket> packets;
    ++video_encode_inputs_;
    video_last_encode_input_us_ = f->time_us;
    if (!video_enc_->Encode(*f, first, &packets)) {
      ++video_encode_failures_;
      MEDIA_LOGE("MediaRecorder: the video encoder failed");
      if (listener_) listener_(1, 1);
      break;
    }
    for (const auto& p : packets) if (!p.config) ++video_encode_packets_;
    first = false;
    Write(video_track_, packets, &last_us);
    if (config_.max_duration_ms > 0 && f->time_us / 1000 >= config_.max_duration_ms) Finish(kInfoMaxDuration);
  }
  std::vector<EncodedPacket> packets;
  video_drain_ok_ = video_enc_->Drain(&packets);
  for (const auto& p : packets) if (!p.config) ++video_drain_packets_;
  Write(video_track_, packets, &last_us);
}

bool Recorder::Stop(std::string* error) {
  if (!started_) {
    *error = "stop() before start()";
    return false;
  }
  size_t pending_at_stop;
  const int64_t stop_requested_us = NowUs();
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    pending_at_stop = frames_.size();
  }
  cv_.notify_all();
  if (capture_ != nullptr) capture_->Stop();
  if (audio_thread_.joinable()) audio_thread_.join();
  if (video_thread_.joinable()) video_thread_.join();
  if (input_surface_ != nullptr) input_surface_->Abandon();
  started_ = false;
  prepared_ = false;
  bool ok;
  {
    std::lock_guard<std::mutex> lock(mux_mu_);
    ok = muxer_->Stop(error);
  }
  MEDIA_LOGI("MediaRecorder: stopped: %lld samples, %lld bytes", static_cast<long long>(samples_written_.load()),
             static_cast<long long>(muxer_->BytesWritten()));
  const char* trace = getenv("GESHER_MEDIA_TRACE_RECORD");
  if (video_enc_ != nullptr && trace != nullptr && trace[0] == '1') {
    std::lock_guard<std::mutex> lock(mu_);
    MEDIA_LOGI("MediaRecorder video trace: backend %s, surface %llu, callbacks %llu, accepted %llu, rejected %llu, "
               "queue drops %llu, pending at stop %zu, encode inputs %llu, encode failures %llu, "
               "encode packets %llu, drained packets %llu (ok %d), muxed packets %llu; source ns %lld..%lld; "
               "first accepted/last encode input us %lld..%lld; start/stop/drained us %lld..%lld..%lld",
               video_enc_->Name().c_str(), static_cast<unsigned long long>(input_surface_->frames()),
               static_cast<unsigned long long>(video_callbacks_), static_cast<unsigned long long>(video_accepted_),
               static_cast<unsigned long long>(video_rejected_), static_cast<unsigned long long>(video_queue_drops_),
               pending_at_stop, static_cast<unsigned long long>(video_encode_inputs_),
               static_cast<unsigned long long>(video_encode_failures_), static_cast<unsigned long long>(video_encode_packets_),
               static_cast<unsigned long long>(video_drain_packets_), static_cast<int>(video_drain_ok_),
               static_cast<unsigned long long>(video_muxed_packets_),
               static_cast<long long>(video_first_source_ns_), static_cast<long long>(video_last_source_ns_),
               static_cast<long long>(video_first_pts_us_), static_cast<long long>(video_last_encode_input_us_),
               static_cast<long long>(start_us_), static_cast<long long>(stop_requested_us), static_cast<long long>(NowUs()));
  }
  if (ok && samples_written_ == 0) {
    *error = "stop failed: nothing was recorded";
    return false;
  }
  return ok;
}

void Recorder::Reset() {
  if (started_) {
    std::string ignored;
    Stop(&ignored);
  }
  if (capture_ != nullptr) capture_->Release();
  capture_.reset();
  if (input_surface_ != nullptr) input_surface_->Abandon();
  input_surface_.reset();
  audio_enc_.reset();
  video_enc_.reset();
  muxer_.reset();
  if (config_.fd >= 0) close(config_.fd);
  config_ = RecorderConfig();
  prepared_ = started_ = false;
  running_ = paused_ = stopping_ = finished_ = false;
  frames_.clear();
  audio_track_ = video_track_ = -1;
  audio_mime_.clear();
  video_mime_.clear();
  samples_written_ = 0;
  paused_total_us_ = 0;
  last_video_us_ = -1;
  video_callbacks_ = video_accepted_ = video_rejected_ = video_queue_drops_ = 0;
  video_encode_inputs_ = video_encode_failures_ = 0;
  video_encode_packets_ = video_drain_packets_ = video_muxed_packets_ = 0;
  video_drain_ok_ = false;
  video_first_source_ns_ = video_last_source_ns_ = -1;
  video_first_pts_us_ = video_last_encode_input_us_ = -1;
}

}  // namespace gesher::media
