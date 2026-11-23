// Gesher media: the recorder (recorder.h; ADR 0059).

#include "recorder.h"

#include <fcntl.h>
#include <time.h>
#include <gesher/clocks.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <media/NdkMediaFormat.h>

#include "../audio/log.h"

namespace gesher::media {

namespace {

// MediaRecorder's constants.
constexpr int kOutDefault = 0, kOut3gpp = 1, kOutMpeg4 = 2, kOutAdts = 6, kOutWebm = 9, kOutOgg = 11;
constexpr int kAudioDefault = 0, kAudioAac = 3, kAudioOpus = 7;
constexpr int kVideoDefault = 0, kVideoH264 = 2;
constexpr int kVideoSourceCamera = 1, kVideoSourceSurface = 2;
constexpr int kInfoMaxDuration = 800, kInfoMaxFileSize = 801, kInfoMaxFileSizeApproaching = 802;
constexpr int kInfoNextOutputFileStarted = 803;
constexpr int kEventError = 1, kEventInfo = 2, kErrorUnknown = 1;

}  // namespace

Recorder::~Recorder() { Reset(); }

int64_t Recorder::NowUs() const {
  timespec ts;
  gesher::AndroidClockGettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}

bool Recorder::Prepare(std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  if (!IsConfigured() ||
      (config_.audio_source >= 0) != (config_.audio_encoder >= 0) ||
      (config_.video_source >= 0) != (config_.video_encoder >= 0)) {
    if (invalid_state != nullptr) *invalid_state = true;
    *error = "prepare() in the wrong state";
    return false;
  }
  if (config_.fd < 0) {
    // Stagefright's prepareInternal returns INVALID_OPERATION for an unset
    // output. The native MediaRecorder wrapper still enters ERROR for this
    // backend failure, unlike its nonfatal configuration-order checks above.
    if (invalid_state != nullptr) *invalid_state = true;
    error_state_ = true;
    *error = "prepare() without an output file";
    return false;
  }
  if (!BuildPipeline(error)) {
    ReleasePipeline();
    error_state_ = true;
    return false;
  }
  // The muxer owns its own duplicate. In particular, a limit-triggered
  // stop must close the output even if the caller never invokes stop().
  close(config_.fd);
  config_.fd = -1;
  prepared_ = true;
  return true;
}

bool Recorder::SetNextOutputFile(int fd, std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  // The native MediaRecorder checks writable access before Stagefright checks
  // its output format/writer state. Failure here does not put it into ERROR.
  const int flags = fcntl(fd, F_GETFL);
  if (flags >= 0 && (flags & (O_RDWR | O_WRONLY)) == 0) {
    *error = "next output file is not a writable descriptor";
    return false;
  }
  std::lock_guard<std::mutex> lock(mux_mu_);
  // A writer completed by its size/duration limit stays the recorder's MP4
  // writer until stop/reset (AOSP clears mWriter only there, and
  // MPEG4Writer::setNextFd has no started check). Admission therefore does
  // not depend on muxer_: a late descriptor is retained unused and closed by
  // ReleasePipeline on stop/reset.
  if (config_.output_format != kOutMpeg4 || !prepared_ || error_state_) {
    if (invalid_state != nullptr) *invalid_state = true;
    *error = "setNextOutputFile requires a prepared MPEG_4 recorder";
    return false;
  }
  // An invalid descriptor cannot be transported to Android's media service.
  // Preserve the documented pre-prepare/unsupported-format state error; for
  // a prepared MP4 recorder report the bad descriptor as a nonfatal I/O error.
  if (flags < 0) {
    *error = "next output file is not a writable descriptor";
    return false;
  }
  if (lseek(fd, 0, SEEK_CUR) < 0 || ftruncate(fd, 0) < 0) {
    *error = "next output file must be seekable and truncatable";
    return false;
  }
  // Stagefright truncates before MPEG4Writer rejects an already pending fd.
  // A second call cannot replace the previously chosen pending output.
  if (next_fd_ >= 0) {
    if (invalid_state != nullptr) *invalid_state = true;
    *error = "a next output file is already pending";
    return false;
  }
  next_fd_ = dup(fd);
  if (next_fd_ < 0) {
    *error = std::string("cannot duplicate next output file: ") + strerror(errno);
    return false;
  }
  next_file_cv_.notify_one();
  return true;
}

InputSurface* Recorder::input_surface() {
  std::lock_guard<std::mutex> lock(mu_);
  // Writer completion abandons the producer but leaves the public recorder
  // prepared/recording until stop or reset. AOSP retains the queryable Surface
  // through that interval; returning it must not restart capture or encoding.
  return prepared_ ? input_surface_.get() : nullptr;
}

bool Recorder::BuildPipeline(std::string* error) {
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
  if (out == kOutDefault) out = kOut3gpp;
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
      case kAudioAac: audio_mime_ = "audio/mp4a-latm"; break;
      case kAudioOpus: audio_mime_ = "audio/opus"; break;
      default:
        // AOSP DEFAULT selects AMR-NB. An absent AMR/Vorbis encoder or an
        // unimplemented HE/ELD profile cannot be replaced by AAC-LC/Opus.
        *error = c.audio_encoder == kAudioDefault ? "default audio encoder AMR-NB is unavailable" :
            "requested audio encoder/profile " + std::to_string(c.audio_encoder) + " is unavailable";
        return false;
    }
    if (xiph && audio_mime_ != "audio/opus") {
      *error = "AAC does not go into WebM or Ogg";
      return false;
    }
    if (mf == MuxerFormat::kAdts && audio_mime_ != "audio/mp4a-latm") {
      *error = "AAC_ADTS holds AAC only";
      return false;
    }
    audio_params_.channels = static_cast<uint32_t>(std::clamp(c.channels, 1, 2));
    audio_params_.bitrate = std::clamp(c.audio_bitrate,
        audio_mime_ == "audio/opus" ? 6000 : 96000,
        audio_mime_ == "audio/opus" ? 510000 : 192000);
    const int requested_rate = c.sample_rate > 0 ? c.sample_rate : 8000;
    uint32_t rate;
    if (audio_mime_ == "audio/opus") {
      rate = static_cast<uint32_t>(std::clamp(requested_rate, 8000, 48000));
      if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) {
        *error = "unsupported Opus sample rate " + std::to_string(rate);
        return false;
      }
    } else {
      rate = static_cast<uint32_t>(std::clamp(requested_rate, 44100, 48000));
      if (rate != 44100 && rate != 48000) {
        *error = "unsupported AAC sample rate " + std::to_string(rate);
        return false;
      }
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
    if (input_surface_ == nullptr) {
      *error = "cannot create the recording Surface";
      return false;
    }
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
  if (!muxer_->SetFileSizeLimit(c.max_file_size)) {
    *error = "file-size limiting is unavailable for this container writer";
    return false;
  }
  MEDIA_LOGI("MediaRecorder: prepared: output %d, audio %s%s, video %s %dx%d", out,
             audio_mime_.empty() ? "none" : audio_mime_.c_str(),
             audio_enc_ != nullptr ? (" " + std::to_string(audio_params_.sample_rate) + " Hz").c_str() : "",
             video_mime_.empty() ? "none" : video_mime_.c_str(), video_params_.width, video_params_.height);
  return true;
}

