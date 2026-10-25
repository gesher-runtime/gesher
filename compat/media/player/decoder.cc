// Gesher media: audio decoders (decoder.h). The codec-specific handling
// follows Codec2's software components (frameworks/av media/codec2/components:
// C2SoftMp3Dec, C2SoftVorbisDec, C2SoftOpusDec), which Android uses with the
// same libraries.

#include "decoder.h"

#include <media/stagefright/foundation/OpusHeader.h>
#include <opus.h>
#include <opus_multistream.h>

#include <algorithm>
#include <cstring>

#include "../audio/log.h"
#include "pvmp3decoder_api.h"

extern "C" {
#include <Tremolo/codec_internal.h>
int _vorbis_unpack_books(vorbis_info* vi, oggpack_buffer* opb);
int _vorbis_unpack_info(vorbis_info* vi, oggpack_buffer* opb);
}

namespace gesher::media {
namespace {

void Append(std::vector<uint8_t>* out, const void* data, size_t bytes) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  out->insert(out->end(), p, p + bytes);
}

// --- PCM as the extractors give it (WAV, FLAC) -------------------------------------------

class RawDecoder : public AudioDecoder {
 public:
  explicit RawDecoder(PcmInfo info) : info_(info) {}
  PcmInfo info() const override { return info_; }
  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    Append(out, in.data.data(), in.data.size());
    return true;
  }

 private:
  PcmInfo info_;
};

// --- G.711 (WAV) -------------------------------------------------------------------------

class G711Decoder : public AudioDecoder {
 public:
  G711Decoder(uint32_t rate, uint32_t channels, bool mu) : mu_(mu) {
    info_ = {rate, channels, PcmFormat::kS16};
  }
  PcmInfo info() const override { return info_; }
  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    size_t at = out->size();
    out->resize(at + in.data.size() * 2);
    int16_t* o = reinterpret_cast<int16_t*>(out->data() + at);
    for (size_t i = 0; i < in.data.size(); i++) o[i] = mu_ ? Mu(in.data[i]) : A(in.data[i]);
    return true;
  }

 private:
  static int16_t Mu(uint8_t u) {
    u = ~u;
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return static_cast<int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
  }
  static int16_t A(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) {
      t += 8;
    } else if (seg == 1) {
      t += 0x108;
    } else {
      t += 0x108;
      t <<= seg - 1;
    }
    return static_cast<int16_t>((a & 0x80) ? t : -t);
  }
  PcmInfo info_;
  bool mu_;
};

// --- MP3 (libstagefright_mp3dec) ---------------------------------------------------------

constexpr int kMp3DecoderDelay = 529;  // samples (C2SoftMp3Dec's kPVMP3DecoderDelay)

class Mp3Decoder : public AudioDecoder {
 public:
  Mp3Decoder(uint32_t rate, uint32_t channels) {
    info_ = {rate, channels, PcmFormat::kS16};
    config_ = {};
    config_.equalizerType = flat;
    config_.crcEnabled = false;
    mem_ = malloc(pvmp3_decoderMemRequirements());
    pvmp3_InitDecoder(&config_, mem_);
  }
  ~Mp3Decoder() override { free(mem_); }

