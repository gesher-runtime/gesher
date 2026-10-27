// Gesher media: container writers (docs/media.md "Recording", ADR 0059), under
// MediaMuxer and MediaRecorder.
//
//  - MPEG-4 (MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4, MediaRecorder's MPEG_4
//    and THREE_GPP): Gesher's own writer (mp4_muxer.cc): ftyp, the samples in
//    one mdat as they come, the moov (sample tables, avcC / esds / dOps) at the
//    end;
//  - WebM (MUXER_OUTPUT_WEBM, MediaRecorder's WEBM): libwebm's mkvmuxer
//    (webm_muxer.cc), VP8/VP9 video, Opus/Vorbis audio;
//  - Ogg (MediaRecorder's OGG): Opus in Ogg pages (ogg_muxer.cc);
//  - AAC ADTS (MediaRecorder's AAC_ADTS): ADTS headers in front of each AAC
//    access unit (ogg_muxer.cc).
//
// Tracks are described by MediaFormat keys (an AMediaFormat): "mime", "width",
// "height", "sample-rate", "channel-count", "csd-0", "csd-1"; samples carry
// MediaCodec's flags (BUFFER_FLAG_KEY_FRAME, BUFFER_FLAG_CODEC_CONFIG: a config
// sample becomes the track's codec-specific data when the format had none).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../player/extractor.h"

namespace gesher::media {

enum class MuxerFormat { kMpeg4, kWebm, kThreeGpp, kOgg, kAdts };

class Muxer {
 public:
  virtual ~Muxer() = default;
  // A writer into `fd` (duplicated; the caller keeps its own); null with `error`.
  static std::unique_ptr<Muxer> Create(int fd, MuxerFormat format, std::string* error);

  // The track's index, or -1 with `error` (a codec the container cannot hold).
  virtual int AddTrack(const Format& format, std::string* error) = 0;
  virtual bool Start(std::string* error) = 0;
  // flags: MediaCodec's BUFFER_FLAG_* (1 key frame, 2 codec config).
  virtual bool WriteSample(int track, const uint8_t* data, size_t size, int64_t time_us, int flags,
                           std::string* error) = 0;
  // Writes what is left (the MPEG-4 moov, the WebM cues); the file is complete after it.
  virtual bool Stop(std::string* error) = 0;
  virtual void SetOrientationHint(int degrees) {}
  virtual void SetLocation(int latitude_e4, int longitude_e4) {}
  virtual int64_t BytesWritten() const = 0;
};

std::unique_ptr<Muxer> CreateMp4Muxer(int fd, bool three_gpp, std::string* error);
std::unique_ptr<Muxer> CreateWebmMuxer(int fd, std::string* error);
std::unique_ptr<Muxer> CreateOggMuxer(int fd, std::string* error);
std::unique_ptr<Muxer> CreateAdtsMuxer(int fd, std::string* error);

// Writes all of `data` to `fd` at its offset; false on an error.
bool WriteAll(int fd, const void* data, size_t size);

}  // namespace gesher::media