bool Recorder::Start(std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  if (!prepared_ || started_ || error_state_) {
    if (invalid_state != nullptr) *invalid_state = true;
    *error = "start() in the wrong state";
    return false;
  }
  const bool has_audio = capture_ != nullptr;
  const bool has_video = video_enc_ != nullptr;
  if (has_audio && capture_->Start() != kOk) {
    *error = "audio capture is unavailable";
    ReleasePipeline();
    error_state_ = true;
    return false;
  }
  if (!muxer_->Start(error)) {
    ReleasePipeline();
    error_state_ = true;
    return false;
  }
  const int64_t start_us = NowUs();
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = true;
    start_us_ = start_us;
    active_workers_ = static_cast<unsigned>(has_audio) + static_cast<unsigned>(has_video);
  }
  started_ = true;
  if (has_audio) audio_thread_ = std::thread([this] { AudioLoop(); });
  if (has_video) video_thread_ = std::thread([this] { VideoLoop(); });
  return true;
}

bool Recorder::Pause(std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!HasStarted()) {
    if (invalid_state != nullptr) *invalid_state = true;
    if (error != nullptr) *error = "pause() outside recording";
    return false;
  }
  // AOSP's public native state stays RECORDING after a writer limit. Its
  // pause/resume are still accepted until stop/reset, and must not restart
  // the completed session or mutate the finalized output.
  if (!running_) return true;
  if (paused_) return true;
  paused_ = true;
  pause_at_us_ = NowUs();
  ++audio_epoch_;
  if (capture_ != nullptr) capture_->Stop();
  return true;
}

