// NDK extraction over the same AOSP container readers used by MediaExtractor.
#include "media_extractor_abi.h"

#include <gesher/bionic_shim.h>
#include <gesher/inet.h>
#include <media/NdkMediaErrorPriv.h>
#include <media/NdkMediaFormat.h>
#include <media/stagefright/MediaErrors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

#include "../player/extractor_format.h"

namespace gesher::media::ndk_extractor {

namespace abi = extractor_abi;

struct SourceCallbacks {
  void* userdata = nullptr;
  abi::ReadAt read_at = nullptr;
  abi::GetSize get_size = nullptr;
  abi::Close close = nullptr;
  abi::GetAvailableSize available = nullptr;
};

// Setters replace this configuration, so existing attachments keep the callbacks
// and userdata they captured. Explicit descriptor close stops that configuration;
// extractor teardown closes only its own attachment. Neither path holds a lock
// across close: the application's close callback must be able to unblock readAt.
struct SourceState {
  std::mutex mutex;
  SourceCallbacks callbacks;
  std::shared_ptr<DataSource> uri;
  bool closed = false;

  std::shared_ptr<SourceState> Copy() {
    std::lock_guard lock(mutex);
    auto copy = std::make_shared<SourceState>();
    copy->callbacks = callbacks;
    copy->uri = uri;
    copy->closed = closed;
    return copy;
  }

  bool Attach(SourceCallbacks* out, std::shared_ptr<DataSource>* backend) {
    std::lock_guard lock(mutex);
    if (closed) return false;
    *out = callbacks;
    *backend = uri;
    return true;
  }

  void Close() {
    SourceCallbacks handlers;
    std::shared_ptr<DataSource> backend;
    {
      std::lock_guard lock(mutex);
      closed = true;
      handlers = callbacks;
      backend = uri;
    }
    if (backend != nullptr) backend->Cancel();
    if (handlers.close != nullptr) handlers.close(handlers.userdata);
  }
};

class NdkSource final : public DataSource {
 public:
  NdkSource(std::shared_ptr<SourceState> state, SourceCallbacks handlers,
            std::shared_ptr<DataSource> backend)
      : state_(std::move(state)), handlers_(handlers), backend_(std::move(backend)) {}

  ssize_t ReadAt(int64_t offset, void* dst, size_t size) override {
    if (offset < 0) return -EINVAL;
    if (size == 0) return 0;
    if (dst == nullptr || size > static_cast<size_t>(std::numeric_limits<ssize_t>::max())) {
      return -EINVAL;
    }
    std::lock_guard lock(callback_mutex_);
    if (IsClosed()) return -ECANCELED;
    if (handlers_.read_at != nullptr && handlers_.userdata == nullptr) return 0;
    const ssize_t result = handlers_.read_at != nullptr ?
        handlers_.read_at(handlers_.userdata, offset, dst, size) :
        backend_->ReadAt(offset, dst, size);
    if (IsClosed()) return -ECANCELED;
    if (handlers_.read_at != nullptr && result == -1) return 0;  // NDK EOF.
    if (result > static_cast<ssize_t>(size)) return -EIO;
    return result;
  }

  int64_t Size() override {
    std::lock_guard lock(callback_mutex_);
    if (IsClosed()) return -1;
    if (handlers_.get_size != nullptr && handlers_.userdata == nullptr) return -1;
    const int64_t result = handlers_.get_size != nullptr ?
        handlers_.get_size(handlers_.userdata) : (backend_ != nullptr ? backend_->Size() : -1);
    return IsClosed() ? -1 : result;
  }

  void Cancel() override {
    if (cancelled_.exchange(true)) return;
    if (backend_ != nullptr) backend_->Cancel();
    if (handlers_.close != nullptr && handlers_.userdata != nullptr) {
      handlers_.close(handlers_.userdata);
    }
  }

  std::string Uri() const override { return backend_ != nullptr ? backend_->Uri() : ""; }
  bool IsNetwork() const override { return backend_ != nullptr && backend_->IsNetwork(); }

