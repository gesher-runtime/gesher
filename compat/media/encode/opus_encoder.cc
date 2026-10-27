// Gesher media: Opus through libopus (external/libopus, built with its encoder;
// encoder.h). 20 ms packets at 8, 12, 16, 24 or 48 kHz; the OpusHead (with the
// encoder's look-ahead as the pre-skip) is the codec-config buffer, as
// C2SoftOpusEnc's header.

#include <opus.h>

#include <algorithm>
#include <cstring>

#include "../audio/log.h"
#include "encoder.h"

namespace gesher::media {
namespace {

class OpusEncoderImpl : public AudioEncoder {
 public:
  ~OpusEncoderImpl() override {
    if (enc_ != nullptr) opus_encoder_destroy(enc_);
  }

  std::string Name() const override { return "soft opus (libopus)"; }

  bool Init(const AudioEncoderParams& p, std::string* error) {
    rate_ = p.sample_rate;
    channels_ = p.channels;
    if (rate_ != 8000 && rate_ != 12000 && rate_ != 16000 && rate_ != 24000 && rate_ != 48000) {
      *error = "Opus takes 8000, 12000, 16000, 24000 or 48000 Hz, not " + std::to_string(rate_);
      return false;
    }
    int err = 0;
    enc_ = opus_encoder_create(static_cast<opus_int32>(rate_), static_cast<int>(channels_),
                               OPUS_APPLICATION_AUDIO, &err);
    if (enc_ == nullptr || err != OPUS_OK) {
      *error = std::string("opus_encoder_create: ") + opus_strerror(err);
      enc_ = nullptr;
      return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(std::clamp(p.bitrate, 6000, 510000)));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc_, OPUS_GET_LOOKAHEAD(&lookahead));
    // OpusHead's pre-skip counts 48 kHz samples.
    pre_skip_ = static_cast<uint32_t>(lookahead) * 48000 / rate_;
    frame_ = rate_ / 50;
    return true;
  }

  bool Encode(const int16_t* pcm, size_t frames, int64_t time_us, std::vector<EncodedPacket>* out) override {
    if (!header_sent_) {
      EncodedPacket h;
      h.data = OpusHead(channels_, pre_skip_, rate_);
      h.config = true;
      h.time_us = time_us;
      out->push_back(std::move(h));
      header_sent_ = true;
    }
    if (pending_.empty()) pending_time_us_ = time_us;
    pending_.insert(pending_.end(), pcm, pcm + frames * channels_);
    return Packets(out, false);
  }

  bool Drain(std::vector<EncodedPacket>* out) override { return Packets(out, true); }

  size_t frame_size() const override { return frame_; }
  uint32_t delay() const override { return pre_skip_ * rate_ / 48000; }

 private:
  bool Packets(std::vector<EncodedPacket>* out, bool flush) {
    const size_t per = frame_ * channels_;
    if (flush && !pending_.empty() && pending_.size() % per != 0) {
      pending_.resize((pending_.size() / per + 1) * per, 0);  // the last packet padded with silence
    }
    size_t at = 0;
    while (pending_.size() - at >= per) {
      unsigned char buf[4000];
      opus_int32 n = opus_encode(enc_, pending_.data() + at, static_cast<int>(frame_), buf, sizeof(buf));
      if (n < 0) {
        MEDIA_LOGE("%s: opus_encode %s", Name().c_str(), opus_strerror(n));
        return false;
      }
      EncodedPacket pkt;
      pkt.data.assign(buf, buf + n);
      pkt.time_us = pending_time_us_;
      pkt.key = true;
      out->push_back(std::move(pkt));
      at += per;
      pending_time_us_ += static_cast<int64_t>(frame_) * 1000000 / rate_;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(at));
    return true;
  }

  OpusEncoder* enc_ = nullptr;
  uint32_t rate_ = 48000, channels_ = 1, pre_skip_ = 0;
  size_t frame_ = 960;
  bool header_sent_ = false;
  std::vector<int16_t> pending_;
  int64_t pending_time_us_ = 0;
};

}  // namespace

std::unique_ptr<AudioEncoder> CreateOpusEncoder(const AudioEncoderParams& p, std::string* error) {
  auto e = std::make_unique<OpusEncoderImpl>();
  if (!e->Init(p, error)) return nullptr;
  return e;
}

}  // namespace gesher::media