bool Recorder::Resume(std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  bool restart_failed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!HasStarted()) {
      if (invalid_state != nullptr) *invalid_state = true;
      if (error != nullptr) *error = "resume() outside recording";
      return false;
    }
    if (!running_) return true;
    if (!paused_) return true;
    restart_failed = capture_ != nullptr && capture_->Start() != kOk;
    if (restart_failed) {
      error_state_ = true;
      const std::string message = "audio capture restart failed";
      if (error != nullptr) *error = message;
      // Finalization can read the terminal reason as soon as both workers
      // have exited. Publish this failure before either can reach it.
      RequestStopLocked(StopReason::kError, message);
    } else {
      resume_at_us_ = NowUs();
      paused_total_us_ += resume_at_us_ - pause_at_us_;
      paused_ = false;
    }
  }
  if (restart_failed) {
    WakeGrace();
    if (audio_thread_.joinable()) audio_thread_.join();
    if (video_thread_.joinable()) video_thread_.join();
    return false;
  }
  cv_.notify_all();
  return true;
}

int Recorder::MaxAmplitude() { return amplitude_.exchange(0); }

bool Recorder::HasAudioInput() {
  std::lock_guard<std::mutex> lock(mu_);
  return running_ && !paused_ && capture_ != nullptr && capture_->started();
}

void Recorder::RequestStop(StopReason reason, std::string error) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    RequestStopLocked(reason, std::move(error));
  }
  WakeGrace();
}

void Recorder::RequestStopUnderMux(StopReason reason, std::string error) {
  // The caller holds mux_mu_, so a writer in the rollover grace wait is
  // blocked in it (mux_mu_ released) and RequestStopLocked's notify reaches it.
  std::lock_guard<std::mutex> lock(mu_);
  RequestStopLocked(reason, std::move(error));
}

void Recorder::WakeGrace() {
  // The grace predicate reads stop_reason_ while holding mux_mu_. Passing
  // through mux_mu_ after publishing the stop orders this notify after any
  // predicate check that missed it, so the waiter cannot sleep the full
  // grace. Lock order is mux_mu_ then mu_; no other recorder lock is held.
  { std::lock_guard<std::mutex> lock(mux_mu_); }
  next_file_cv_.notify_all();
}

void Recorder::RequestStopLocked(StopReason reason, std::string error) {
  if (stop_reason_ == StopReason::kError ||
      (stop_reason_ != StopReason::kNone && reason != StopReason::kError)) return;
  stop_reason_ = reason;
  stop_error_ = std::move(error);
  if (running_) {
    stop_requested_us_ = NowUs();
    pending_at_stop_ = frames_.size();
  }
  running_ = false;
  if (reason == StopReason::kSize || reason == StopReason::kError) frames_.clear();
  if (capture_ != nullptr) capture_->Stop();
  cv_.notify_all();
  // Callers holding mux_mu_ (Write) make this notify reliable. Others hold
  // only mu_, which follows mux_mu_; RequestStop/WakeGrace repeat the wake
  // after taking mux_mu_ once mu_ is released.
  next_file_cv_.notify_all();
}

