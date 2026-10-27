// Gesher media: the Ogg writer (Opus, RFC 7845: OpusHead and OpusTags pages,
// then a page per packet, granule positions at 48 kHz) and the AAC ADTS writer
// (a 7-byte header in front of each access unit), for MediaRecorder's OGG and
// AAC_ADTS outputs (muxer.h; ADR 0059).

#include <errno.h>
#include <unistd.h>

#include <cstring>

#include "../encode/encoder.h"
#include "muxer.h"

namespace gesher::media {
namespace {

uint32_t OggCrc(const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t r = i << 24;
      for (int k = 0; k < 8; k++) r = (r & 0x80000000u) ? (r << 1) ^ 0x04C11DB7u : r << 1;
      table[i] = r;
    }
    init = true;
  }
  uint32_t crc = 0;
  for (size_t i = 0; i < n; i++) crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xff];
  return crc;
}

class OggMuxer : public Muxer {
 public:
  explicit OggMuxer(int fd) : fd_(fd) {}
  ~OggMuxer() override {
    if (fd_ >= 0) close(fd_);
  }

  int AddTrack(const Format& f, std::string* error) override {
    if (have_track_ || f.Mime() != "audio/opus") {
      *error = "Ogg: one Opus track only";
      return -1;
    }
    int32_t v = 0;
    if (f.GetInt32("channel-count", &v)) channels_ = static_cast<uint32_t>(v);
    if (f.GetInt32("sample-rate", &v)) rate_ = static_cast<uint32_t>(v);
    f.GetBuffer("csd-0", &head_);
    have_track_ = true;
    return 0;
  }

  bool Start(std::string*) override {
    started_ = true;
    return true;
  }

  bool WriteSample(int, const uint8_t* data, size_t size, int64_t time_us, int flags,
                   std::string* error) override {
    if (!started_) {
      *error = "not started";
      return false;
    }
    if (flags & 2) {
      if (head_.empty() && size >= 19) head_.assign(data, data + size);
      return true;
    }
    if (!headers_written_ && !WriteHeaders(error)) return false;
    // One packet a page; the granule position counts 48 kHz samples to the packet's end.
    if (first_us_ < 0) first_us_ = time_us;
    const uint64_t samples = PacketSamples(data, size);
    granule_ += samples;
    return Page(data, size, granule_ + pre_skip_, 0, error);
  }

  bool Stop(std::string* error) override {
    if (!headers_written_ && !WriteHeaders(error)) return false;
    // The last page: an empty packet with the end-of-stream flag.
    if (!Page(nullptr, 0, granule_ + pre_skip_, 0x04, error)) return false;
    close(fd_);
    fd_ = -1;
    return true;
  }

  int64_t BytesWritten() const override { return bytes_; }

 private:
  // 48 kHz samples in an Opus packet (its TOC byte, RFC 6716 3.1).
  static uint64_t PacketSamples(const uint8_t* p, size_t n) {
    if (n < 1) return 0;
    const int config = p[0] >> 3;
    static const int kFrame[32] = {480, 960, 1920, 2880, 480, 960, 1920, 2880, 480, 960, 1920,
                                   2880, 480, 960, 480, 960, 120, 240, 480, 960, 120, 240,
                                   480, 960, 120, 240, 480, 960, 120, 240, 480, 960};
    int frames = 1;
    const int code = p[0] & 3;
    if (code == 1 || code == 2) frames = 2;
    if (code == 3 && n >= 2) frames = p[1] & 0x3f;
    return static_cast<uint64_t>(kFrame[config]) * frames;
  }

  bool WriteHeaders(std::string* error) {
    if (head_.size() < 19) head_ = OpusHead(channels_, 312, rate_);
    pre_skip_ = static_cast<uint64_t>(head_[10] | (head_[11] << 8));
    if (!Page(head_.data(), head_.size(), 0, 0x02, error)) return false;
    static const char kVendor[] = "Gesher";
    std::vector<uint8_t> tags = {'O', 'p', 'u', 's', 'T', 'a', 'g', 's', 6, 0, 0, 0};
    tags.insert(tags.end(), kVendor, kVendor + 6);
    tags.insert(tags.end(), {0, 0, 0, 0});
    if (!Page(tags.data(), tags.size(), 0, 0, error)) return false;
    headers_written_ = true;
    return true;
  }

