// Gesher media: encoders (docs/media.md "Recording", ADR 0059), under
// MediaCodec's encoders and MediaRecorder.
//
//  - H.264: Media Foundation's H.264 encoder MFT where Windows has one,
//    AOSP's libavc encoder otherwise (GESHER_VIDEO_ENCODERS=soft: that one);
//  - AAC (LC): Media Foundation's AAC encoder MFT;
//  - Opus: libopus.
//
// Video comes in as I420 frames (compat/media/video's VideoFrame), audio as
// interleaved 16-bit PCM; what comes out is MediaCodec's output: H.264 as
// Annex B access units (the parameter sets first, on their own, as the
// codec-config buffer), AAC as raw access units (the AudioSpecificConfig as
// the codec-config buffer), Opus as packets (the OpusHead as codec config).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../video/video_decoder.h"

namespace gesher::media {

struct EncodedPacket {
  std::vector<uint8_t> data;
  int64_t time_us = 0;
  bool key = false;     // BUFFER_FLAG_KEY_FRAME
  bool config = false;  // BUFFER_FLAG_CODEC_CONFIG
};

struct EncoderInfo {
  std::string mime;
  std::string backend;  // "mf", "soft"
  bool video = false;
};

// The encoders this machine has, one per MIME type.
const std::vector<EncoderInfo>& AvailableEncoders();

struct VideoEncoderParams {
  int width = 0, height = 0;
  int frame_rate = 30;
  int bitrate = 2000000;
  float i_frame_interval_s = 1.0f;  // KEY_I_FRAME_INTERVAL; <= 0: every frame a key frame
  int profile = 0;                  // AVCProfile*; 0: baseline
};

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;
  static std::unique_ptr<VideoEncoder> Create(const std::string& mime, const VideoEncoderParams& p,
                                              std::string* error);
  virtual std::string Name() const = 0;
  // One frame (its visible part is the picture); `key`: a key frame is asked for.
  virtual bool Encode(const VideoFrame& frame, bool key, std::vector<EncodedPacket>* out) = 0;
  // The end: what is still inside.
  virtual bool Drain(std::vector<EncodedPacket>* out) = 0;
};

struct AudioEncoderParams {
  uint32_t sample_rate = 44100;
  uint32_t channels = 1;
  int bitrate = 128000;
  int aac_profile = 2;  // AACObjectLC
};

class AudioEncoder {
 public:
  virtual ~AudioEncoder() = default;
  static std::unique_ptr<AudioEncoder> Create(const std::string& mime, const AudioEncoderParams& p,
                                              std::string* error);
  virtual std::string Name() const = 0;
  // Interleaved 16-bit PCM starting at `time_us`; packets as they are complete.
  virtual bool Encode(const int16_t* pcm, size_t frames, int64_t time_us,
                      std::vector<EncodedPacket>* out) = 0;
  virtual bool Drain(std::vector<EncodedPacket>* out) = 0;
  // Frames of input a packet takes (AAC 1024, Opus 20 ms).
  virtual size_t frame_size() const = 0;
  // The encoder's delay, in frames at its rate (Opus' pre-skip).
  virtual uint32_t delay() const { return 0; }
};

// Media Foundation (mf_encoder.cc).
bool MfH264EncoderAvailable();
bool MfAacEncoderAvailable();
std::unique_ptr<VideoEncoder> CreateMfH264Encoder(const VideoEncoderParams& p, std::string* error);
std::unique_ptr<AudioEncoder> CreateMfAacEncoder(const AudioEncoderParams& p, std::string* error);
// libavc (soft_avc_encoder.cc), libopus (opus_encoder.cc).
std::unique_ptr<VideoEncoder> CreateSoftAvcEncoder(const VideoEncoderParams& p, std::string* error);
std::unique_ptr<AudioEncoder> CreateOpusEncoder(const AudioEncoderParams& p, std::string* error);

// H.264 Annex B: splits a stream into its NAL units (start codes removed).
std::vector<std::pair<const uint8_t*, size_t>> SplitAnnexB(const uint8_t* p, size_t n);
// An AAC AudioSpecificConfig (2 bytes) for an object type, a rate and channels.
std::vector<uint8_t> AacAudioSpecificConfig(int object_type, uint32_t rate, uint32_t channels);
// An OpusHead (RFC 7845) for channels, a pre-skip and the input rate.
std::vector<uint8_t> OpusHead(uint32_t channels, uint32_t pre_skip, uint32_t rate);

}  // namespace gesher::media