bool Recorder::Write(int track, const std::vector<EncodedPacket>& packets, int64_t* last_us) {
  // Only an MP4 size boundary can start another file. Keep ordinary unlimited
  // and other-container recording on its existing single writer-lock path.
  std::unique_lock<std::mutex> event_lock(writer_event_mu_, std::defer_lock);
  if (config_.output_format == kOutMpeg4 && config_.max_file_size > 0) event_lock.lock();
  std::unique_lock<std::mutex> lock(mux_mu_);
  auto notify = [&](int info) {
    lock.unlock();
    // JNI posts to Java's Handler. The app can supply the next descriptor
    // while this post is in flight; it never needs writer_event_mu_. Keeping
    // that lock orders 802 before 803 across audio and video workers without
    // buffering callbacks or holding the recorder state/writer locks in Java.
    if (listener_) listener_(kEventInfo, info);
    lock.lock();
  };
  for (const EncodedPacket& p : packets) {
    StopReason reason;
    {
      std::lock_guard<std::mutex> state_lock(mu_);
      reason = stop_reason_;
    }
    if (size_reached_ || reason == StopReason::kError) return false;
    int64_t t = p.time_us;
    int64_t duration_origin = segment_first_us_;
    if (!p.config) {
      t = std::max(t, *last_us + 1);
      if (track == video_track_ && awaiting_video_key_ && !p.key) {
        // MPEG4Writer starts each new video track at a real sync sample.
        // Skipped dependent packets do not start its new duration timeline.
        *last_us = t;
        continue;
      }
      if (config_.max_duration_ms > 0) {
        if (duration_origin < 0 || t < duration_origin) duration_origin = t;
        if ((t - duration_origin) / 1000 >= config_.max_duration_ms) {
          RequestStopUnderMux(StopReason::kDuration);
          continue;
        }
      }
      *last_us = t;
      if (track == video_track_) awaiting_video_key_ = false;
    }
    const int flags = (p.config ? 2 : 0) | (p.key && !p.config ? 1 : 0);
    std::string error;
    if (!muxer_->WriteSample(track, p.data.data(), p.data.size(), t, flags, &error)) {
      RequestStopUnderMux(StopReason::kError, std::move(error));
      return false;
    }
    if (muxer_->FileSizeLimitApproaching() && !size_approaching_) {
      size_approaching_ = true;
      notify(kInfoMaxFileSizeApproaching);
    }
    if (muxer_->FileSizeLimitReached()) {
      // Android allows the pending fd's Binder delivery a bounded 100 ms
      // grace. Stop/reset wakes this wait; neither writer nor state mutex
      // is held while waiting, so the app can deliver its selected output.
      if (config_.output_format == kOutMpeg4) {
        next_file_cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
          if (next_fd_ >= 0) return true;
          std::lock_guard<std::mutex> state_lock(mu_);
          return stop_reason_ != StopReason::kNone;
        });
      }
      {
        std::lock_guard<std::mutex> state_lock(mu_);
        reason = stop_reason_;
      }
      if (next_fd_ >= 0 && reason == StopReason::kNone) {
        if ((audio_track_ >= 0 && segment_audio_samples_ == 0) ||
            (video_track_ >= 0 && segment_video_samples_ == 0)) {
          RequestStopUnderMux(StopReason::kError, "rollover failed: a configured track recorded no samples");
          return false;
        }
        const int next = next_fd_;
        next_fd_ = -1;
        const bool switched = muxer_->SwitchFile(next, &error);
        close(next);
        if (!switched) {
          RequestStopUnderMux(StopReason::kError, std::move(error));
          return false;
        }
        size_approaching_ = false;
        // Android resets each track's duration when starting a new file,
        // while the existing encoder/source timestamps continue advancing.
        segment_first_us_ = -1;
        has_rolled_over_ = true;
        segment_audio_samples_ = segment_video_samples_ = 0;
        awaiting_video_key_ = video_track_ >= 0;
        force_video_key_ = awaiting_video_key_;
        notify(kInfoNextOutputFileStarted);
        // Like MPEG4Writer, discard the packet which crossed the old file's
        // limit. Continue reading the same live source/encoder for this file.
        continue;
      }
      size_reached_ = true;
      RequestStopUnderMux(StopReason::kSize);
      return false;
    }
    if (!p.config) {
      if (config_.max_duration_ms > 0) segment_first_us_ = duration_origin;
      samples_written_++;
      if (track == video_track_) {
        ++video_muxed_packets_;
        ++segment_video_samples_;
      }
      if (track == audio_track_) {
        ++audio_muxed_packets_;
        ++segment_audio_samples_;
      }
    }
  }
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
    unsigned epoch;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return !running_ || !paused_; });
      if (!running_) break;
      epoch = audio_epoch_;
    }
    ssize_t n = capture_->Read(pcm.data(), pcm.size() * sizeof(int16_t), true);
    if (n < 0) {
      RequestStop(StopReason::kError, "audio capture read failed");
      break;
    }
    if (n == 0) continue;
    const size_t got = static_cast<size_t>(n) / (sizeof(int16_t) * ch);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!running_ || paused_ || epoch != audio_epoch_) continue;
    }
    int peak = 0;
    for (size_t i = 0; i < got * ch; i++) peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    int prev = amplitude_.load();
    while (peak > prev && !amplitude_.compare_exchange_weak(prev, peak)) {
    }
    const int64_t time_us = static_cast<int64_t>(frames * 1000000 / rate);
    std::vector<EncodedPacket> packets;
    if (!audio_enc_->Encode(pcm.data(), got, time_us, &packets)) {
      RequestStop(StopReason::kError, "audio encoder failed");
      break;
    }
    frames += got;
    if (!Write(audio_track_, packets, &last_us)) break;
    if (config_.max_duration_ms > 0 && !has_rolled_over_ &&
        static_cast<int64_t>(frames * 1000 / rate) >= config_.max_duration_ms) {
      RequestStop(StopReason::kDuration);
    }
  }
  if (ShouldDrain()) {
    std::vector<EncodedPacket> packets;
    if (audio_enc_->Drain(&packets)) {
      Write(audio_track_, packets, &last_us);
    } else {
      RequestStop(StopReason::kError, "audio encoder drain failed");
    }
  }
  CompleteWorker();
}