  PcmInfo info() const override { return info_; }

  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    size_t pos = 0;
    std::vector<int16_t> pcm(4608);
    while (pos < in.data.size()) {
      config_.pInputBuffer = const_cast<uint8_t*>(in.data.data() + pos);
      config_.inputBufferCurrentLength = static_cast<int32_t>(in.data.size() - pos);
      config_.inputBufferMaxLength = 0;
      config_.inputBufferUsedLength = 0;
      config_.outputFrameSize = static_cast<int32_t>(pcm.size());
      config_.pOutputBuffer = pcm.data();
      ERROR_CODE err = pvmp3_framedecoder(&config_, mem_);
      if (err != NO_DECODING_ERROR) {
        if (err != NO_ENOUGH_MAIN_DATA_ERROR && err != SIDE_INFO_ERROR) {
          MEDIA_LOGE("mp3: decoder error %d", static_cast<int>(err));
          return false;
        }
        // Recoverable: silence for this frame (as C2SoftMp3Dec).
        if (config_.outputFrameSize == 0) break;
        std::fill(pcm.begin(), pcm.begin() + config_.outputFrameSize, 0);
      }
      if (config_.inputBufferUsedLength <= 0) break;
      size_t frames = static_cast<size_t>(config_.outputFrameSize) / std::max(info_.channels, 1u);
      size_t skip = std::min<size_t>(frames, to_skip_);
      to_skip_ -= skip;
      Append(out, pcm.data() + skip * info_.channels, (frames - skip) * info_.channels * 2);
      pos += static_cast<size_t>(config_.inputBufferUsedLength);
    }
    return true;
  }

  void Flush(int64_t) override {
    pvmp3_InitDecoder(&config_, mem_);
    to_skip_ = kMp3DecoderDelay;
  }

  void Drain(std::vector<uint8_t>* out) override {
    // The decoder's delay, as C2SoftMp3Dec adds it at the end.
    std::vector<int16_t> zeros(static_cast<size_t>(kMp3DecoderDelay) * info_.channels, 0);
    Append(out, zeros.data(), zeros.size() * 2);
  }

 private:
  PcmInfo info_;
  tPVMP3DecoderExternal config_;
  void* mem_;
  size_t to_skip_ = kMp3DecoderDelay;
};

// --- Vorbis (Tremolo) --------------------------------------------------------------------

constexpr int kVorbisMaxSamples = 8192;

void MakeBitReader(const void* data, size_t size, ogg_buffer* buf, ogg_reference* ref,
                   oggpack_buffer* bits) {
  buf->data = static_cast<uint8_t*>(const_cast<void*>(data));
  buf->size = size;
  buf->refcount = 1;
  buf->ptr.owner = nullptr;
  ref->buffer = buf;
  ref->begin = 0;
  ref->length = size;
  ref->next = nullptr;
  oggpack_readinit(bits, ref);
}

class VorbisDecoder : public AudioDecoder {
 public:
  ~VorbisDecoder() override {
    vorbis_dsp_clear(&state_);
    vorbis_info_clear(&vi_);
  }

  bool Init(const std::vector<uint8_t>& ident, const std::vector<uint8_t>& setup, std::string* error) {
    vorbis_info_init(&vi_);
    state_ = {};
    if (ident.size() < 8 || ident[0] != 1 || memcmp(&ident[1], "vorbis", 6) != 0 ||
        setup.size() < 8 || setup[0] != 5 || memcmp(&setup[1], "vorbis", 6) != 0) {
      *error = "vorbis: bad headers";
      return false;
    }
    ogg_buffer buf;
    ogg_reference ref;
    oggpack_buffer bits;
    MakeBitReader(ident.data() + 7, ident.size() - 7, &buf, &ref, &bits);
    if (_vorbis_unpack_info(&vi_, &bits) != 0) {
      *error = "vorbis: bad identification header";
      return false;
    }
    MakeBitReader(setup.data() + 7, setup.size() - 7, &buf, &ref, &bits);
    if (_vorbis_unpack_books(&vi_, &bits) != 0 || vorbis_dsp_init(&state_, &vi_) != 0) {
      *error = "vorbis: bad setup header";
      return false;
    }
    info_ = {static_cast<uint32_t>(vi_.rate), static_cast<uint32_t>(vi_.channels), PcmFormat::kS16};
    return true;
  }

