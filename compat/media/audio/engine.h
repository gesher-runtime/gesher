// Gesher: the audio output of an app process (docs/media.md, ADR 0042).
//
// Every sound an app makes (AudioTrack, MediaPlayer, SoundPool, AAudio,
// OpenSL ES) is a Track of this process's Engine. The engine's thread mixes
// the tracks at the output's rate, one mix per Android stream volume group
// (media, ring, alarm, notification, call), and hands the mixes to the sink:
//
//  - WASAPI, shared mode: one render stream per group in use, each its own
//    Windows audio session ("<app> (Alarm)"), whose volume is the Android
//    stream volume of the group (shared_state.h; the Windows volume mixer
//    changes it back);
//  - a capture file (GESHER_AUDIO_CAPTURE): the groups mixed into one WAV,
//    paced by the clock as a device would be, with the stream volumes
//    applied in software (what the volume mixer would do), for tests on
//    machines without an audio endpoint;
//  - nothing, paced by the clock, when no endpoint opens (or GESHER_AUDIO=null).
//
// Tracks are format-converted (8/16/24/32-bit PCM, float), resampled from any
// rate (windowed sinc), channel-mapped to the output and mixed in float.
// Nothing here knows Java: the JNI layers sit on top (compat/media/jni).
#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gesher::media {

enum class PcmFormat : int { kU8, kS16, kS24Packed, kS32, kFloat };

size_t BytesPerSample(PcmFormat f);

// Android's stream types (AudioSystem.STREAM_*) and their volume groups:
// a stream plays at the volume of its alias (AudioService's
// STREAM_VOLUME_ALIAS_DEFAULT on a phone).
constexpr int kNumStreamTypes = 12;  // STREAM_VOICE_CALL (0) .. STREAM_ASSISTANT (11)
int StreamAlias(int stream);
// The volume groups, by alias: the streams that alias to themselves.
constexpr int kNumGroups = 5;
int GroupOfStream(int stream);       // 0..kNumGroups-1
int StreamOfGroup(int group);        // the alias stream of a group
const char* GroupName(int group);    // "Media", "Ringtone", ...

// Status codes, as Android's (AudioSystem.SUCCESS, ERROR, BAD_VALUE, ...).
constexpr int kOk = 0;
constexpr int kError = -1;
constexpr int kBadValue = -2;
constexpr int kInvalidOperation = -3;
constexpr int kWouldBlock = -7;

struct TrackParams {
  uint32_t sample_rate = 48000;
  uint32_t channels = 2;
  PcmFormat format = PcmFormat::kS16;
  int stream_type = 3;          // STREAM_MUSIC
  size_t capacity_frames = 0;   // the stream's buffer, or the static buffer's size
  bool is_static = false;
  int session_id = 0;
  std::string name;             // for logs
};

// What a track tells its owner; called on the engine's event thread (never
// the mixing thread), in order.
struct TrackEvents {
  std::function<void(uint32_t position)> marker;        // the marker was played
  std::function<void(uint32_t position)> new_position;  // one more update period
  std::function<void()> end;                            // the data ran out (static end, stop drained)
  std::function<void()> underrun;                       // streaming, playing, no data
};

class Engine;

class Track : public std::enable_shared_from_this<Track> {
 public:
  enum class State { kStopped, kPlaying, kStopping, kPaused };

  ~Track();

  const TrackParams& params() const { return params_; }
  size_t frame_size() const { return frame_size_; }

  void SetEvents(TrackEvents events);

  // Playback control, as android::AudioTrack's.
  int Start();
  void Pause();
  // Streaming: plays what was written, then stops (kStopping until then).
  // Static: stops at once and rewinds.
  void Stop();
  // Streaming, stopped or paused: drops what was not played; position 0.
  void Flush();
  State state() const;
  bool stopped() const;  // stopped, or stopping and drained

  // Streaming: copies up to `bytes` (whole frames) into the buffer; with
  // `blocking`, waits for room until everything is written or the track is
  // stopped, paused or released. Returns the bytes written or an error.
  // Static: writes at the start of the static buffer (data loaded so far).
  ssize_t Write(const void* data, size_t bytes, bool blocking);

  // Static tracks.
  int SetLoop(uint32_t start, uint32_t end, int count);  // count -1: forever
  int Reload();                                          // rewind to 0
  int SetPosition(uint32_t frame);                       // stopped or paused only
  size_t static_frames() const;

