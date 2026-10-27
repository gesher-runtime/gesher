// Gesher media: the MPEG-4 (and 3GPP) writer (muxer.h; ADR 0059).
//
// ftyp, then one mdat the samples go into as they come (its 64-bit size
// written at the end), then the moov: per track tkhd, mdhd, hdlr and the
// sample tables (stts from the samples' times, stss for video, stsz, one sample
// a chunk in co64), the sample description (avc1 + avcC from the SPS and PPS,
// mp4a + esds from the AudioSpecificConfig, Opus + dOps from the OpusHead), and
// an edit list where a track starts after the first.

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <media/NdkMediaFormat.h>

#include "../audio/log.h"
#include "../encode/codec_config.h"
#include "muxer.h"

namespace gesher::media {

bool WriteAll(int fd, const void* data, size_t size) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (size > 0) {
    ssize_t n = write(fd, p, size);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    size -= static_cast<size_t>(n);
  }
  return true;
}

namespace {

class Box {
 public:
  explicit Box(const char* type) { Begin(type); }
  Box() = default;

  void Begin(const char* type) {
    starts_.push_back(b_.size());
    U32(0);
    b_.insert(b_.end(), type, type + 4);
  }
  void FullBegin(const char* type, uint8_t version, uint32_t flags) {
    Begin(type);
    U32((static_cast<uint32_t>(version) << 24) | flags);
  }
  void End() {
    const size_t start = starts_.back();
    starts_.pop_back();
    const uint32_t size = static_cast<uint32_t>(b_.size() - start);
    b_[start] = static_cast<uint8_t>(size >> 24);
    b_[start + 1] = static_cast<uint8_t>(size >> 16);
    b_[start + 2] = static_cast<uint8_t>(size >> 8);
    b_[start + 3] = static_cast<uint8_t>(size);
  }
  void U8(uint8_t v) { b_.push_back(v); }
  void U16(uint16_t v) {
    U8(static_cast<uint8_t>(v >> 8));
    U8(static_cast<uint8_t>(v));
  }
  void U24(uint32_t v) {
    U8(static_cast<uint8_t>(v >> 16));
    U16(static_cast<uint16_t>(v));
  }
  void U32(uint32_t v) {
    U16(static_cast<uint16_t>(v >> 16));
    U16(static_cast<uint16_t>(v));
  }
  void U64(uint64_t v) {
    U32(static_cast<uint32_t>(v >> 32));
    U32(static_cast<uint32_t>(v));
  }
  void Bytes(const void* p, size_t n) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    b_.insert(b_.end(), q, q + n);
  }
  void Zeros(size_t n) { b_.insert(b_.end(), n, 0); }
  // An MPEG-4 descriptor: tag, a 4-byte length, then what `body` writes.
  template <typename F>
  void Descriptor(uint8_t tag, F body) {
    U8(tag);
    const size_t at = b_.size();
    U32(0);
    const size_t start = b_.size();
    body();
    const uint32_t len = static_cast<uint32_t>(b_.size() - start);
    b_[at] = static_cast<uint8_t>(0x80 | ((len >> 21) & 0x7f));
    b_[at + 1] = static_cast<uint8_t>(0x80 | ((len >> 14) & 0x7f));
    b_[at + 2] = static_cast<uint8_t>(0x80 | ((len >> 7) & 0x7f));
    b_[at + 3] = static_cast<uint8_t>(len & 0x7f);
  }
  void Matrix(int degrees) {
    // a, b, u, c, d, v, x, y, w (16.16, 2.30 for u, v, w).
    int32_t a = 0x10000, b = 0, c = 0, d = 0x10000;
    if (degrees == 90) {
      a = 0; b = 0x10000; c = -0x10000; d = 0;
    } else if (degrees == 180) {
      a = -0x10000; d = -0x10000;
    } else if (degrees == 270) {
      a = 0; b = -0x10000; c = 0x10000; d = 0;
    }
    for (int32_t v : {a, b, 0, c, d, 0, 0, 0, 0x40000000}) U32(static_cast<uint32_t>(v));
  }
  const std::vector<uint8_t>& bytes() const { return b_; }

 private:
  std::vector<uint8_t> b_;
  std::vector<size_t> starts_;
};

struct Track {
  std::string mime;
  bool video = false;
  int width = 0, height = 0;
  uint32_t rate = 0, channels = 0;
  std::vector<std::vector<uint8_t>> csd;
  std::vector<uint32_t> sizes;
  std::vector<uint64_t> offsets;
  std::vector<int64_t> times;  // us
  std::vector<uint32_t> keys;  // 1-based
  uint32_t timescale = 90000;
};