  PcmInfo info() const override { return info_; }

  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    if (in.valid_samples >= 0) frames_left_on_page_ = in.valid_samples;
    ogg_buffer buf;
    buf.data = const_cast<uint8_t*>(in.data.data());
    buf.size = in.data.size();
    buf.refcount = 1;
    buf.ptr.owner = nullptr;
    ogg_reference ref;
    ref.buffer = &buf;
    ref.begin = 0;
    ref.length = buf.size;
    ref.next = nullptr;
    ogg_packet pack = {};
    pack.packet = &ref;
    pack.bytes = static_cast<long>(ref.length);
    std::vector<int16_t> pcm(static_cast<size_t>(kVorbisMaxSamples) * info_.channels);
    int frames = 0;
    if (vorbis_dsp_synthesis(&state_, &pack, 1) == 0) {
      frames = vorbis_dsp_pcmout(&state_, pcm.data(), kVorbisMaxSamples);
      if (frames < 0) frames = 0;
    }
    if (frames_left_on_page_ >= 0) {
      frames = std::min(frames, frames_left_on_page_);
      frames_left_on_page_ -= frames;
    }
    Append(out, pcm.data(), static_cast<size_t>(frames) * info_.channels * 2);
    return true;
  }

  void Flush(int64_t) override {
    vorbis_dsp_restart(&state_);
    frames_left_on_page_ = -1;
  }

 private:
  vorbis_info vi_ = {};
  vorbis_dsp_state state_ = {};
  PcmInfo info_;
  int frames_left_on_page_ = -1;
};

// --- Opus (libopus) ----------------------------------------------------------------------

constexpr int kOpusRate = 48000;
constexpr int kOpusMaxPacketSamples = 960 * 6;

uint64_t NsToSamples(uint64_t ns) { return static_cast<uint64_t>(static_cast<double>(ns) * kOpusRate / 1e9); }

class OpusDecoder : public AudioDecoder {
 public:
  ~OpusDecoder() override {
    if (dec_ != nullptr) opus_multistream_decoder_destroy(dec_);
  }

  bool Init(const Format& f, std::string* error) {
    std::vector<uint8_t> csd0, csd1, csd2;
    f.GetBuffer("csd-0", &csd0);
    const uint8_t* head = csd0.data();
    size_t head_size = csd0.size();
    void *hb = nullptr, *db = nullptr, *pb = nullptr;
    size_t hs = 0, ds = 0, ps = 0;
    // A unified CSD (all three in csd-0, with markers), or one each.
    if (android::GetOpusHeaderBuffers(csd0.data(), csd0.size(), &hb, &hs, &db, &ds, &pb, &ps)) {
      head = static_cast<const uint8_t*>(hb);
      head_size = hs;
      if (db != nullptr && ds == 8) codec_delay_ = NsToSamples(Read64(db));
      if (pb != nullptr && ps == 8) seek_preroll_ = NsToSamples(Read64(pb));
    }
    if (f.GetBuffer("csd-1", &csd1) && csd1.size() >= 8) codec_delay_ = NsToSamples(Read64(csd1.data()));
    if (f.GetBuffer("csd-2", &csd2) && csd2.size() >= 8) seek_preroll_ = NsToSamples(Read64(csd2.data()));
    if (!android::ParseOpusHeader(head, head_size, &header_)) {
      *error = "opus: bad OpusHead";
      return false;
    }
    uint8_t mapping[8] = {0, 1};
    if (header_.channels > 2) memcpy(mapping, header_.stream_map, static_cast<size_t>(header_.channels));
    int status = OPUS_INVALID_STATE;
    dec_ = opus_multistream_decoder_create(kOpusRate, header_.channels, header_.num_streams,
                                           header_.num_coupled, mapping, &status);
    if (dec_ == nullptr || status != OPUS_OK) {
      *error = std::string("opus: ") + opus_strerror(status);
      return false;
    }
    opus_multistream_decoder_ctl(dec_, OPUS_SET_GAIN(header_.gain_db));
    to_discard_ = codec_delay_;
    info_ = {kOpusRate, static_cast<uint32_t>(header_.channels), PcmFormat::kS16};
    return true;
  }

  PcmInfo info() const override { return info_; }

