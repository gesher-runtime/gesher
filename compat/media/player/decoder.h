// Gesher media: audio decoders for the player (docs/media.md, ADR 0042):
// AOSP's mp3dec, Tremolo (Vorbis) and libopus, PCM and G.711 as the
// extractors give them, and AAC through Media Foundation.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../audio/engine.h"
#include "extractor.h"

namespace gesher::media {

struct PcmInfo {
  uint32_t rate = 0;
  uint32_t channels = 0;
  PcmFormat format = PcmFormat::kS16;
};

class AudioDecoder {
 public:
  virtual ~AudioDecoder() = default;
  // A decoder for a track's format (its "mime" and codec-specific data); null
  // with `error` when there is none.
  static std::unique_ptr<AudioDecoder> Create(const Format& track, std::string* error);
  // Whether a decoder exists for `mime` on this machine.
  static bool Supports(const std::string& mime);

  virtual PcmInfo info() const = 0;
  // Decodes one sample, appending PCM (info().format, interleaved) to `out`.
  // False on an error the stream cannot go on from.
  virtual bool Decode(const Sample& in, std::vector<uint8_t>* out) = 0;
  // After a seek: drop what is buffered (`time_us` is where reading resumes).
  virtual void Flush(int64_t time_us) {}
  // At the end of the stream: what is still inside.
  virtual void Drain(std::vector<uint8_t>* out) {}
};

// Decodes the first audio track of `source` whole (at most `max_bytes` of
// PCM): SoundPool's samples (decode_file.cc).
bool DecodeFile(std::shared_ptr<DataSource> source, size_t max_bytes, std::vector<uint8_t>* pcm,
                PcmInfo* info, std::string* error);

// Media Foundation's AAC decoder (mf_decoder.cc).
std::unique_ptr<AudioDecoder> CreateMfAacDecoder(const Format& track, std::string* error);
bool MfAacAvailable();

}  // namespace gesher::media