void Recorder::OnFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns) {
  const int64_t now = NowUs();
  int64_t ts = ts_ns > 0 ? ts_ns / 1000 : now;
  if (ts > now + 10000000 || ts < now - 10000000) ts = now;  // another clock: the arrival time
  std::lock_guard<std::mutex> lock(mu_);
  ++video_callbacks_;
  if (video_first_source_ns_ < 0) video_first_source_ns_ = ts_ns;
  video_last_source_ns_ = ts_ns;
  if (!running_ || paused_) {
    ++video_rejected_;
    return;
  }
  const int64_t pts = ts - start_us_ - paused_total_us_;
  if (pts < 0 || ts < resume_at_us_) {
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
    const bool request_key = force_video_key_.exchange(false);
    if (!video_enc_->Encode(*f, first || request_key, &packets)) {
      ++video_encode_failures_;
      RequestStop(StopReason::kError, "video encoder failed");
      break;
    }
    for (const auto& p : packets) if (!p.config) ++video_encode_packets_;
    first = false;
    if (!Write(video_track_, packets, &last_us)) break;
    if (config_.max_duration_ms > 0 && !has_rolled_over_ && f->time_us / 1000 >= config_.max_duration_ms) {
      RequestStop(StopReason::kDuration);
    }
  }
  if (ShouldDrain()) {
    std::vector<EncodedPacket> packets;
    video_drain_ok_ = video_enc_->Drain(&packets);
    for (const auto& p : packets) if (!p.config) ++video_drain_packets_;
    if (video_drain_ok_) {
      Write(video_track_, packets, &last_us);
    } else {
      RequestStop(StopReason::kError, "video encoder drain failed");
    }
  }
  CompleteWorker();
}

bool Recorder::ShouldDrain() {
  std::lock_guard<std::mutex> lock(mu_);
  return stop_reason_ == StopReason::kManual || stop_reason_ == StopReason::kDuration;
}

void Recorder::CompleteWorker() {
  StopReason reason;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (--active_workers_ != 0) return;
    reason = stop_reason_;
  }
  // The last worker owns finalization. Manual stop waits for it by joining
  // both workers, while a limit can complete without any Java call.
  if (capture_ != nullptr) capture_->Release();
  if (input_surface_ != nullptr) input_surface_->Abandon();
  // A small limit can be exhausted by one track before the other has any
  // media/configuration. Do not call that incomplete recording successful.
  // The worker-count handoff above makes both per-track counters stable.
  if (samples_written_ > 0 && stop_error_.empty() &&
      ((audio_enc_ != nullptr && segment_audio_samples_ == 0) ||
       (video_enc_ != nullptr && segment_video_samples_ == 0))) {
    stop_error_ = "stop failed: a configured track recorded no samples";
    reason = StopReason::kError;
  }
  std::string error;
  {
    // A public setNextOutputFile may race terminal worker cleanup. Retire
    // both descriptors under the same lock used for admission and rollover.
    std::lock_guard<std::mutex> lock(mux_mu_);
    if (!muxer_->Stop(&error) && stop_error_.empty()) {
      stop_error_ = std::move(error);
      reason = StopReason::kError;
    }
    MEDIA_LOGI("MediaRecorder: stopped: %lld samples, %lld bytes", static_cast<long long>(samples_written_.load()),
               static_cast<long long>(muxer_->BytesWritten()));
    muxer_.reset();
    if (next_fd_ >= 0) close(next_fd_);
    next_fd_ = -1;
  }
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
               pending_at_stop_, static_cast<unsigned long long>(video_encode_inputs_),
               static_cast<unsigned long long>(video_encode_failures_), static_cast<unsigned long long>(video_encode_packets_),
               static_cast<unsigned long long>(video_drain_packets_), static_cast<int>(video_drain_ok_),
               static_cast<unsigned long long>(video_muxed_packets_),
               static_cast<long long>(video_first_source_ns_), static_cast<long long>(video_last_source_ns_),
               static_cast<long long>(video_first_pts_us_), static_cast<long long>(video_last_encode_input_us_),
               static_cast<long long>(start_us_), static_cast<long long>(stop_requested_us_), static_cast<long long>(NowUs()));
  }
  audio_enc_.reset();
  video_enc_.reset();
  if (reason == StopReason::kError) {
    MEDIA_LOGE("MediaRecorder: %s", stop_error_.c_str());
    if (listener_) listener_(kEventError, kErrorUnknown);
  } else if (reason == StopReason::kDuration || reason == StopReason::kSize) {
    if (listener_) listener_(kEventInfo, reason == StopReason::kDuration ? kInfoMaxDuration : kInfoMaxFileSize);
  }
}