  bool Page(const uint8_t* data, size_t size, uint64_t granule, uint8_t type, std::string* error) {
    std::vector<uint8_t> page = {'O', 'g', 'g', 'S', 0, type};
    for (int i = 0; i < 8; i++) page.push_back(static_cast<uint8_t>(granule >> (8 * i)));
    for (int i = 0; i < 4; i++) page.push_back(static_cast<uint8_t>(serial_ >> (8 * i)));
    for (int i = 0; i < 4; i++) page.push_back(static_cast<uint8_t>(sequence_ >> (8 * i)));
    sequence_++;
    page.insert(page.end(), {0, 0, 0, 0});  // CRC, below
    std::vector<uint8_t> lacing;
    size_t left = size;
    while (left >= 255) {
      lacing.push_back(255);
      left -= 255;
    }
    lacing.push_back(static_cast<uint8_t>(left));
    page.push_back(static_cast<uint8_t>(lacing.size()));
    page.insert(page.end(), lacing.begin(), lacing.end());
    if (size > 0) page.insert(page.end(), data, data + size);
    const uint32_t crc = OggCrc(page.data(), page.size());
    for (int i = 0; i < 4; i++) page[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    if (!WriteAll(fd_, page.data(), page.size())) {
      *error = std::string("Ogg writer: ") + strerror(errno);
      return false;
    }
    bytes_ += static_cast<int64_t>(page.size());
    return true;
  }

  int fd_;
  bool have_track_ = false, started_ = false, headers_written_ = false;
  uint32_t channels_ = 1, rate_ = 48000;
  std::vector<uint8_t> head_;
  uint64_t granule_ = 0, pre_skip_ = 0;
  uint32_t serial_ = 0x47657368, sequence_ = 0;
  int64_t first_us_ = -1;
  int64_t bytes_ = 0;
};

class AdtsMuxer : public Muxer {
 public:
  explicit AdtsMuxer(int fd) : fd_(fd) {}
  ~AdtsMuxer() override {
    if (fd_ >= 0) close(fd_);
  }

  int AddTrack(const Format& f, std::string* error) override {
    if (have_track_ || f.Mime() != "audio/mp4a-latm") {
      *error = "ADTS: one AAC track only";
      return -1;
    }
    int32_t v = 0;
    if (f.GetInt32("channel-count", &v)) channels_ = static_cast<uint32_t>(v);
    if (f.GetInt32("sample-rate", &v)) rate_ = static_cast<uint32_t>(v);
    have_track_ = true;
    return 0;
  }
  bool Start(std::string*) override { return true; }
  bool WriteSample(int, const uint8_t* data, size_t size, int64_t, int flags, std::string* error) override {
    if (flags & 2) return true;
    static const uint32_t kRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                      22050, 16000, 12000, 11025, 8000,  7350};
    int index = 4;
    for (int i = 0; i < 13; i++) {
      if (kRates[i] == rate_) index = i;
    }
    const size_t len = size + 7;
    const uint8_t h[7] = {0xFF, 0xF1, static_cast<uint8_t>((1 << 6) | (index << 2) | (channels_ >> 2)),
                          static_cast<uint8_t>(((channels_ & 3) << 6) | (len >> 11)),
                          static_cast<uint8_t>((len >> 3) & 0xff),
                          static_cast<uint8_t>(((len & 7) << 5) | 0x1F), 0xFC};
    if (!WriteAll(fd_, h, 7) || !WriteAll(fd_, data, size)) {
      *error = std::string("ADTS writer: ") + strerror(errno);
      return false;
    }
    bytes_ += static_cast<int64_t>(len);
    return true;
  }
  bool Stop(std::string*) override {
    close(fd_);
    fd_ = -1;
    return true;
  }
  int64_t BytesWritten() const override { return bytes_; }

 private:
  int fd_;
  bool have_track_ = false;
  uint32_t channels_ = 1, rate_ = 44100;
  int64_t bytes_ = 0;
};

}  // namespace

std::unique_ptr<Muxer> CreateOggMuxer(int fd, std::string*) { return std::make_unique<OggMuxer>(fd); }
std::unique_ptr<Muxer> CreateAdtsMuxer(int fd, std::string*) { return std::make_unique<AdtsMuxer>(fd); }

}  // namespace gesher::media
