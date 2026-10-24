// Gesher media: AOSP's extractors behind one interface (docs/media.md).
//
// Each extractor is a plugin with a sniffer (cmake/aosp/media_codecs.cmake
// builds them in, their GETEXTRACTORDEF renamed); Open asks every one, as
// MediaExtractorFactory does, and takes the most confident. Formats are the
// extractors' AMediaFormats (the NDK's keys: "mime", "durationUs",
// "sample-rate", "channel-count", "csd-0", ...).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data_source.h"

struct AMediaFormat;

namespace gesher::media {

// An AMediaFormat, owned.
class Format {
 public:
  Format();
  explicit Format(AMediaFormat* f) : f_(f) {}
  ~Format();
  Format(Format&& o) noexcept : f_(o.f_) { o.f_ = nullptr; }
  Format& operator=(Format&& o) noexcept;
  Format(const Format&) = delete;
  Format& operator=(const Format&) = delete;

  AMediaFormat* get() const { return f_; }
  bool GetInt32(const char* key, int32_t* out) const;
  bool GetInt64(const char* key, int64_t* out) const;
  bool GetFloat(const char* key, float* out) const;
  bool GetString(const char* key, std::string* out) const;
  bool GetBuffer(const char* key, std::vector<uint8_t>* out) const;
  std::string Mime() const;
  std::string ToString() const;

 private:
  AMediaFormat* f_;
};

struct Sample {
  std::vector<uint8_t> data;
  int64_t time_us = 0;
  bool sync = false;
  int32_t valid_samples = -1;  // Ogg Vorbis: samples of the page's last packet
};

// AOSP's CMediaTrackReadOptions seek modes.
enum class SeekMode : uint32_t { kPreviousSync = 0, kNextSync = 1, kClosestSync = 2, kClosest = 3 };

class TrackReader {
 public:
  virtual ~TrackReader() = default;
  // The next sample. Returns 0, 1 at the end, negative on error; with
  // seek_us >= 0, the first sample at or around that time (by mode).
  virtual int Read(Sample* out, int64_t seek_us = -1, SeekMode mode = SeekMode::kPreviousSync) = 0;
};

class Extractor {
 public:
  virtual ~Extractor() = default;
  // Sniffs `source` with every extractor; null (with `error`) if none takes it.
  static std::unique_ptr<Extractor> Open(std::shared_ptr<DataSource> source, std::string* error);

  virtual std::string Name() const = 0;
  virtual size_t TrackCount() = 0;
  virtual Format TrackFormat(size_t index) = 0;
  // The container's metadata (title, artist, album, ...).
  virtual Format FileFormat() = 0;
  // A track's samples, from its start. One reader per track at a time.
  virtual std::unique_ptr<TrackReader> OpenTrack(size_t index) = 0;
};

}  // namespace gesher::media
