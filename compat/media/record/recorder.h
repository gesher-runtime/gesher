// Gesher media: the recorder under android.media.MediaRecorder (docs/media.md
// "Recording", ADR 0059), in place of libmediaplayerservice's
// StagefrightRecorder: audio from the capture engine (compat/media/audio/
// capture.h), video from an input Surface (compat/media/video/input_surface.h:
// what MediaRecorder.getSurface gives, which an app draws into or a camera
// session targets), each through an encoder (compat/media/encode) into a
// container writer (compat/media/mux), on threads of its own.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../audio/capture.h"
#include "../encode/encoder.h"
#include "../mux/muxer.h"
#include "../video/input_surface.h"

namespace gesher::media {

struct RecorderConfig {
  int audio_source = -1;   // MediaRecorder.AudioSource, -1: none
  int video_source = -1;   // MediaRecorder.VideoSource (2: SURFACE), -1: none
  int output_format = -1;  // MediaRecorder.OutputFormat
  int audio_encoder = -1, video_encoder = -1;  // -1: setter not called; 0: explicit DEFAULT
  int width = 0, height = 0, frame_rate = 30;
  int video_bitrate = 2000000;
  int audio_bitrate = 12200, sample_rate = 8000, channels = 1;
  int64_t max_duration_ms = 0, max_file_size = 0;
  int orientation = 0;
  int fd = -1;  // the output (duplicated when set)
};

class Recorder {
 public:
  // Workers report MEDIA_RECORDER_EVENT_* codes; an approaching-limit post
  // precedes terminal completion. JNI posts to the app's Handler, which may
  // then call stop or release.
  using Listener = std::function<void(int what, int extra)>;

  explicit Recorder(Listener listener) : listener_(std::move(listener)) {}
  ~Recorder();

  RecorderConfig& config() { return config_; }
  bool CanConfigure() const { return !prepared_ && !started_ && !error_state_; }
  bool IsIdle() const {
    return CanConfigure() && config_.audio_source < 0 && config_.video_source < 0;
  }
  bool CanSetSource(bool audio) const {
    return CanConfigure() && config_.output_format < 0 &&
        (audio ? config_.audio_source : config_.video_source) < 0;
  }
  bool CanSetOutputFormat() const {
    return CanConfigure() && config_.output_format < 0 &&
        (config_.audio_source >= 0 || config_.video_source >= 0);
  }
  bool IsConfigured() const { return CanConfigure() && config_.output_format >= 0; }
  bool CanSetEncoder(bool audio) const {
    return IsConfigured() && (audio ? config_.audio_source : config_.video_source) >= 0 &&
        (audio ? config_.audio_encoder : config_.video_encoder) < 0;
  }
  bool HasStarted() const { return started_ && !error_state_; }
  bool HasAudioInput();
  bool InError() const { return error_state_; }
  void EnterError() { error_state_ = true; }
  bool Prepare(std::string* error, bool* invalid_state = nullptr);
  // The input Surface (video source SURFACE), after Prepare.
  InputSurface* input_surface();
  bool Start(std::string* error, bool* invalid_state = nullptr);
  // False (and `error`) when nothing was recorded.
  bool Stop(std::string* error, bool* invalid_state = nullptr);
  bool Pause(std::string* error = nullptr, bool* invalid_state = nullptr);
  bool Resume(std::string* error = nullptr, bool* invalid_state = nullptr);
  // The largest sample since the last call (0..32767).
  int MaxAmplitude();
  void Reset();

 private:
  enum class StopReason { kNone, kManual, kDuration, kSize, kError };

  bool BuildPipeline(std::string* error);
  void ReleasePipeline();
  void Clear();
  void RequestStop(StopReason reason, std::string error = {});
  bool ShouldDrain();
  void CompleteWorker();
  void AudioLoop();
  void VideoLoop();
  void OnFrame(const uint32_t* rgba, int w, int h, int64_t ts_ns);
  // Writes packets of a track (the muxer's lock taken here).
  bool Write(int track, const std::vector<EncodedPacket>& packets, int64_t* last_us);
  int64_t NowUs() const;

  Listener listener_;
  RecorderConfig config_;
  std::unique_ptr<Muxer> muxer_;
  std::mutex mux_mu_;
  bool size_reached_ = false;  // protected by mux_mu_ while recording
  bool size_approaching_ = false;
  int audio_track_ = -1, video_track_ = -1;
  std::string audio_mime_, video_mime_;
  std::shared_ptr<CaptureStream> capture_;
  std::unique_ptr<AudioEncoder> audio_enc_;
  AudioEncoderParams audio_params_;
  std::unique_ptr<VideoEncoder> video_enc_;
  VideoEncoderParams video_params_;
  std::unique_ptr<InputSurface> input_surface_;

  std::thread audio_thread_, video_thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<VideoFrame>> frames_;
  bool running_ = false, paused_ = false;
  unsigned audio_epoch_ = 0;
  unsigned active_workers_ = 0;
  StopReason stop_reason_ = StopReason::kNone;
  std::string stop_error_;
  bool prepared_ = false, started_ = false;
  // Public API state, serialized by the JNI receiver lease. Worker cleanup
  // does not change it: a backend operation failure requires explicit reset.
  bool error_state_ = false;
  int64_t start_us_ = 0;        // the recording's time 0 (CLOCK_MONOTONIC)
  int64_t paused_total_us_ = 0;  // time spent paused
  int64_t pause_at_us_ = 0, resume_at_us_ = 0;
  std::atomic<int> amplitude_{0};
  std::atomic<int64_t> samples_written_{0};
  int64_t last_video_us_ = -1;
  // Diagnostic counters: callback-side fields use mu_; encoder-side fields
  // are read only after the video thread joins. They do not alter collection.
  int64_t stop_requested_us_ = 0;
  size_t pending_at_stop_ = 0;
  uint64_t video_callbacks_ = 0, video_accepted_ = 0, video_rejected_ = 0, video_queue_drops_ = 0;
  uint64_t video_encode_inputs_ = 0, video_encode_failures_ = 0;
  uint64_t video_encode_packets_ = 0, video_drain_packets_ = 0, video_muxed_packets_ = 0;
  uint64_t audio_muxed_packets_ = 0;
  bool video_drain_ok_ = false;
  int64_t video_first_source_ns_ = -1, video_last_source_ns_ = -1;
  int64_t video_first_pts_us_ = -1, video_last_encode_input_us_ = -1;
};

}  // namespace gesher::media