  int64_t CachedBytes() override {
    std::lock_guard lock(callback_mutex_);
    if (IsClosed()) return -1;
    if (handlers_.available != nullptr && handlers_.userdata == nullptr) return -1;
    // NuMediaExtractor requests the current-read position with this sentinel.
    const int64_t result = handlers_.available != nullptr ?
        handlers_.available(handlers_.userdata, -1) :
        (backend_ != nullptr ? backend_->CachedBytes() : -1);
    return IsClosed() ? -1 : result;
  }

 private:
  bool IsClosed() const {
    if (cancelled_.load()) return true;
    std::lock_guard lock(state_->mutex);
    return state_->closed;
  }

  std::shared_ptr<SourceState> state_;
  SourceCallbacks handlers_;
  std::shared_ptr<DataSource> backend_;
  std::mutex callback_mutex_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace gesher::media::ndk_extractor

struct AMediaDataSource {
  std::mutex mutex;
  std::shared_ptr<gesher::media::ndk_extractor::SourceState> state =
      std::make_shared<gesher::media::ndk_extractor::SourceState>();
};

struct AMediaCodecCryptoInfo {
  std::array<uint8_t, 16> key, iv;
  int32_t mode = 0;
  gesher::media::extractor_abi::CryptoPattern pattern{};
  std::vector<size_t> clear, encrypted;
};

struct AMediaExtractor {
  struct Track {
    size_t index;
    std::unique_ptr<gesher::media::TrackReader> reader;
    std::string mime;
    gesher::media::Sample sample;
    bool pending = false;
    int result = AMEDIA_OK;
  };

  std::mutex mutex;
  std::shared_ptr<gesher::media::DataSource> source;
  std::unique_ptr<gesher::media::Extractor> extractor;
  std::vector<Track> selected;
  int64_t total_bitrate = -1;
  int64_t duration_us = -1;
  std::vector<uint8_t> pssh_data;
  std::unique_ptr<gesher::media::extractor_abi::PsshInfo, decltype(&std::free)> pssh{
      nullptr, std::free};