std::vector<uint8_t> StripStartCode(const std::vector<uint8_t>& nal) {
  size_t i = 0;
  while (i < nal.size() && nal[i] == 0) i++;
  if (i < nal.size() && nal[i] == 1 && i >= 2) return {nal.begin() + static_cast<ptrdiff_t>(i + 1), nal.end()};
  return nal;
}

bool StartsWithStartCode(const uint8_t* p, size_t n) {
  return (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) || (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1);
}

class Mp4Muxer : public Muxer {
 public:
  Mp4Muxer(int fd, bool three_gpp) : fd_(fd), three_gpp_(three_gpp) {}
  ~Mp4Muxer() override {
    if (fd_ >= 0) close(fd_);
  }

  int AddTrack(const Format& f, std::string* error) override {
    if (started_) {
      *error = "addTrack after start";
      return -1;
    }
    Track t;
    t.mime = f.Mime();
    t.video = t.mime.rfind("video/", 0) == 0;
    if (t.mime != "video/avc" && t.mime != "audio/mp4a-latm" && t.mime != "audio/opus") {
      *error = "MPEG-4: unsupported track type " + t.mime;
      return -1;
    }
    int32_t v = 0;
    if (f.GetInt32("width", &v)) t.width = v;
    if (f.GetInt32("height", &v)) t.height = v;
    if (f.GetInt32("sample-rate", &v)) t.rate = static_cast<uint32_t>(v);
    if (f.GetInt32("channel-count", &v)) t.channels = static_cast<uint32_t>(v);
    for (const char* key : {"csd-0", "csd-1"}) {
      std::vector<uint8_t> b;
      if (f.GetBuffer(key, &b) && !b.empty()) t.csd.push_back(b);
    }
    t.timescale = t.video ? 90000 : (t.mime == "audio/opus" ? 48000 : std::max<uint32_t>(t.rate, 8000));
    tracks_.push_back(std::move(t));
    return static_cast<int>(tracks_.size() - 1);
  }

  bool Start(std::string* error) override {
    if (tracks_.empty()) {
      *error = "no tracks";
      return false;
    }
    Box ftyp("ftyp");
    if (three_gpp_) {
      ftyp.Bytes("3gp4", 4);
      ftyp.U32(0);
      ftyp.Bytes("isom3gp43gp5", 12);
    } else {
      ftyp.Bytes("isom", 4);
      ftyp.U32(0x200);
      ftyp.Bytes("isomiso2avc1mp41", 16);
    }
    ftyp.End();
    // mdat with a 64-bit size, filled in at the end.
    static const uint8_t kMdat[16] = {0, 0, 0, 1, 'm', 'd', 'a', 't', 0, 0, 0, 0, 0, 0, 0, 0};
    if (!WriteAll(fd_, ftyp.bytes().data(), ftyp.bytes().size())) return Fail(error);
    mdat_at_ = ftyp.bytes().size();
    if (!WriteAll(fd_, kMdat, sizeof(kMdat))) return Fail(error);
    pos_ = mdat_at_ + sizeof(kMdat);
    started_ = true;
    return true;
  }

  bool WriteSample(int index, const uint8_t* data, size_t size, int64_t time_us, int flags,
                   std::string* error) override {
    if (!started_ || index < 0 || static_cast<size_t>(index) >= tracks_.size()) {
      *error = "writeSampleData: not started or a bad track";
      return false;
    }
    Track& t = tracks_[static_cast<size_t>(index)];
    if (flags & 2) {
      // Codec config: the track's csd where its format had none.
      if (t.csd.empty()) {
        if (t.mime == "video/avc") {
          for (auto [nal, len] : SplitAnnexB(data, size)) {
            if (len > 0 && ((nal[0] & 0x1f) == 7 || (nal[0] & 0x1f) == 8)) t.csd.emplace_back(nal, nal + len);
          }
        } else {
          t.csd.emplace_back(data, data + size);
        }
      }
      return true;
    }
    std::vector<uint8_t> converted;
    const uint8_t* p = data;
    size_t n = size;
    if (t.mime == "video/avc" && StartsWithStartCode(data, size)) {
      // Annex B to 4-byte lengths.
      for (auto [nal, len] : SplitAnnexB(data, size)) {
        if (len == 0) continue;
        const int type = nal[0] & 0x1f;
        if (type == 9) continue;
        if ((type == 7 || type == 8) && t.csd.size() < 2) {
          t.csd.emplace_back(nal, nal + len);
        }
        const uint32_t l = static_cast<uint32_t>(len);
        const uint8_t be[4] = {static_cast<uint8_t>(l >> 24), static_cast<uint8_t>(l >> 16),
                               static_cast<uint8_t>(l >> 8), static_cast<uint8_t>(l)};
        converted.insert(converted.end(), be, be + 4);
        converted.insert(converted.end(), nal, nal + len);
      }
      p = converted.data();
      n = converted.size();
    }
    if (!WriteAll(fd_, p, n)) return Fail(error);
    t.offsets.push_back(pos_);
    t.sizes.push_back(static_cast<uint32_t>(n));
    t.times.push_back(time_us);
    if (!t.video || (flags & 1)) t.keys.push_back(static_cast<uint32_t>(t.sizes.size()));
    pos_ += n;
    return true;
  }