  bool Decode(const Sample& in, std::vector<uint8_t>* out) override {
    if (in.time_us == 0) to_discard_ = codec_delay_;
    std::vector<int16_t> pcm(static_cast<size_t>(kOpusMaxPacketSamples) * info_.channels);
    int n = opus_multistream_decode(dec_, in.data.data(), static_cast<opus_int32>(in.data.size()),
                                    pcm.data(), kOpusMaxPacketSamples, 0);
    if (n < 0) {
      MEDIA_LOGE("opus: %s", opus_strerror(n));
      return false;
    }
    size_t skip = static_cast<size_t>(std::min<uint64_t>(to_discard_, static_cast<uint64_t>(n)));
    to_discard_ -= skip;
    Append(out, pcm.data() + skip * info_.channels, (static_cast<size_t>(n) - skip) * info_.channels * 2);
    return true;
  }

  void Flush(int64_t time_us) override {
    opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
    to_discard_ = time_us == 0 ? codec_delay_ : seek_preroll_;
  }

 private:
  static uint64_t Read64(const void* p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
  }
  android::OpusHeader header_ = {};
  OpusMSDecoder* dec_ = nullptr;
  uint64_t codec_delay_ = 0, seek_preroll_ = 0, to_discard_ = 0;
  PcmInfo info_;
};

PcmFormat FromEncoding(int32_t encoding) {
  switch (encoding) {
    case 3: return PcmFormat::kU8;
    case 4: return PcmFormat::kFloat;
    case 21: return PcmFormat::kS24Packed;
    case 22: return PcmFormat::kS32;
    default: return PcmFormat::kS16;
  }
}

}  // namespace

bool AudioDecoder::Supports(const std::string& mime) {
  return mime == "audio/raw" || mime == "audio/mpeg" || mime == "audio/vorbis" ||
         mime == "audio/opus" || mime == "audio/g711-mlaw" || mime == "audio/g711-alaw" ||
         (mime == "audio/mp4a-latm" && MfAacAvailable());
}

std::unique_ptr<AudioDecoder> AudioDecoder::Create(const Format& track, std::string* error) {
  std::string mime = track.Mime();
  int32_t rate = 0, channels = 0;
  track.GetInt32("sample-rate", &rate);
  track.GetInt32("channel-count", &channels);
  if (mime == "audio/raw") {
    int32_t encoding = 2;
    track.GetInt32("pcm-encoding", &encoding);
    if (rate <= 0 || channels <= 0) {
      *error = "audio/raw without rate or channels";
      return nullptr;
    }
    return std::make_unique<RawDecoder>(
        PcmInfo{static_cast<uint32_t>(rate), static_cast<uint32_t>(channels), FromEncoding(encoding)});
  }
  if (mime == "audio/g711-mlaw" || mime == "audio/g711-alaw") {
    return std::make_unique<G711Decoder>(static_cast<uint32_t>(rate), static_cast<uint32_t>(channels),
                                         mime == "audio/g711-mlaw");
  }
  if (mime == "audio/mpeg") {
    if (rate <= 0 || channels <= 0) {
      *error = "audio/mpeg without rate or channels";
      return nullptr;
    }
    return std::make_unique<Mp3Decoder>(static_cast<uint32_t>(rate), static_cast<uint32_t>(channels));
  }
  if (mime == "audio/vorbis") {
    std::vector<uint8_t> ident, setup;
    track.GetBuffer("csd-0", &ident);
    track.GetBuffer("csd-1", &setup);
    auto d = std::make_unique<VorbisDecoder>();
    if (!d->Init(ident, setup, error)) return nullptr;
    return d;
  }
  if (mime == "audio/opus") {
    auto d = std::make_unique<OpusDecoder>();
    if (!d->Init(track, error)) return nullptr;
    return d;
  }
  if (mime == "audio/mp4a-latm") return CreateMfAacDecoder(track, error);
  *error = "no decoder for " + mime;
  return nullptr;
}

}  // namespace gesher::media