  Track* Current(media_status_t* status = nullptr, int64_t seek = -1,
                 gesher::media::SeekMode mode = gesher::media::SeekMode::kPreviousSync) {
    Track* current = nullptr;
    for (Track& track : selected) {
      if (seek >= 0 || (!track.pending && track.result == AMEDIA_OK)) {
        track.result = track.reader->Read(&track.sample, seek, mode);
        track.pending = track.result == AMEDIA_OK;
      }
      if (track.result < 0) {
        if (status != nullptr) {
          *status = translate_error(reverse_translate_error(static_cast<media_status_t>(track.result)));
        }
        return nullptr;
      }
      if (track.pending && (current == nullptr || track.sample.time_us < current->sample.time_us)) {
        current = &track;
      }
    }
    if (status != nullptr) *status = current == nullptr ? AMEDIA_ERROR_END_OF_STREAM : AMEDIA_OK;
    return current;
  }
};

namespace gesher::media {
namespace {

namespace abi = extractor_abi;
using ndk_extractor::NdkSource;
using ndk_extractor::SourceCallbacks;

std::unique_ptr<DataSource> UriSource(const char* uri,
                                     const std::map<std::string, std::string>& headers) {
  if (uri == nullptr || *uri == '\0') return nullptr;
  std::string address(uri);
  const size_t scheme_end = address.find("://");
  std::string scheme = address.substr(0, scheme_end);
  for (char& c : scheme) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  }
  std::string error;
  if (scheme_end != std::string::npos && (scheme == "http" || scheme == "https")) {
    if (!gesher_inet_allowed()) return nullptr;
    address.replace(0, scheme_end, scheme);
    return OpenHttpSource(address, headers, &error);
  }
  if (scheme_end != std::string::npos) {
    if (scheme != "file") return nullptr;
    address.erase(0, scheme_end + 3);
  }
  return OpenFileSource(address, &error);
}

AMediaDataSource* NewDataSource() { return new (std::nothrow) AMediaDataSource; }

AMediaDataSource* NewUri(const char* uri, int count, const char* const* pairs) {
  if (count < 0 || (count != 0 && pairs == nullptr)) return nullptr;
  std::map<std::string, std::string> headers;
  for (int i = 0; i < count; ++i, pairs += 2) {
    if (pairs[0] == nullptr || pairs[1] == nullptr) return nullptr;
    headers.insert_or_assign(pairs[0], pairs[1]);
  }
  auto source = UriSource(uri, headers);
  if (source == nullptr) return nullptr;
  auto* descriptor = NewDataSource();
  if (descriptor != nullptr) descriptor->state->uri = std::move(source);
  return descriptor;
}

void DeleteDataSource(AMediaDataSource* source) { delete source; }

void CloseDataSource(AMediaDataSource* source) {
  if (source == nullptr) return;
  std::shared_ptr<ndk_extractor::SourceState> state;
  {
    std::lock_guard lock(source->mutex);
    state = source->state;
  }
  // Every explicit close is forwarded, including repeated close calls.
  state->Close();
}

template <auto Field, class Value>
void SetCallback(AMediaDataSource* source, Value value) {
  if (source == nullptr) return;
  std::lock_guard lock(source->mutex);
  // Before attachment (and after failed setup) the descriptor owns the only
  // reference, so ordinary setter calls need no new allocation.
  if (source->state.use_count() != 1) source->state = source->state->Copy();
  std::lock_guard state_lock(source->state->mutex);
  source->state->callbacks.*Field = value;
}

AMediaExtractor* New() { return new (std::nothrow) AMediaExtractor; }

media_status_t Delete(AMediaExtractor* ex) {
  if (ex != nullptr) {
    ex->selected.clear();
    ex->extractor.reset();
    if (ex->source != nullptr) ex->source->Cancel();
  }
  delete ex;
  return AMEDIA_OK;
}

// Source setup is transactional and repeatable after a failed first setup.
// All call sites hold ex.mutex; callbacks can cancel via the separate source
// state without acquiring that mutex.
media_status_t Open(AMediaExtractor& ex, std::shared_ptr<DataSource> source) {
  if (source == nullptr) return AMEDIA_ERROR_UNKNOWN;
  std::string error;
  auto extractor = Extractor::Open(source, &error);
  if (extractor == nullptr) return translate_error(android::ERROR_UNSUPPORTED);
  // Cache immutable metadata as NuMediaExtractor does, rather than allocate
  // every track's format again for each cached-duration query.
  const size_t tracks = extractor->TrackCount();
  if (tracks > 16384) return translate_error(android::ERROR_UNSUPPORTED);
  int64_t bitrate = 0, duration = -1;
  for (size_t i = 0; i < tracks; ++i) {
    const auto format = extractor->TrackFormat(i);
    int32_t track_rate;
    if (!format.GetInt32("bitrate", &track_rate)) bitrate = -1;
    else if (bitrate >= 0) bitrate += track_rate;
    int64_t track_duration;
    if (format.GetInt64("durationUs", &track_duration)) {
      duration = std::max(duration, track_duration);
    }
  }
  ex.source = std::move(source);
  ex.extractor = std::move(extractor);
  ex.total_bitrate = bitrate;
  ex.duration_us = duration;
  return AMEDIA_OK;
}

media_status_t SetFd(AMediaExtractor* ex, int fd, int64_t offset, int64_t length) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (fd < 0 || offset < 0 || length < 0) return translate_error(android::BAD_VALUE);
  std::lock_guard lock(ex->mutex);
  if (ex->extractor != nullptr) return translate_error(-EINVAL);
  std::string error;
  return Open(*ex, OpenFdSource(fd, offset, length, &error));
}

media_status_t SetUri(AMediaExtractor* ex, const char* uri) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (uri == nullptr || *uri == '\0') return translate_error(-EINVAL);
  std::lock_guard lock(ex->mutex);
  if (ex->extractor != nullptr) return translate_error(-EINVAL);
  return Open(*ex, UriSource(uri, {}));
}

media_status_t SetCustom(AMediaExtractor* ex, AMediaDataSource* source) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (source == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(ex->mutex);
  if (ex->extractor != nullptr) return translate_error(-EINVAL);

  SourceCallbacks handlers;
  std::shared_ptr<DataSource> backend;
  std::shared_ptr<ndk_extractor::SourceState> state;
  {
    std::lock_guard source_lock(source->mutex);
    state = source->state;
  }
  if (!state->Attach(&handlers, &backend)) return AMEDIA_ERROR_UNKNOWN;
  if (handlers.read_at == nullptr && backend == nullptr) return AMEDIA_ERROR_UNKNOWN;
  auto adapted = std::make_shared<NdkSource>(std::move(state), handlers, std::move(backend));
  return Open(*ex, std::move(adapted));
}

size_t TrackCount(AMediaExtractor* ex) {
  if (ex == nullptr) return 0;
  std::lock_guard lock(ex->mutex);
  return ex->extractor != nullptr ? ex->extractor->TrackCount() : 0;
}

Format TrackFormat(Extractor& extractor, size_t index) {
  Format format = ExtractorTrackFormat(&extractor, index);
  int32_t track_id;
  if (index < static_cast<size_t>(INT32_MAX) && !format.GetInt32("track-id", &track_id)) {
    AMediaFormat_setInt32(format.get(), "track-id", static_cast<int32_t>(index + 1));
  }
  return format;
}

AMediaFormat* GetTrackFormat(AMediaExtractor* ex, size_t index) {
  if (ex == nullptr) return nullptr;
  std::lock_guard lock(ex->mutex);
  if (ex->extractor == nullptr || index >= ex->extractor->TrackCount()) return AMediaFormat_new();
  return TrackFormat(*ex->extractor, index).Release();
}

AMediaFormat* GetFileFormat(AMediaExtractor* ex) {
  if (ex == nullptr) return nullptr;
  std::lock_guard lock(ex->mutex);
  return ex->extractor != nullptr ? ex->extractor->FileFormat().Release() : AMediaFormat_new();
}

media_status_t Select(AMediaExtractor* ex, size_t index) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  std::lock_guard lock(ex->mutex);
  if (ex->extractor == nullptr || index >= ex->extractor->TrackCount()) {
    return translate_error(ex->extractor == nullptr ? -EINVAL : -ERANGE);
  }
  if (std::ranges::any_of(ex->selected, [=](const auto& track) { return track.index == index; })) {
    return AMEDIA_OK;
  }
  auto reader = ex->extractor->OpenTrack(index);
  if (reader == nullptr) return AMEDIA_ERROR_MALFORMED;
  // CMediaTrack returns NDK statuses; AOSP unwraps them to native statuses and
  // the public extractor wrapper translates them back (unlisted errors -> UNKNOWN).
  const auto start_status = static_cast<media_status_t>(reader->StartStatus());
  if (start_status != AMEDIA_OK) return translate_error(reverse_translate_error(start_status));
  reader->EnableSampleMetadata();
  std::string mime = ex->extractor->TrackFormat(index).Mime();
  if (mime.empty()) return AMEDIA_ERROR_MALFORMED;
  ex->selected.push_back({index, std::move(reader), std::move(mime), {}});
  return AMEDIA_OK;
}

