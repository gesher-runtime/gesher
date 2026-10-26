// Gesher: the audio input of an app process (docs/media.md "Recording", ADR 0059).
//
// Every recorder an app opens (AudioRecord, AAudio and OpenSL ES input,
// MediaRecorder's audio) is a CaptureStream of this process's capture engine.
// The engine's thread takes what the input source gives, every 10 ms, and
// hands it to each started stream, converted to the stream's rate (linear
// interpolation), channels and PCM format. The source is, in order:
//
//  - GESHER_AUDIO_SOURCE=<file.wav>: a WAV file played as the microphone,
//    paced by the clock, from its start each time recording starts from idle,
//    looping (a source a machine without a microphone, CI, can feed);
//  - GESHER_AUDIO_SOURCE=loopback: what the default output endpoint plays
//    (WASAPI loopback);
//  - the default capture endpoint (WASAPI shared mode): the PC's microphone;
//  - silence, paced by the clock, when there is none (as a muted microphone:
//    recorders still open, AudioManager lists no input device).
//
// Nothing here knows Java: the JNI and NDK layers sit on top.
#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine.h"

namespace gesher::media {

struct CaptureParams {
  uint32_t sample_rate = 48000;
  uint32_t channels = 1;
  PcmFormat format = PcmFormat::kS16;
  size_t capacity_frames = 0;  // the stream's buffer (0: half a second)
  int source = 1;              // MediaRecorder.AudioSource (MIC), for logs
  std::string name;            // for logs
};

// What a stream tells its owner; called on the capture thread, in order.
struct CaptureEvents {
  std::function<void(uint32_t position)> marker;
  std::function<void(uint32_t position)> new_position;
  // Data arrived (frames now available), for callback-driven readers.
  std::function<void()> data;
};

class CaptureEngine;

class CaptureStream {
 public:
  ~CaptureStream();

  const CaptureParams& params() const { return params_; }
  size_t frame_size() const { return frame_size_; }

  void SetEvents(CaptureEvents events);

  int Start();
  void Stop();
  bool started() const;

  // Copies up to `bytes` (whole frames) of what was captured; with `blocking`,
  // waits until that much is there or the stream stops. Returns the bytes
  // read (0 when stopped and empty) or an error.
  ssize_t Read(void* dst, size_t bytes, bool blocking);
  // Frames captured and not read yet.
  size_t Available() const;
  // Waits until `frames` are there (or the stream stops), at most timeout_ms.
  bool WaitForData(size_t frames, int timeout_ms);

  // Frames ever read (the record position) and captured.
  uint64_t frames_read() const;
  uint64_t frames_captured() const;
  // The frame captured last and the CLOCK_MONOTONIC time it was captured at.
  bool Timestamp(int64_t* frames, int64_t* time_ns) const;
  uint32_t overrun_frames() const;
  // Cumulative transitions into overrun, distinct from the frames discarded.
  uint32_t overrun_count() const;
  size_t capacity_frames() const { return params_.capacity_frames; }

  int SetMarker(uint32_t frame);
  uint32_t marker() const;
  int SetUpdatePeriod(uint32_t frames);
  uint32_t update_period() const;

  // Detaches the stream from the engine; blocked readers return.
  void Release();

 private:
  friend class CaptureEngine;
  friend std::shared_ptr<CaptureStream> OpenCapture(const CaptureParams&, std::string*);
  CaptureStream(CaptureEngine* engine, const CaptureParams& params);

  // Capture thread: `frames` frames of the source (float, src_channels
  // interleaved, at src_rate) arrive.
  void Push(const float* src, size_t frames, uint32_t src_rate, uint32_t src_channels,
            int64_t now_ns);

  CaptureEngine* engine_;
  CaptureParams params_;
  size_t frame_size_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool started_ = false, released_ = false;
  std::vector<uint8_t> ring_;
  uint64_t written_ = 0, read_ = 0;  // frames
  uint32_t overruns_ = 0;
  uint32_t overrun_count_ = 0;
  bool was_overrun_ = false;
  int64_t last_ns_ = 0;
  // The resampler's state: the fractional source position and the last frame.
  double phase_ = 0;
  std::vector<float> last_;
  bool primed_ = false;
  uint32_t marker_ = 0;
  bool marker_reached_ = false;
  uint32_t update_period_ = 0;
  uint64_t next_update_ = 0;
  CaptureEvents events_;
};

std::shared_ptr<CaptureStream> OpenCapture(const CaptureParams& params, std::string* error);

// The input, for AudioManager and AudioSystem's answers.
struct InputInfo {
  bool device = false;      // a real input device (a microphone, loopback or the file)
  uint32_t sample_rate = 48000;
  uint32_t channels = 1;
  std::string description;  // "WASAPI shared mode capture on ...", "file ...", "none: ..."
  std::string device_name;
  int device_type = 0x80000004;  // AUDIO_DEVICE_IN_BUILTIN_MIC
};
InputInfo GetInputInfo();

// The minimum buffer a recorder needs at a rate (frames): two capture periods.
size_t MinCaptureFrames(uint32_t rate);

// Converts float samples to `format` (interleaved), `count` samples.
void FloatToPcm(const float* src, size_t count, PcmFormat format, void* dst);

// --- Sources (capture_wasapi.cc, capture.cc) -----------------------------------------

class CaptureSource {
 public:
  virtual ~CaptureSource() = default;
  virtual uint32_t rate() const = 0;
  virtual uint32_t channels() const = 0;
  virtual std::string Describe() const = 0;
  virtual std::string DeviceName() const = 0;
  // Starts (or restarts) capturing; false when it cannot.
  virtual bool Start() = 0;
  virtual void Stop() = 0;
  // What arrived since the last call, as float frames, appended to `out`.
  // False when the source is gone.
  virtual bool Pull(std::vector<float>* out) = 0;
};

// WASAPI shared mode on the default capture endpoint, or (loopback) the
// default render endpoint's loopback; null with `error` when there is none.
std::unique_ptr<CaptureSource> OpenWasapiCapture(bool loopback, std::string* error);
// Whether the default capture endpoint exists (without opening it).
bool HasWasapiCaptureEndpoint(std::string* name);
// COM for the calling thread (the multithreaded apartment).
void CaptureComInit();

}  // namespace gesher::media
