// Gesher media: the WebM writer (muxer.h; ADR 0059) over libwebm's mkvmuxer
// (external/libwebm): VP8/VP9 video, Opus and Vorbis audio (CodecPrivate:
// the OpusHead, or Vorbis' three headers laced as Xiph does). Frames of the
// tracks are interleaved by time (each written once every track has one
// waiting, the rest at the end), as mkvmuxer wants them in order; video is
// the cues' track.

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <deque>

#include "mkvmuxer/mkvmuxer.h"

#include "../audio/log.h"
#include "muxer.h"

namespace gesher::media {
namespace {

class FdWriter : public mkvmuxer::IMkvWriter {
 public:
  explicit FdWriter(int fd) : fd_(fd) {}
  ~FdWriter() override {
    if (fd_ >= 0) close(fd_);
  }
  mkvmuxer::int32 Write(const void* buf, mkvmuxer::uint32 len) override {
    if (!WriteAll(fd_, buf, len)) return -1;
    pos_ += len;
    return 0;
  }
  mkvmuxer::int64 Position() const override { return pos_; }
  mkvmuxer::int32 Position(mkvmuxer::int64 position) override {
    if (lseek(fd_, static_cast<off_t>(position), SEEK_SET) < 0) return -1;
    pos_ = position;
    return 0;
  }
  bool Seekable() const override { return true; }
  void ElementStartNotify(mkvmuxer::uint64, mkvmuxer::int64) override {}

 private:
  int fd_;
  int64_t pos_ = 0;
};

struct Pending {
  std::vector<uint8_t> data;
  int64_t time_us;
  bool key;
};

struct TrackState {
  uint64_t number = 0;
  bool video = false;
  std::string mime;
  std::deque<Pending> pending;
  bool has_private = false;
};

class WebmMuxer : public Muxer {
 public:
  explicit WebmMuxer(int fd) : writer_(fd) {}

  int AddTrack(const Format& f, std::string* error) override {
    if (started_) {
      *error = "addTrack after start";
      return -1;
    }
    TrackState t;
    t.mime = f.Mime();
    t.video = t.mime.rfind("video/", 0) == 0;
    int32_t w = 0, h = 0, rate = 0, ch = 0;
    f.GetInt32("width", &w);
    f.GetInt32("height", &h);
    f.GetInt32("sample-rate", &rate);
    f.GetInt32("channel-count", &ch);
    if (!segment_init_) {
      if (!segment_.Init(&writer_)) {
        *error = "WebM: Segment.Init failed";
        return -1;
      }
      segment_.set_mode(mkvmuxer::Segment::kFile);
      segment_.OutputCues(true);
      segment_.GetSegmentInfo()->set_writing_app("Gesher");
      segment_init_ = true;
    }
    if (t.mime == "video/x-vnd.on2.vp8" || t.mime == "video/x-vnd.on2.vp9") {
      t.number = segment_.AddVideoTrack(w, h, 0);
      auto* vt = static_cast<mkvmuxer::VideoTrack*>(segment_.GetTrackByNumber(t.number));
      if (vt == nullptr) {
        *error = "WebM: AddVideoTrack failed";
        return -1;
      }
      vt->set_codec_id(t.mime == "video/x-vnd.on2.vp8" ? mkvmuxer::Tracks::kVp8CodecId
                                                       : mkvmuxer::Tracks::kVp9CodecId);
      segment_.CuesTrack(t.number);
    } else if (t.mime == "audio/opus" || t.mime == "audio/vorbis") {
      t.number = segment_.AddAudioTrack(rate > 0 ? rate : 48000, ch > 0 ? ch : 1, 0);
      auto* at = static_cast<mkvmuxer::AudioTrack*>(segment_.GetTrackByNumber(t.number));
      if (at == nullptr) {
        *error = "WebM: AddAudioTrack failed";
        return -1;
      }
      at->set_bit_depth(16);
      if (t.mime == "audio/opus") {
        at->set_codec_id(mkvmuxer::Tracks::kOpusCodecId);
        std::vector<uint8_t> head, delay, preroll;
        if (f.GetBuffer("csd-0", &head) && !head.empty()) {
          at->SetCodecPrivate(head.data(), head.size());
          t.has_private = true;
        }
        int64_t v = 0;
        if (f.GetBuffer("csd-1", &delay) && delay.size() == 8) {
          memcpy(&v, delay.data(), 8);
          at->set_codec_delay(static_cast<uint64_t>(v));
        }
        if (f.GetBuffer("csd-2", &preroll) && preroll.size() == 8) {
          memcpy(&v, preroll.data(), 8);
          at->set_seek_pre_roll(static_cast<uint64_t>(v));
        } else {
          at->set_seek_pre_roll(80000000);
        }
      } else {
        at->set_codec_id(mkvmuxer::Tracks::kVorbisCodecId);
        // Vorbis' identification, comment and setup headers (csd-0, -1, -2) laced.
        std::vector<uint8_t> h0, h1, h2;
        if (f.GetBuffer("csd-0", &h0) && f.GetBuffer("csd-1", &h1) && f.GetBuffer("csd-2", &h2)) {
          std::vector<uint8_t> priv = {2};
          for (size_t len : {h0.size(), h1.size()}) {
            while (len >= 255) {
              priv.push_back(255);
              len -= 255;
            }
            priv.push_back(static_cast<uint8_t>(len));
          }
          priv.insert(priv.end(), h0.begin(), h0.end());
          priv.insert(priv.end(), h1.begin(), h1.end());
          priv.insert(priv.end(), h2.begin(), h2.end());
          at->SetCodecPrivate(priv.data(), priv.size());
          t.has_private = true;
        }
      }
    } else {
      *error = "WebM: unsupported track type " + t.mime;
      return -1;
    }
    tracks_.push_back(std::move(t));
    return static_cast<int>(tracks_.size() - 1);
  }