media_status_t Unselect(AMediaExtractor* ex, size_t index) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  std::lock_guard lock(ex->mutex);
  if (ex->extractor == nullptr || index >= ex->extractor->TrackCount()) {
    return translate_error(ex->extractor == nullptr ? -EINVAL : -ERANGE);
  }
  std::erase_if(ex->selected, [=](const auto& track) { return track.index == index; });
  return AMEDIA_OK;
}

bool Advance(AMediaExtractor* ex) {
  if (ex == nullptr) return false;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  if (track == nullptr) return false;
  track->pending = false;
  return ex->Current() != nullptr;
}

media_status_t Seek(AMediaExtractor* ex, int64_t time_us, int32_t mode) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  // AOSP treats a value other than previous/closest sync as next sync.
  const auto mode_abi = mode == 0 ? SeekMode::kPreviousSync :
      mode == 2 ? SeekMode::kClosestSync : SeekMode::kNextSync;
  std::lock_guard lock(ex->mutex);
  return ex->Current(nullptr, time_us, mode_abi) != nullptr ? AMEDIA_OK : AMEDIA_ERROR_END_OF_STREAM;
}

bool Vorbis(const AMediaExtractor::Track& t) { return t.mime == "audio/vorbis"; }

ssize_t SampleSize(const AMediaExtractor::Track& t) {
  const size_t trailer = Vorbis(t) ? sizeof(int32_t) : 0;
  if (t.sample.data.size() > static_cast<size_t>(std::numeric_limits<ssize_t>::max()) - trailer) return -1;
  return static_cast<ssize_t>(t.sample.data.size() + trailer);
}

