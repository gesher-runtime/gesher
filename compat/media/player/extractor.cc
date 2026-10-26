// Gesher media: AOSP's extractors behind one interface (extractor.h).

#include "extractor.h"

#include <media/MediaExtractorPluginApi.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/MediaBufferBase.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/foundation/AMessage.h>

#include <cstdlib>
#include <cstring>

#include "../audio/log.h"

// The extractors' GETEXTRACTORDEFs, renamed (cmake/aosp/media_codecs.cmake).
extern "C" {
android::ExtractorDef GesherExtractorDef_mp3();
android::ExtractorDef GesherExtractorDef_mp4();
android::ExtractorDef GesherExtractorDef_ogg();
android::ExtractorDef GesherExtractorDef_flac();
android::ExtractorDef GesherExtractorDef_wav();
android::ExtractorDef GesherExtractorDef_aac();
android::ExtractorDef GesherExtractorDef_mkv();
}

namespace gesher::media {

// --- Format ------------------------------------------------------------------------------

Format::Format() : f_(AMediaFormat_new()) {}
Format::~Format() {
  if (f_ != nullptr) AMediaFormat_delete(f_);
}
Format& Format::operator=(Format&& o) noexcept {
  if (this != &o) {
    if (f_ != nullptr) AMediaFormat_delete(f_);
    f_ = o.f_;
    o.f_ = nullptr;
  }
  return *this;
}
bool Format::GetInt32(const char* key, int32_t* out) const {
  return f_ != nullptr && AMediaFormat_getInt32(f_, key, out);
}
bool Format::GetInt64(const char* key, int64_t* out) const {
  return f_ != nullptr && AMediaFormat_getInt64(f_, key, out);
}
bool Format::GetFloat(const char* key, float* out) const {
  return f_ != nullptr && AMediaFormat_getFloat(f_, key, out);
}
bool Format::GetString(const char* key, std::string* out) const {
  const char* s = nullptr;
  if (f_ == nullptr || !AMediaFormat_getString(f_, key, &s) || s == nullptr) return false;
  *out = s;
  return true;
}
bool Format::GetBuffer(const char* key, std::vector<uint8_t>* out) const {
  void* data = nullptr;
  size_t size = 0;
  if (f_ == nullptr || !AMediaFormat_getBuffer(f_, key, &data, &size)) return false;
  out->assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  return true;
}
std::string Format::Mime() const {
  std::string m;
  GetString("mime", &m);
  return m;
}
std::string Format::ToString() const {
  if (f_ == nullptr) return "{}";
  const char* s = AMediaFormat_toString(f_);
  return s != nullptr ? s : "{}";
}

namespace {

using android::CDataSource;
using android::CMediaBuffer;
using android::CMediaExtractor;
using android::CMediaTrack;
using android::ExtractorDef;

struct Plugin {
  const char* name;
  ExtractorDef (*def)();
};

const Plugin kPlugins[] = {
    {"mp3", GesherExtractorDef_mp3}, {"mp4", GesherExtractorDef_mp4},
    {"ogg", GesherExtractorDef_ogg}, {"flac", GesherExtractorDef_flac},
    {"wav", GesherExtractorDef_wav}, {"aac", GesherExtractorDef_aac},
    {"mkv", GesherExtractorDef_mkv},
};

// The data source, as the plugins read it.
struct SourceAdapter {
  explicit SourceAdapter(std::shared_ptr<DataSource> s) : source(std::move(s)) {
    c.handle = this;
    c.readAt = [](void* h, off64_t offset, void* data, size_t size) -> ssize_t {
      return static_cast<SourceAdapter*>(h)->source->ReadAt(offset, data, size);
    };
    c.getSize = [](void* h, off64_t* size) -> android::status_t {
      int64_t s = static_cast<SourceAdapter*>(h)->source->Size();
      if (s < 0) return android::ERROR_UNSUPPORTED;
      *size = s;
      return android::OK;
    };
    c.flags = [](void*) -> uint32_t { return 0; };
    c.getUri = [](void* h, char* uri, size_t size) -> bool {
      std::string u = static_cast<SourceAdapter*>(h)->source->Uri();
      if (u.empty() || size == 0) return false;
      strncpy(uri, u.c_str(), size - 1);
      uri[size - 1] = '\0';
      return true;
    };
  }
  std::shared_ptr<DataSource> source;
  CDataSource c;
};

class AospTrackReader : public TrackReader {
 public:
  AospTrackReader(CMediaTrack* track) : track_(track) {
    started_ = track_->start(track_->data, group_.wrap()) == AMEDIA_OK;
  }
  ~AospTrackReader() override {
    if (started_) track_->stop(track_->data);
    track_->free(track_->data);
    free(track_);  // malloc'd by the plugin helper's wrap()
  }

