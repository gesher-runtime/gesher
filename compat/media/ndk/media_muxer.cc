// Native muxing over the same real container writers as MediaMuxer (ADR 0064).
#include <cstdint>
#include <cstddef>

#include <media/NdkMediaErrorPriv.h>
#include <media/NdkMediaFormatPriv.h>
#include <media/stagefright/foundation/AMessage.h>

#include <gesher/bionic_shim.h>
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cerrno>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "../mux/muxer.h"
#include "media_muxer_abi.h"

static_assert(sizeof(ssize_t) == 8);
using gesher::media::muxer_abi::OutputFormat;
using gesher::media::muxer_abi::BufferInfo;

struct AMediaMuxer {
  enum class State { kInitialized, kStarted, kStopped };

  AMediaMuxer(std::unique_ptr<gesher::media::Muxer> impl, OutputFormat output)
      : writer(std::move(impl)), format(output) {}

  std::mutex mutex;
  std::unique_ptr<gesher::media::Muxer> writer;
  OutputFormat format;
  State state = State::kInitialized;
  bool writer_started = false;
  std::vector<android::sp<android::AMessage>> formats;
  std::map<size_t, int64_t> last_webm_time;
};

namespace gesher::media {
using enum muxer_abi::OutputFormat;
namespace {

// Gesher's F_GETFL currently reports O_RDWR for ordinary disk descriptors.
// Check the actual duplicated Win32 handle instead of inferring write access.
bool SuitableFd(int fd, bool needs_read) {
  if (fd < 0) return false;
  struct stat status;
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) return false;
  const int copy = dup(fd);
  if (copy < 0) return false;
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(copy));
  DWORD written = 0;
  uint8_t byte = 0;
  const bool writable = handle != INVALID_HANDLE_VALUE &&
                        handle != reinterpret_cast<HANDLE>(static_cast<intptr_t>(-2)) &&
                        WriteFile(handle, &byte, 0, &written, nullptr) != 0;
  // Pinned MediaWriter validates write access and seeks to zero before
  // WebmWriter performs its additional read-access check. dup shares the
  // caller's offset, including a failed write-only WebM construction.
  const bool seekable = writable && lseek(copy, 0, SEEK_SET) == 0;
  DWORD received = 0;
  const bool readable = !needs_read || (seekable && ReadFile(handle, &byte, 0, &received, nullptr) != 0);
  close(copy);
  return seekable && readable;
}

media_status_t BadValue() { return translate_error(android::BAD_VALUE); }

AMediaMuxer* New(int fd, OutputFormat format) {
  MuxerFormat selected;
  switch (format) {
    case AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4: selected = MuxerFormat::kMpeg4; break;
    case AMEDIAMUXER_OUTPUT_FORMAT_WEBM: selected = MuxerFormat::kWebm; break;
    case AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP: selected = MuxerFormat::kThreeGpp; break;
    case AMEDIAMUXER_OUTPUT_FORMAT_OGG: selected = MuxerFormat::kOgg; break;
    default: return nullptr;
  }
  if (!SuitableFd(fd, format == AMEDIAMUXER_OUTPUT_FORMAT_WEBM)) return nullptr;
  std::string error;
  auto writer = Muxer::Create(fd, selected, &error);
  return writer != nullptr ? new (std::nothrow) AMediaMuxer(std::move(writer), format) : nullptr;
}

media_status_t Delete(AMediaMuxer* muxer) {
  if (muxer != nullptr && muxer->state == AMediaMuxer::State::kStarted && muxer->writer_started) {
    std::string error;
    muxer->writer->Stop(&error);
  }
  delete muxer;
  return AMEDIA_OK;
}

media_status_t SetOrientation(AMediaMuxer* muxer, int degrees) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (muxer->state != AMediaMuxer::State::kInitialized) return AMEDIA_ERROR_INVALID_OPERATION;
  if (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270) return BadValue();
  muxer->writer->SetOrientationHint(degrees);
  return AMEDIA_OK;
}

media_status_t SetLocation(AMediaMuxer* muxer, float latitude, float longitude) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (muxer->state != AMediaMuxer::State::kInitialized ||
      (muxer->format != AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 &&
       muxer->format != AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP)) return AMEDIA_ERROR_INVALID_OPERATION;
  // The pinned NDK wrapper multiplies in float and truncates before the
  // writer's integer range check. Keep that order, without an invalid cast.
  const float lat = latitude * 10000, lon = longitude * 10000;
  if (!std::isfinite(lat) || !std::isfinite(lon) ||
      std::fabs(lat) >= 900001 || std::fabs(lon) >= 1800001) return BadValue();
  const int lat_e4 = static_cast<int>(lat), lon_e4 = static_cast<int>(lon);
  if (lat_e4 < -900000 || lat_e4 > 900000 || lon_e4 < -1800000 || lon_e4 > 1800000) return BadValue();
  muxer->writer->SetLocation(lat_e4, lon_e4);
  return AMEDIA_OK;
}