ssize_t GetSampleSize(AMediaExtractor* ex) {
  if (ex == nullptr) return -1;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  return track != nullptr ? SampleSize(*track) : -1;
}

ssize_t ReadSample(AMediaExtractor* ex, uint8_t* dst, size_t capacity) {
  if (ex == nullptr) return -1;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  if (track == nullptr) return -1;
  const ssize_t total = SampleSize(*track);
  if (total < 0 || static_cast<size_t>(total) > capacity || (total != 0 && dst == nullptr)) return -1;
  const auto& sample = track->sample;
  if (!sample.data.empty()) std::memcpy(dst, sample.data.data(), sample.data.size());
  if (Vorbis(*track)) {
    std::memcpy(dst + sample.data.size(), &sample.valid_samples, sizeof(sample.valid_samples));
  }
  return total;
}

int GetSampleIndex(AMediaExtractor* ex) {
  if (ex == nullptr) return -1;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  return track != nullptr ? static_cast<int>(track->index) : -1;
}

int64_t GetSampleTime(AMediaExtractor* ex) {
  if (ex == nullptr) return -1;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  return track != nullptr ? track->sample.time_us : -1;
}

uint32_t GetFlags(AMediaExtractor* ex) {
  if (ex == nullptr) return UINT32_MAX;
  std::lock_guard lock(ex->mutex);
  auto* track = ex->Current();
  if (track == nullptr) return UINT32_MAX;
  void* encrypted = nullptr;
  size_t length = 0;
  const auto* meta = track->reader->SampleMetadata();
  const bool has_encryption = meta != nullptr &&
      AMediaFormat_getBuffer(meta->get(), "crypto-encrypted-sizes", &encrypted, &length);
  return (track->sample.sync ? 1u : 0u) | (has_encryption ? 2u : 0u);
}