bool Recorder::Stop(std::string* error, bool* invalid_state) {
  if (invalid_state != nullptr) *invalid_state = false;
  if (!HasStarted()) {
    if (invalid_state != nullptr) *invalid_state = true;
    *error = "stop() before start()";
    return false;
  }
  RequestStop(StopReason::kManual);
  if (audio_thread_.joinable()) audio_thread_.join();
  if (video_thread_.joinable()) video_thread_.join();
  *error = std::move(stop_error_);
  if (error->empty() && samples_written_ == 0) *error = "stop failed: nothing was recorded";
  if (error->empty()) {
    Clear();
    return true;
  }
  ReleasePipeline();
  error_state_ = true;
  return false;
}

void Recorder::Reset() {
  // Public Stop is deliberately unavailable in ERROR. Reset still owns and
  // joins any workers left by that session before returning to IDLE.
  if (started_) RequestStop(StopReason::kManual);
  if (audio_thread_.joinable()) audio_thread_.join();
  if (video_thread_.joinable()) video_thread_.join();
  Clear();
}

void Recorder::ReleasePipeline() {
  if (capture_ != nullptr) capture_->Release();
  capture_.reset();
  if (input_surface_ != nullptr) input_surface_->Abandon();
  input_surface_.reset();
  audio_enc_.reset();
  video_enc_.reset();
  {
    std::lock_guard<std::mutex> lock(mux_mu_);
    muxer_.reset();
    if (next_fd_ >= 0) close(next_fd_);
    next_fd_ = -1;
  }
  audio_track_ = video_track_ = -1;
  audio_mime_.clear();
  video_mime_.clear();
  audio_params_ = {};
  video_params_ = {};
}

void Recorder::Clear() {
  ReleasePipeline();
  if (config_.fd >= 0) close(config_.fd);
  config_ = RecorderConfig();
  prepared_ = started_ = false;
  error_state_ = false;
  running_ = paused_ = false;
  audio_epoch_ = active_workers_ = 0;
  stop_reason_ = StopReason::kNone;
  stop_error_.clear();
  size_reached_ = false;
  size_approaching_ = false;
  awaiting_video_key_ = false;
  segment_first_us_ = 0;
  has_rolled_over_ = false;
  segment_audio_samples_ = segment_video_samples_ = 0;
  force_video_key_ = false;
  frames_.clear();
  amplitude_ = 0;
  samples_written_ = 0;
  start_us_ = paused_total_us_ = pause_at_us_ = resume_at_us_ = stop_requested_us_ = 0;
  pending_at_stop_ = 0;
  last_video_us_ = -1;
  video_callbacks_ = video_accepted_ = video_rejected_ = video_queue_drops_ = 0;
  video_encode_inputs_ = video_encode_failures_ = 0;
  audio_muxed_packets_ = 0;
  video_encode_packets_ = video_drain_packets_ = video_muxed_packets_ = 0;
  video_drain_ok_ = false;
  video_first_source_ns_ = video_last_source_ns_ = -1;
  video_first_pts_us_ = video_last_encode_input_us_ = -1;
}

}  // namespace gesher::media