  int Read(Sample* out, int64_t seek_us, SeekMode mode) override {
    if (!started_) return -1;
    uint32_t opts = 0;
    if (seek_us >= 0) opts = android::CMediaTrackReadOptions::SEEK | static_cast<uint32_t>(mode);
    CMediaBuffer* buf = nullptr;
    media_status_t err = track_->read(track_->data, &buf, opts, seek_us >= 0 ? seek_us : 0);
    if (err == AMEDIA_ERROR_END_OF_STREAM) return 1;
    if (err != AMEDIA_OK || buf == nullptr) return err == AMEDIA_OK ? 1 : -1;
    const uint8_t* data = static_cast<const uint8_t*>(buf->data(buf->handle));
    size_t off = buf->range_offset(buf->handle), len = buf->range_length(buf->handle);
    out->data.assign(data + off, data + off + len);
    AMediaFormat* meta = buf->meta_data(buf->handle);
    int64_t t = 0;
    out->time_us = meta != nullptr && AMediaFormat_getInt64(meta, "timeUs", &t) ? t : 0;
    int32_t v = 0;
    out->sync = meta != nullptr && AMediaFormat_getInt32(meta, "is-sync-frame", &v) && v != 0;
    out->valid_samples =
        meta != nullptr && AMediaFormat_getInt32(meta, "valid-samples", &v) ? v : -1;
    buf->release(buf->handle);
    return 0;
  }

 private:
  CMediaTrack* track_;
  android::MediaBufferGroup group_;
  bool started_ = false;
};

class AospExtractor : public Extractor {
 public:
  AospExtractor(std::unique_ptr<SourceAdapter> source, CMediaExtractor* ex, std::string name)
      : source_(std::move(source)), ex_(ex), name_(std::move(name)) {}
  ~AospExtractor() override {
    ex_->free(ex_->data);
    free(ex_);  // malloc'd by the plugin helper's wrap()
  }

  std::string Name() const override { return name_; }
  size_t TrackCount() override { return ex_->countTracks(ex_->data); }

  Format TrackFormat(size_t index) override {
    Format f;
    ex_->getTrackMetaData(ex_->data, f.get(), index, 0);
    return f;
  }

  Format FileFormat() override {
    Format f;
    ex_->getMetaData(ex_->data, f.get());
    return f;
  }

  std::unique_ptr<TrackReader> OpenTrack(size_t index) override {
    CMediaTrack* t = ex_->getTrack(ex_->data, index);
    if (t == nullptr) return nullptr;
    return std::make_unique<AospTrackReader>(t);
  }

 private:
  std::unique_ptr<SourceAdapter> source_;
  CMediaExtractor* ex_;
  std::string name_;
};

}  // namespace

std::unique_ptr<Extractor> Extractor::Open(std::shared_ptr<DataSource> source, std::string* error) {
  auto adapter = std::make_unique<SourceAdapter>(std::move(source));
  float best = 0;
  const Plugin* best_plugin = nullptr;
  android::CreatorFunc best_creator = nullptr;
  void* best_meta = nullptr;
  android::FreeMetaFunc best_free = nullptr;
  for (const Plugin& p : kPlugins) {
    ExtractorDef def = p.def();
    float confidence = 0;
    void* meta = nullptr;
    android::FreeMetaFunc free_meta = nullptr;
    android::CreatorFunc creator = def.u.v3.sniff(&adapter->c, &confidence, &meta, &free_meta);
    if (creator != nullptr && confidence > best) {
      if (best_free != nullptr) best_free(best_meta);
      best = confidence;
      best_plugin = &p;
      best_creator = creator;
      best_meta = meta;
      best_free = free_meta;
    } else if (free_meta != nullptr) {
      free_meta(meta);
    }
  }
  if (best_creator == nullptr) {
    *error = "no extractor recognizes the data";
    return nullptr;
  }
  CMediaExtractor* ex = best_creator(&adapter->c, best_meta);
  if (best_free != nullptr) best_free(best_meta);
  if (ex == nullptr) {
    *error = std::string(best_plugin->name) + " extractor failed to open the data";
    return nullptr;
  }
  return std::make_unique<AospExtractor>(std::move(adapter), ex, best_plugin->name);
}

}  // namespace gesher::media