  // Position: frames played since the start (uint32 wraps, as Android's).
  uint32_t Position() const;
  // Frames presented and the CLOCK_MONOTONIC time of that, for getTimestamp.
  bool Timestamp(int64_t* frames, int64_t* time_ns) const;
  int SetMarker(uint32_t frame);
  uint32_t marker() const;
  int SetUpdatePeriod(uint32_t frames);
  uint32_t update_period() const;

  void SetVolume(float left, float right);
  // The playback rate in Hz (setPlaybackRate) and speed (PlaybackParams);
  // pitch follows the speed (no time stretching).
  int SetSampleRate(uint32_t rate);
  uint32_t sample_rate() const;
  void SetSpeed(float speed);
  float speed() const;

  uint32_t buffer_capacity_frames() const { return static_cast<uint32_t>(params_.capacity_frames); }
  uint32_t buffer_size_frames() const;
  int SetBufferSizeFrames(uint32_t frames);
  uint32_t underrun_frames() const;
  uint32_t LatencyMs() const;

  // Streaming: frames written and not yet played.
  size_t Queued() const;

  // Detaches the track from the engine; blocked writers return.
  void Release();

 private:
  friend class Engine;
  friend std::shared_ptr<Track> CreateTrack(const TrackParams&, std::string*);
  Track(Engine* engine, const TrackParams& params);

  // Mixing thread: adds up to `frames` output frames at `out_rate` with
  // `out_channels` into `mix` (already scaled by the track's volume).
  // Returns false when the track has nothing to play.
  bool MixInto(float* mix, size_t frames, uint32_t out_rate, uint32_t out_channels);
  // Reads up to `frames` source frames as float (the track's channels).
  size_t ReadSource(float* dst, size_t frames, bool* ended);
  void QueueEvents(uint64_t before, uint64_t after);

  Engine* engine_;
  TrackParams params_;
  size_t frame_size_;

  mutable std::mutex mu_;
  std::condition_variable room_;  // signalled as frames are played, and on state changes
  State state_ = State::kStopped;
  bool released_ = false;
  bool drained_ = false;  // kStopping: all played

  // Streaming ring, or static data.
  std::vector<uint8_t> data_;
  uint64_t written_ = 0;      // streaming: frames ever written
  uint64_t read_ = 0;         // streaming: frames ever taken by the resampler
  size_t buffer_frames_ = 0;  // streaming: setBufferSizeInFrames' limit
  size_t static_frames_ = 0;  // static: frames loaded
  size_t static_pos_ = 0;     // static: next frame to take
  uint32_t loop_start_ = 0, loop_end_ = 0;
  int loop_count_ = 0;

  // Played position (source frames), as the resampler passes them.
  uint64_t played_ = 0;
  uint64_t position_base_ = 0;  // subtracted from played_ for Position()
  int64_t played_time_ns_ = 0;
  uint32_t marker_ = 0;
  bool marker_reached_ = false;
  uint32_t update_period_ = 0;
  uint64_t next_update_ = 0;
  uint32_t underrun_frames_ = 0;
  bool was_underrun_ = false;
  bool end_signalled_ = false;

  float vol_left_ = 1.0f, vol_right_ = 1.0f;
  float applied_left_ = 1.0f, applied_right_ = 1.0f;  // ramps toward vol_*
  uint32_t rate_;
  float speed_ = 1.0f;

  TrackEvents events_;
  struct Resampler;
  std::unique_ptr<Resampler> rs_;
};

std::shared_ptr<Track> CreateTrack(const TrackParams& params, std::string* error);

// The engine's output, for AudioSystem's answers.
struct OutputInfo {
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t period_frames;   // the mixer's period
  uint32_t latency_frames;  // what the sink holds
  std::string description;  // "WASAPI shared mode, ...", "capture to ...", "none: ..."
  std::string device_name;  // the endpoint's friendly name
  int device_type;          // AUDIO_DEVICE_OUT_* of the endpoint's form factor
  uint32_t generation;      // changes when the output does
};
OutputInfo GetOutputInfo();

// Runs `fn` on the engine's event thread (after earlier events).
void PostEvent(std::function<void()> fn);

// Streams with a track playing now (or within the last `in_past_ms`), on this
// device (every process; shared_state.h).
bool IsStreamActive(int stream, int in_past_ms);

// The process's application name for the Windows audio sessions
// ("<label> (Media)"); set by the JNI layer when it knows it.
void SetAppName(const std::u16string& name);

}  // namespace gesher::media