  bool Start(std::string* error) override {
    if (tracks_.empty()) {
      *error = "no tracks";
      return false;
    }
    started_ = true;
    return true;
  }

  bool WriteSample(int index, const uint8_t* data, size_t size, int64_t time_us, int flags,
                   std::string* error) override {
    if (!started_ || index < 0 || static_cast<size_t>(index) >= tracks_.size()) {
      *error = "writeSampleData: not started or a bad track";
      return false;
    }
    TrackState& t = tracks_[static_cast<size_t>(index)];
    if (flags & 2) {
      // Codec config: Opus' header as the CodecPrivate where the format had none.
      if (!t.has_private && t.mime == "audio/opus") {
        auto* at = static_cast<mkvmuxer::AudioTrack*>(segment_.GetTrackByNumber(t.number));
        if (at != nullptr && size >= 19 && memcmp(data, "OpusHead", 8) == 0) {
          at->SetCodecPrivate(data, size);
          t.has_private = true;
        }
      }
      return true;
    }
    if (first_us_ < 0 || time_us < first_us_) first_us_ = first_us_ < 0 ? time_us : first_us_;
    t.pending.push_back({std::vector<uint8_t>(data, data + size), time_us, t.video ? (flags & 1) != 0 : true});
    bytes_ += static_cast<int64_t>(size);
    return Flush(false, error);
  }

  bool Stop(std::string* error) override {
    if (!started_) {
      *error = "stop before start";
      return false;
    }
    if (!Flush(true, error)) return false;
    started_ = false;
    if (!segment_.Finalize()) {
      *error = "WebM: Finalize failed";
      return false;
    }
    return true;
  }

  int64_t BytesWritten() const override { return bytes_; }

 private:
  // Writes the earliest waiting frames: while every track has one (or all, at the end).
  bool Flush(bool all, std::string* error) {
    for (;;) {
      TrackState* best = nullptr;
      for (TrackState& t : tracks_) {
        if (t.pending.empty()) {
          if (!all) return true;
          continue;
        }
        if (best == nullptr || t.pending.front().time_us < best->pending.front().time_us) best = &t;
      }
      if (best == nullptr) return true;
      Pending p = std::move(best->pending.front());
      best->pending.pop_front();
      if (base_us_ < 0) base_us_ = p.time_us;
      const int64_t t_ns = std::max<int64_t>(p.time_us - base_us_, last_ns_ / 1000) * 1000;
      last_ns_ = t_ns;
      if (!segment_.AddFrame(p.data.data(), p.data.size(), best->number, static_cast<uint64_t>(t_ns), p.key)) {
        *error = "WebM: AddFrame failed";
        return false;
      }
    }
  }

  FdWriter writer_;
  mkvmuxer::Segment segment_;
  bool segment_init_ = false;
  bool started_ = false;
  std::vector<TrackState> tracks_;
  int64_t first_us_ = -1, base_us_ = -1, last_ns_ = 0;
  int64_t bytes_ = 0;
};

}  // namespace

std::unique_ptr<Muxer> CreateWebmMuxer(int fd, std::string* error) {
  return std::make_unique<WebmMuxer>(fd);
}

}  // namespace gesher::media