  bool Stop(std::string* error) override {
    if (!started_) {
      *error = "stop before start";
      return false;
    }
    started_ = false;
    // The mdat's size.
    const uint64_t mdat_size = pos_ - mdat_at_;
    uint8_t be[8];
    for (int i = 0; i < 8; i++) be[i] = static_cast<uint8_t>(mdat_size >> (56 - 8 * i));
    if (pwrite(fd_, be, 8, static_cast<off_t>(mdat_at_ + 8)) != 8) return Fail(error);
    std::vector<uint8_t> moov = Moov();
    if (lseek(fd_, static_cast<off_t>(pos_), SEEK_SET) < 0) return Fail(error);
    if (!WriteAll(fd_, moov.data(), moov.size())) return Fail(error);
    pos_ += moov.size();
    close(fd_);
    fd_ = -1;
    return true;
  }

  void SetOrientationHint(int degrees) override { rotation_ = degrees; }
  int64_t BytesWritten() const override { return static_cast<int64_t>(pos_); }

 private:
  bool Fail(std::string* error) {
    *error = std::string("MPEG-4 writer: ") + strerror(errno);
    return false;
  }

  // The duration of each sample in the track's timescale (the last one as the one before).
  std::vector<uint32_t> Durations(const Track& t) const {
    std::vector<uint32_t> d;
    const size_t n = t.times.size();
    uint32_t fallback = t.video ? t.timescale / 30 : (t.mime == "audio/opus" ? 960 : 1024);
    for (size_t i = 0; i + 1 < n; i++) {
      int64_t delta = t.times[i + 1] - t.times[i];
      uint32_t v = delta > 0 ? static_cast<uint32_t>(delta * t.timescale / 1000000) : 1;
      d.push_back(std::max<uint32_t>(v, 1));
    }
    if (n > 0) d.push_back(d.empty() ? fallback : d.back());
    return d;
  }

  std::vector<uint8_t> Moov() const {
    int64_t start = INT64_MAX;
    for (const Track& t : tracks_) {
      if (!t.times.empty()) start = std::min(start, t.times.front());
    }
    if (start == INT64_MAX) start = 0;
    // The movie's duration (ms).
    uint64_t movie_ms = 0;
    std::vector<std::vector<uint32_t>> durations;
    for (const Track& t : tracks_) {
      durations.push_back(Durations(t));
      uint64_t total = 0;
      for (uint32_t d : durations.back()) total += d;
      const int64_t offset_us = t.times.empty() ? 0 : t.times.front() - start;
      movie_ms = std::max<uint64_t>(movie_ms, total * 1000 / t.timescale + static_cast<uint64_t>(offset_us / 1000));
    }
    Box m("moov");
    m.FullBegin("mvhd", 0, 0);
    m.U32(0);
    m.U32(0);
    m.U32(1000);
    m.U32(static_cast<uint32_t>(movie_ms));
    m.U32(0x00010000);
    m.U16(0x0100);
    m.Zeros(10);
    m.Matrix(0);
    m.Zeros(24);
    m.U32(static_cast<uint32_t>(tracks_.size() + 1));
    m.End();
    for (size_t i = 0; i < tracks_.size(); i++) {
      Trak(&m, tracks_[i], static_cast<uint32_t>(i + 1), durations[i], start);
    }
    m.End();
    return m.bytes();
  }