ssize_t AddTrack(AMediaMuxer* muxer, const AMediaFormat* format) {
  android::sp<android::AMessage> message;
  if (format != nullptr) android::AMediaFormat_getFormat(format, &message);
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (message == nullptr) return BadValue();
  if (muxer->state != AMediaMuxer::State::kInitialized) return AMEDIA_ERROR_INVALID_OPERATION;
  // The format wrapper is temporary, while the actual AMessage is retained
  // for getTrackFormat's independently owned wrapper, as in pinned AOSP.
  Format view(android::AMediaFormat_fromMsg(&message));
  std::string error;
  const int index = muxer->writer->AddTrack(view, &error);
  if (index < 0) return AMEDIA_ERROR_UNKNOWN;
  muxer->formats.push_back(std::move(message));
  return index;
}

media_status_t Start(AMediaMuxer* muxer) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (muxer->state != AMediaMuxer::State::kInitialized) return AMEDIA_ERROR_INVALID_OPERATION;
  muxer->state = AMediaMuxer::State::kStarted;
  if (muxer->formats.empty()) return muxer->format == AMEDIAMUXER_OUTPUT_FORMAT_OGG ?
      AMEDIA_ERROR_UNKNOWN : AMEDIA_ERROR_INVALID_OPERATION;
  std::string error;
  errno = 0;
  muxer->writer_started = muxer->writer->Start(&error);
  return muxer->writer_started ? AMEDIA_OK :
      (errno != 0 ? translate_error(-errno) : AMEDIA_ERROR_UNKNOWN);
}

media_status_t Stop(AMediaMuxer* muxer) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (muxer->state != AMediaMuxer::State::kStarted) return AMEDIA_ERROR_INVALID_OPERATION;
  muxer->state = AMediaMuxer::State::kStopped;
  if (muxer->formats.empty()) return AMEDIA_OK;
  std::string error;
  errno = 0;
  return muxer->writer->Stop(&error) ? AMEDIA_OK :
      (errno != 0 ? translate_error(-errno) : AMEDIA_ERROR_UNKNOWN);
}

media_status_t WriteSample(AMediaMuxer* muxer, size_t track, const uint8_t* data,
                           const BufferInfo* info) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (info == nullptr) return BadValue();
  if (muxer->format == AMEDIAMUXER_OUTPUT_FORMAT_WEBM) {
    const auto previous = muxer->last_webm_time.find(track);
    const int64_t before = previous != muxer->last_webm_time.end() ? previous->second : 0;
    const int64_t time = info->presentationTimeUs;
    if (time < 0 || time < before || time > INT64_MAX / 1000 - (time - before)) return BadValue();
    // Pinned WebmWriter validates and updates this metadata before checking
    // the muxer's state or track index, including unsuccessful writes.
    muxer->last_webm_time[track] = time;
  }
  if (muxer->state != AMediaMuxer::State::kStarted) return AMEDIA_ERROR_INVALID_OPERATION;
  if (track >= muxer->formats.size() || data == nullptr ||
      info->offset < 0 || info->size < 0) return BadValue();
  std::string error;
  errno = 0;
  return muxer->writer->WriteSample(static_cast<int>(track), data + info->offset,
      static_cast<size_t>(info->size), info->presentationTimeUs, info->flags, &error) ? AMEDIA_OK :
      (errno != 0 ? translate_error(-errno) : AMEDIA_ERROR_UNKNOWN);
}

ssize_t TrackCount(AMediaMuxer* muxer) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  return muxer->state != AMediaMuxer::State::kStopped ? static_cast<ssize_t>(muxer->formats.size()) : -1;
}

AMediaFormat* TrackFormat(AMediaMuxer* muxer, size_t index) {
  std::lock_guard<std::mutex> lock(muxer->mutex);
  if (muxer->state == AMediaMuxer::State::kStopped || index >= muxer->formats.size()) return nullptr;
  return android::AMediaFormat_fromMsg(&muxer->formats[index]);
}

template <auto F> struct Sysv;
template <class R, class... A, R (*F)(A...)>
struct Sysv<F> {
  static R __attribute__((sysv_abi)) Call(A... args) { return F(args...); }
};

void Register() {
  using gesher::bionic::AddBionicSymbol;
#define MUXER_ENTRY(name, fn) AddBionicSymbol(name, reinterpret_cast<void*>(&Sysv<&fn>::Call))
  MUXER_ENTRY("AMediaMuxer_new", New);
  MUXER_ENTRY("AMediaMuxer_delete", Delete);
  MUXER_ENTRY("AMediaMuxer_setLocation", SetLocation);
  MUXER_ENTRY("AMediaMuxer_setOrientationHint", SetOrientation);
  MUXER_ENTRY("AMediaMuxer_addTrack", AddTrack);
  MUXER_ENTRY("AMediaMuxer_start", Start);
  MUXER_ENTRY("AMediaMuxer_stop", Stop);
  MUXER_ENTRY("AMediaMuxer_writeSampleData", WriteSample);
  MUXER_ENTRY("AMediaMuxer_getTrackCount", TrackCount);
  MUXER_ENTRY("AMediaMuxer_getTrackFormat", TrackFormat);
#undef MUXER_ENTRY
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