media_status_t GetSampleFormat(AMediaExtractor* ex, AMediaFormat* dest) {
  if (ex == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (dest == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(ex->mutex);
  media_status_t status;
  auto* track = ex->Current(&status);
  if (track == nullptr) return status;
  AMediaFormat_clear(dest);
  AMediaFormat_setString(dest, "mime", track->mime.c_str());
  AMediaFormat_setInt32(dest, "track-index", static_cast<int32_t>(track->index));
  const auto* meta = track->reader->SampleMetadata();
  if (meta == nullptr) return AMEDIA_OK;
  int32_t layer;
  if (meta->GetInt32("temporal-layer-id", &layer)) {
    AMediaFormat_setInt32(dest, "temporal-layer-id", layer);
  }
  for (const char* key : {"durationUs", "sample-file-offset", "last-sample-index-in-chunk"}) {
    int64_t value;
    if (meta->GetInt64(key, &value)) AMediaFormat_setInt64(dest, key, value);
  }
  for (const char* key : {"sei", "mpeg-user-data", "audio-presentation-info"}) {
    void* data = nullptr;
    size_t size = 0;
    if (AMediaFormat_getBuffer(meta->get(), key, &data, &size)) {
      AMediaFormat_setBuffer(dest, key, data, size);
    }
  }
  return AMEDIA_OK;
}

abi::PsshInfo* GetPssh(AMediaExtractor* ex) {
  if (ex == nullptr) return nullptr;
  std::lock_guard lock(ex->mutex);
  if (ex->pssh != nullptr || ex->extractor == nullptr) return ex->pssh.get();
  auto format = ex->extractor->FileFormat();
  std::vector<uint8_t> bytes;
  if (!format.GetBuffer("pssh", &bytes) || bytes.empty()) return nullptr;

  // Plugins supply repeated UUID, host-order uint32 length, payload tuples.
  constexpr size_t header = 16 + sizeof(uint32_t);
  std::vector<size_t> offsets;
  for (size_t offset = 0; offset < bytes.size();) {
    if (bytes.size() - offset < header) return nullptr;
    uint32_t length;
    std::memcpy(&length, bytes.data() + offset + 16, sizeof(length));
    if (length > bytes.size() - offset - header) return nullptr;
    offsets.push_back(offset);
    offset += header + length;
  }

  constexpr size_t entries_offset = offsetof(abi::PsshInfo, entries);
  if (offsets.size() > (SIZE_MAX - entries_offset) / sizeof(abi::PsshEntry)) return nullptr;
  const size_t allocation = entries_offset + offsets.size() * sizeof(abi::PsshEntry);
  ex->pssh.reset(static_cast<abi::PsshInfo*>(std::malloc(allocation)));
  if (ex->pssh == nullptr) return nullptr;
  ex->pssh_data = std::move(bytes);
  ex->pssh->numentries = offsets.size();
  for (size_t i = 0; i < offsets.size(); ++i) {
    uint8_t* data = ex->pssh_data.data() + offsets[i];
    // Trailing entries follow the Android ABI's zero-length array.
    auto* entry = reinterpret_cast<abi::PsshEntry*>(
        reinterpret_cast<uint8_t*>(ex->pssh.get()) + entries_offset) + i;
    std::memcpy(entry->uuid, data, sizeof(entry->uuid));
    uint32_t length;
    std::memcpy(&length, data + 16, sizeof(length));
    entry->datalen = length;
    entry->data = data + header;
  }
  return ex->pssh.get();
}

AMediaCodecCryptoInfo* NewCrypto(int count, const uint8_t* key, const uint8_t* iv,
                                int32_t mode, const size_t* clear, const size_t* encrypted) {
  if (count < 0 || key == nullptr || iv == nullptr || (count > 0 && encrypted == nullptr)) {
    return nullptr;
  }
  auto info = std::unique_ptr<AMediaCodecCryptoInfo>(new (std::nothrow) AMediaCodecCryptoInfo);
  if (info == nullptr) return nullptr;
  std::memcpy(info->key.data(), key, info->key.size());
  std::memcpy(info->iv.data(), iv, info->iv.size());
  info->mode = mode;
  if (count > 0) {
    if (clear != nullptr) info->clear.assign(clear, clear + count);
    else info->clear.resize(count);
    info->encrypted.assign(encrypted, encrypted + count);
  }
  return info.release();
}

bool ReadSubsamples(const Format& meta, const char* key, std::vector<size_t>* out) {
  void* bytes = nullptr;
  size_t length = 0;
  if (!AMediaFormat_getBuffer(meta.get(), key, &bytes, &length)) return true;
  if (length % sizeof(uint32_t) != 0 || (length > 0 && bytes == nullptr)) return false;
  const auto* p = static_cast<const uint8_t*>(bytes);
  out->resize(length / sizeof(uint32_t));
  for (size_t i = 0; i < out->size(); ++i) {
    uint32_t value;
    std::memcpy(&value, p + i * sizeof(value), sizeof(value));
    (*out)[i] = value;
  }
  return true;
}

AMediaCodecCryptoInfo* GetSampleCrypto(AMediaExtractor* ex) {
  if (ex == nullptr) return nullptr;
  std::lock_guard lock(ex->mutex);
  const auto* track = ex->Current();
  if (track == nullptr) return nullptr;
  const Format* meta = track->reader->SampleMetadata();
  if (meta == nullptr) return nullptr;

  auto info = std::unique_ptr<AMediaCodecCryptoInfo>(new (std::nothrow) AMediaCodecCryptoInfo);
  if (info == nullptr ||
      !ReadSubsamples(*meta, "crypto-encrypted-sizes", &info->encrypted) ||
      info->encrypted.empty() || !ReadSubsamples(*meta, "crypto-plain-sizes", &info->clear)) {
    return nullptr;
  }

  if (info->clear.empty()) info->clear.resize(info->encrypted.size());
  if (info->clear.size() != info->encrypted.size()) return nullptr;
  void* key = nullptr;
  void* iv = nullptr;
  size_t key_size = 0, iv_size = 0;
  if (!AMediaFormat_getBuffer(meta->get(), "crypto-key", &key, &key_size) ||
      !AMediaFormat_getBuffer(meta->get(), "crypto-iv", &iv, &iv_size) ||
      key == nullptr || iv == nullptr || key_size != 16 || iv_size != 16) {
    return nullptr;
  }
  std::memcpy(info->key.data(), key, info->key.size());
  std::memcpy(info->iv.data(), iv, info->iv.size());
  size_t remaining = track->sample.data.size();
  for (size_t i = 0; i < info->encrypted.size(); ++i) {
    if (info->clear[i] > remaining) return nullptr;
    remaining -= info->clear[i];
    if (info->encrypted[i] > remaining) return nullptr;
    remaining -= info->encrypted[i];
  }
  if (remaining != 0) return nullptr;

  info->mode = 1;  // AES-CTR by default, as the Android extractor.
  meta->GetInt32("crypto-mode", &info->mode);
  meta->GetInt32("crypto-encrypted-byte-block", &info->pattern.encryptBlocks);
  meta->GetInt32("crypto-skip-byte-block", &info->pattern.skipBlocks);
  if (Vorbis(*track)) {
    info->clear.push_back(sizeof(int32_t));
    info->encrypted.push_back(0);
  }
  return info.release();
}

media_status_t DeleteCrypto(AMediaCodecCryptoInfo* info) {
  delete info;
  return AMEDIA_OK;
}

void SetCryptoPattern(AMediaCodecCryptoInfo* info, const abi::CryptoPattern* pattern) {
  if (info != nullptr && pattern != nullptr) info->pattern = *pattern;
}

size_t CryptoSubsamples(AMediaCodecCryptoInfo* info) {
  return info != nullptr ? info->encrypted.size() : 0;
}

int32_t CryptoMode(AMediaCodecCryptoInfo* info) { return info != nullptr ? info->mode : 0; }

template <auto Field, class Value>
media_status_t CryptoCopy(AMediaCodecCryptoInfo* info, Value* dest) {
  if (info == nullptr) return AMEDIA_ERROR_INVALID_OBJECT;
  if (dest == nullptr) return AMEDIA_ERROR_INVALID_PARAMETER;
  const auto& field = info->*Field;
  std::copy(field.begin(), field.end(), dest);
  return AMEDIA_OK;
}

int64_t GetCachedDuration(AMediaExtractor* ex) {
  if (ex == nullptr) return -1;
  std::lock_guard lock(ex->mutex);
  if (ex->source == nullptr) return -1;
  const int64_t cached = ex->source->CachedBytes();
  if (cached < 0) return -1;

  long double estimate;
  if (ex->total_bitrate > 0) {
    estimate = static_cast<long double>(cached) * 8000000 / ex->total_bitrate;
  } else {
    const int64_t file_size = ex->source->Size();
    if (file_size <= 0 || ex->duration_us <= 0) return -1;
    estimate = static_cast<long double>(cached) * ex->duration_us / file_size;
  }
  return estimate >= static_cast<long double>(INT64_MAX) ? INT64_MAX :
      static_cast<int64_t>(estimate);
}

template <auto F> struct Guest;
template <class R, class... A, R (*F)(A...)>
struct Guest<F> {
  static R GESHER_MEDIA_GUEST_ABI Call(A... args) { return F(args...); }
};

void Register() {
  using gesher::bionic::AddBionicSymbol;
#define MEDIA_ENTRY(name, ...) AddBionicSymbol("libmediandk.so", #name, reinterpret_cast<void*>(&Guest<&__VA_ARGS__>::Call))
  MEDIA_ENTRY(AMediaDataSource_new, NewDataSource);
  MEDIA_ENTRY(AMediaDataSource_newUri, NewUri);
  MEDIA_ENTRY(AMediaDataSource_delete, DeleteDataSource);
  MEDIA_ENTRY(AMediaDataSource_close, CloseDataSource);
  MEDIA_ENTRY(AMediaDataSource_setUserdata, SetCallback<&SourceCallbacks::userdata, void*>);
  MEDIA_ENTRY(AMediaDataSource_setGetSize, SetCallback<&SourceCallbacks::get_size, abi::GetSize>);
  MEDIA_ENTRY(AMediaDataSource_setReadAt, SetCallback<&SourceCallbacks::read_at, abi::ReadAt>);
  MEDIA_ENTRY(AMediaDataSource_setClose, SetCallback<&SourceCallbacks::close, abi::Close>);
  MEDIA_ENTRY(AMediaDataSource_setGetAvailableSize,
              SetCallback<&SourceCallbacks::available, abi::GetAvailableSize>);
  MEDIA_ENTRY(AMediaExtractor_new, New);
  MEDIA_ENTRY(AMediaExtractor_delete, Delete);
  MEDIA_ENTRY(AMediaExtractor_setDataSourceFd, SetFd);
  MEDIA_ENTRY(AMediaExtractor_setDataSource, SetUri);
  MEDIA_ENTRY(AMediaExtractor_setDataSourceCustom, SetCustom);
  MEDIA_ENTRY(AMediaExtractor_getTrackCount, TrackCount);
  MEDIA_ENTRY(AMediaExtractor_getTrackFormat, GetTrackFormat);
  MEDIA_ENTRY(AMediaExtractor_getFileFormat, GetFileFormat);
  MEDIA_ENTRY(AMediaExtractor_selectTrack, Select);
  MEDIA_ENTRY(AMediaExtractor_unselectTrack, Unselect);
  MEDIA_ENTRY(AMediaExtractor_advance, Advance);
  MEDIA_ENTRY(AMediaExtractor_seekTo, Seek);
  MEDIA_ENTRY(AMediaExtractor_readSampleData, ReadSample);
  MEDIA_ENTRY(AMediaExtractor_getSampleSize, GetSampleSize);
  MEDIA_ENTRY(AMediaExtractor_getSampleFlags, GetFlags);
  MEDIA_ENTRY(AMediaExtractor_getSampleTrackIndex, GetSampleIndex);
  MEDIA_ENTRY(AMediaExtractor_getSampleTime, GetSampleTime);
  MEDIA_ENTRY(AMediaExtractor_getSampleFormat, GetSampleFormat);
  MEDIA_ENTRY(AMediaExtractor_getCachedDuration, GetCachedDuration);
  MEDIA_ENTRY(AMediaExtractor_getPsshInfo, GetPssh);
  MEDIA_ENTRY(AMediaExtractor_getSampleCryptoInfo, GetSampleCrypto);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_new, NewCrypto);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_delete, DeleteCrypto);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_setPattern, SetCryptoPattern);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getNumSubSamples, CryptoSubsamples);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getMode, CryptoMode);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getKey, CryptoCopy<&AMediaCodecCryptoInfo::key, uint8_t>);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getIV, CryptoCopy<&AMediaCodecCryptoInfo::iv, uint8_t>);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getClearBytes, CryptoCopy<&AMediaCodecCryptoInfo::clear, size_t>);
  MEDIA_ENTRY(AMediaCodecCryptoInfo_getEncryptedBytes,
              CryptoCopy<&AMediaCodecCryptoInfo::encrypted, size_t>);
#undef MEDIA_ENTRY
}

[[maybe_unused]] const bool kRegistered = (Register(), true);

}  // namespace
}  // namespace gesher::media