  void Trak(Box* b, const Track& t, uint32_t id, const std::vector<uint32_t>& dur, int64_t start) const {
    uint64_t total = 0;
    for (uint32_t d : dur) total += d;
    const uint64_t track_ms = total * 1000 / t.timescale;
    const int64_t offset_us = t.times.empty() ? 0 : t.times.front() - start;
    b->Begin("trak");
    b->FullBegin("tkhd", 0, 7);
    b->U32(0);
    b->U32(0);
    b->U32(id);
    b->U32(0);
    b->U32(static_cast<uint32_t>(track_ms + offset_us / 1000));
    b->Zeros(8);
    b->U16(0);
    b->U16(t.video ? 0 : 1);
    b->U16(t.video ? 0 : 0x0100);
    b->U16(0);
    b->Matrix(t.video ? rotation_ : 0);
    b->U32(static_cast<uint32_t>(t.width) << 16);
    b->U32(static_cast<uint32_t>(t.height) << 16);
    b->End();
    if (offset_us > 0) {
      // An empty edit for the time before this track's first sample.
      b->Begin("edts");
      b->FullBegin("elst", 0, 0);
      b->U32(2);
      b->U32(static_cast<uint32_t>(offset_us / 1000));
      b->U32(0xFFFFFFFF);
      b->U32(0x00010000);
      b->U32(static_cast<uint32_t>(track_ms));
      b->U32(0);
      b->U32(0x00010000);
      b->End();
      b->End();
    }
    b->Begin("mdia");
    b->FullBegin("mdhd", 0, 0);
    b->U32(0);
    b->U32(0);
    b->U32(t.timescale);
    b->U32(static_cast<uint32_t>(total));
    b->U16(0x55C4);  // "und"
    b->U16(0);
    b->End();
    b->FullBegin("hdlr", 0, 0);
    b->U32(0);
    b->Bytes(t.video ? "vide" : "soun", 4);
    b->Zeros(12);
    const char* name = t.video ? "VideoHandle" : "SoundHandle";
    b->Bytes(name, strlen(name) + 1);
    b->End();
    b->Begin("minf");
    if (t.video) {
      b->FullBegin("vmhd", 0, 1);
      b->Zeros(8);
      b->End();
    } else {
      b->FullBegin("smhd", 0, 0);
      b->Zeros(4);
      b->End();
    }
    b->Begin("dinf");
    b->FullBegin("dref", 0, 0);
    b->U32(1);
    b->FullBegin("url ", 0, 1);
    b->End();
    b->End();
    b->End();
    b->Begin("stbl");
    b->FullBegin("stsd", 0, 0);
    b->U32(1);
    SampleEntry(b, t);
    b->End();
    // stts: runs of equal durations.
    b->FullBegin("stts", 0, 0);
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t d : dur) {
      if (!runs.empty() && runs.back().second == d) {
        runs.back().first++;
      } else {
        runs.emplace_back(1, d);
      }
    }
    b->U32(static_cast<uint32_t>(runs.size()));
    for (auto [count, d] : runs) {
      b->U32(count);
      b->U32(d);
    }
    b->End();
    if (t.video) {
      b->FullBegin("stss", 0, 0);
      b->U32(static_cast<uint32_t>(t.keys.size()));
      for (uint32_t k : t.keys) b->U32(k);
      b->End();
    }
    b->FullBegin("stsc", 0, 0);
    b->U32(t.sizes.empty() ? 0 : 1);
    if (!t.sizes.empty()) {
      b->U32(1);
      b->U32(1);
      b->U32(1);
    }
    b->End();
    b->FullBegin("stsz", 0, 0);
    b->U32(0);
    b->U32(static_cast<uint32_t>(t.sizes.size()));
    for (uint32_t s : t.sizes) b->U32(s);
    b->End();
    b->FullBegin("co64", 0, 0);
    b->U32(static_cast<uint32_t>(t.offsets.size()));
    for (uint64_t o : t.offsets) b->U64(o);
    b->End();
    b->End();  // stbl
    b->End();  // minf
    b->End();  // mdia
    b->End();  // trak
  }

  void SampleEntry(Box* b, const Track& t) const {
    if (t.mime == "video/avc") {
      b->Begin("avc1");
      b->Zeros(6);
      b->U16(1);
      b->Zeros(16);
      b->U16(static_cast<uint16_t>(t.width));
      b->U16(static_cast<uint16_t>(t.height));
      b->U32(0x00480000);
      b->U32(0x00480000);
      b->U32(0);
      b->U16(1);
      b->Zeros(32);
      b->U16(0x0018);
      b->U16(0xFFFF);
      std::vector<uint8_t> sps, pps;
      for (const auto& c : t.csd) {
        std::vector<uint8_t> nal = StripStartCode(c);
        // A csd may hold both (Annex B): split it.
        for (auto [p, len] : SplitAnnexB(c.data(), c.size())) {
          if (len == 0) continue;
          if ((p[0] & 0x1f) == 7 && sps.empty()) sps.assign(p, p + len);
          if ((p[0] & 0x1f) == 8 && pps.empty()) pps.assign(p, p + len);
        }
        if (!nal.empty() && (nal[0] & 0x1f) == 7 && sps.empty()) sps = nal;
        if (!nal.empty() && (nal[0] & 0x1f) == 8 && pps.empty()) pps = nal;
      }
      b->Begin("avcC");
      b->U8(1);
      b->U8(sps.size() > 1 ? sps[1] : 66);
      b->U8(sps.size() > 2 ? sps[2] : 0);
      b->U8(sps.size() > 3 ? sps[3] : 30);
      b->U8(0xFF);
      b->U8(0xE1);
      b->U16(static_cast<uint16_t>(sps.size()));
      b->Bytes(sps.data(), sps.size());
      b->U8(1);
      b->U16(static_cast<uint16_t>(pps.size()));
      b->Bytes(pps.data(), pps.size());
      b->End();
      b->End();
      return;
    }
    const bool opus = t.mime == "audio/opus";
    b->Begin(opus ? "Opus" : "mp4a");
    b->Zeros(6);
    b->U16(1);
    b->Zeros(8);
    b->U16(static_cast<uint16_t>(std::max<uint32_t>(t.channels, 1)));
    b->U16(16);
    b->U16(0);
    b->U16(0);
    b->U32((opus ? 48000u : t.rate) << 16);
    if (opus) {
      // dOps from the OpusHead (little-endian there, big-endian here).
      std::vector<uint8_t> head = t.csd.empty() ? OpusHead(t.channels, 312, t.rate) : t.csd[0];
      if (head.size() < 19) head = OpusHead(t.channels, 312, t.rate);
      b->Begin("dOps");
      b->U8(0);
      b->U8(head[9]);
      b->U16(static_cast<uint16_t>(head[10] | (head[11] << 8)));
      b->U32(static_cast<uint32_t>(head[12] | (head[13] << 8) | (head[14] << 16) | (head[15] << 24)));
      b->U16(static_cast<uint16_t>(head[16] | (head[17] << 8)));
      b->U8(0);
      b->End();
    } else {
      const std::vector<uint8_t> asc = t.csd.empty() ? AacAudioSpecificConfig(2, t.rate, t.channels) : t.csd[0];
      b->FullBegin("esds", 0, 0);
      b->Descriptor(3, [&] {
        b->U16(0);
        b->U8(0);
        b->Descriptor(4, [&] {
          b->U8(0x40);  // MPEG-4 audio
          b->U8(0x15);  // audio stream
          b->U24(0);
          b->U32(0);
          b->U32(0);
          b->Descriptor(5, [&] { b->Bytes(asc.data(), asc.size()); });
        });
        b->Descriptor(6, [&] { b->U8(2); });
      });
      b->End();
    }
    b->End();
  }

  int fd_;
  bool three_gpp_;
  bool started_ = false;
  std::vector<Track> tracks_;
  uint64_t mdat_at_ = 0, pos_ = 0;
  int rotation_ = 0;
};

}  // namespace

std::unique_ptr<Muxer> CreateMp4Muxer(int fd, bool three_gpp, std::string* error) {
  return std::make_unique<Mp4Muxer>(fd, three_gpp);
}

std::unique_ptr<Muxer> Muxer::Create(int fd, MuxerFormat format, std::string* error) {
  int copy = dup(fd);
  if (copy < 0) {
    *error = std::string("dup: ") + strerror(errno);
    return nullptr;
  }
  // From the start of the file, whatever the descriptor's offset.
  lseek(copy, 0, SEEK_SET);
  switch (format) {
    case MuxerFormat::kMpeg4: return CreateMp4Muxer(copy, false, error);
    case MuxerFormat::kThreeGpp: return CreateMp4Muxer(copy, true, error);
    case MuxerFormat::kWebm: return CreateWebmMuxer(copy, error);
    case MuxerFormat::kOgg: return CreateOggMuxer(copy, error);
    case MuxerFormat::kAdts: return CreateAdtsMuxer(copy, error);
  }
  close(copy);
  *error = "unknown output format";
  return nullptr;
}

}  // namespace gesher::media
