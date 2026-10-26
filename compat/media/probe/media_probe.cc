// Gesher media probe (docs/media.md), run in CI on Windows by
// tests/windows/tracks/38-media.ps1:
//
//   media_probe tones              plays a tone per PCM format through the engine
//                                  (streaming and static tracks, 8 kHz..96 kHz,
//                                  mono and stereo, volume, a loop), checking the
//                                  positions it reports; with GESHER_AUDIO_CAPTURE
//                                  the output lands in a WAV file
//   media_probe analyze <wav>      the WAV's sounds: start, duration, frequency
//                                  (zero crossings) and level of each, one per line
//   media_probe wasapi             opens WASAPI shared mode on the default endpoint
//                                  and renders 200 ms (a tone with GESHER_PROBE_AUDIBLE,
//                                  else silence), or says why it cannot
//   media_probe mf                 lists the Media Foundation decoders this machine has
//
// The engine's own tests: what the framework adds on top (AudioTrack and the rest)
// is tested through test apps.

#include <windows.h>
#include <objbase.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../audio/engine.h"
#include "../audio/sink.h"

using namespace gesher::media;

namespace {

int failures = 0;

void Expect(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) failures++;
}

// A tone of `frames` frames in the given format.
std::vector<uint8_t> Tone(PcmFormat fmt, uint32_t channels, uint32_t rate, double hz, size_t frames,
                          double amplitude) {
  size_t bps = BytesPerSample(fmt);
  std::vector<uint8_t> out(frames * channels * bps);
  for (size_t i = 0; i < frames; i++) {
    double v = amplitude * std::sin(2 * M_PI * hz * static_cast<double>(i) / rate);
    for (uint32_t c = 0; c < channels; c++) {
      uint8_t* p = out.data() + (i * channels + c) * bps;
      switch (fmt) {
        case PcmFormat::kU8: *p = static_cast<uint8_t>(std::lround(128 + 127 * v)); break;
        case PcmFormat::kS16: {
          int16_t s = static_cast<int16_t>(std::lround(32767 * v));
          memcpy(p, &s, 2);
          break;
        }
        case PcmFormat::kS24Packed: {
          int32_t s = static_cast<int32_t>(std::lround(8388607 * v));
          p[0] = static_cast<uint8_t>(s);
          p[1] = static_cast<uint8_t>(s >> 8);
          p[2] = static_cast<uint8_t>(s >> 16);
          break;
        }
        case PcmFormat::kS32: {
          int32_t s = static_cast<int32_t>(std::llround(2147483647.0 * v));
          memcpy(p, &s, 4);
          break;
        }
        case PcmFormat::kFloat: {
          float f = static_cast<float>(v);
          memcpy(p, &f, 4);
          break;
        }
      }
    }
  }
  return out;
}

struct ToneCase {
  const char* name;
  PcmFormat fmt;
  uint32_t channels;
  uint32_t rate;
  double hz;
  double seconds;
  bool is_static;
  float volume;
};

void WaitStopped(const std::shared_ptr<Track>& t, double seconds) {
  for (int i = 0; i < static_cast<int>(seconds * 1000 / 10) + 300; i++) {
    if (t->stopped()) return;
    Sleep(10);
  }
}

int Tones() {
  OutputInfo info = GetOutputInfo();
  printf("output: %s\n", info.description.c_str());
  const ToneCase cases[] = {
      {"16-bit mono 44100 Hz, streaming", PcmFormat::kS16, 1, 44100, 440, 0.6, false, 1.0f},
      {"8-bit mono 8000 Hz, streaming", PcmFormat::kU8, 1, 8000, 500, 0.5, false, 1.0f},
      {"float stereo 22050 Hz, static", PcmFormat::kFloat, 2, 22050, 660, 0.5, true, 1.0f},
      {"24-bit stereo 96000 Hz, streaming", PcmFormat::kS24Packed, 2, 96000, 880, 0.5, false, 1.0f},
      {"32-bit mono 48000 Hz, streaming, volume 0.5", PcmFormat::kS32, 1, 48000, 330, 0.5, false, 0.5f},
      {"16-bit stereo 11025 Hz, static", PcmFormat::kS16, 2, 11025, 1200, 0.4, true, 1.0f},
  };
  for (const ToneCase& c : cases) {
    size_t frames = static_cast<size_t>(c.seconds * c.rate);
    TrackParams p;
    p.sample_rate = c.rate;
    p.channels = c.channels;
    p.format = c.fmt;
    p.is_static = c.is_static;
    p.capacity_frames = c.is_static ? frames : c.rate / 10;  // 100 ms streaming buffer
    p.name = c.name;
    std::string error;
    auto t = CreateTrack(p, &error);
    if (t == nullptr) {
      Expect(false, std::string(c.name) + ": " + error);
      continue;
    }
    t->SetVolume(c.volume, c.volume);
    std::vector<uint8_t> data = Tone(c.fmt, c.channels, c.rate, c.hz, frames, 0.5);
    int64_t start = GetTickCount64();
    if (c.is_static) {
      ssize_t n = t->Write(data.data(), data.size(), false);
      Expect(n == static_cast<ssize_t>(data.size()), std::string(c.name) + ": static data loaded");
      t->Start();
      // A static track stays playing at its end; wait for its position.
      for (int i = 0; i < 400 && t->Position() < frames; i++) Sleep(10);
      t->Stop();
    } else {
      t->Start();
      ssize_t n = t->Write(data.data(), data.size(), true);
      Expect(n == static_cast<ssize_t>(data.size()), std::string(c.name) + ": blocking write took it all");
      t->Stop();
      WaitStopped(t, c.seconds);
    }
    int64_t ms = GetTickCount64() - start;
    uint32_t pos = c.is_static ? static_cast<uint32_t>(frames) : t->Position();
    char line[256];
    snprintf(line, sizeof(line), "%s: %.0f Hz, position %u of %zu frames, %lld ms", c.name, c.hz,
             pos, frames, static_cast<long long>(ms));
    Expect(pos == frames && ms >= c.seconds * 1000 * 0.9, line);
    t->Release();
    Sleep(300);
  }
  // A static track looping twice: 3 x 0.2 s of 750 Hz.
  {
    TrackParams p;
    p.sample_rate = 16000;
    p.channels = 1;
    p.format = PcmFormat::kS16;
    p.is_static = true;
    p.capacity_frames = 3200;
    std::string error;
    auto t = CreateTrack(p, &error);
    std::vector<uint8_t> data = Tone(PcmFormat::kS16, 1, 16000, 750, 3200, 0.5);
    t->Write(data.data(), data.size(), false);
    t->SetLoop(0, 3200, 2);
    int64_t start = GetTickCount64();
    bool ended = false;
    TrackEvents ev;
    ev.end = [&ended] { ended = true; };
    t->SetEvents(ev);
    t->Start();
    for (int i = 0; i < 300 && !ended; i++) Sleep(10);
    int64_t ms = GetTickCount64() - start;
    char line[128];
    snprintf(line, sizeof(line), "static loop x2 (16-bit mono 16000 Hz, 750 Hz): ended after %lld ms",
             static_cast<long long>(ms));
    Expect(ended && ms >= 550 && ms < 1200, line);
    t->Release();
    Sleep(300);
  }
  // Markers and periodic positions on a streaming track: 0.5 s of 520 Hz.
  {
    TrackParams p;
    p.sample_rate = 32000;
    p.channels = 1;
    p.format = PcmFormat::kS16;
    p.capacity_frames = 32000;
    std::string error;
    auto t = CreateTrack(p, &error);
    int markers = 0, updates = 0;
    uint32_t marker_at = 0;
    TrackEvents ev;
    ev.marker = [&](uint32_t pos) { markers++; marker_at = pos; };
    ev.new_position = [&](uint32_t) { updates++; };
    t->SetEvents(ev);
    t->SetMarker(8000);
    t->SetUpdatePeriod(3200);
    std::vector<uint8_t> data = Tone(PcmFormat::kS16, 1, 32000, 520, 16000, 0.5);
    t->Write(data.data(), data.size(), true);  // before play(): fills the buffer
    t->Start();
    t->Stop();
    WaitStopped(t, 0.5);
    Sleep(50);
    char line[128];
    snprintf(line, sizeof(line), "events: marker at %u (%d), %d periodic positions, final position %u",
             marker_at, markers, updates, t->Position());
    Expect(markers == 1 && marker_at == 8000 && updates == 5 && t->Position() == 16000, line);
    t->Release();
  }
  Sleep(300);
  printf("%s\n", failures == 0 ? "media_probe tones: all passed" : "media_probe tones: failed");
  return failures == 0 ? 0 : 1;
}

// --- WAV analysis ------------------------------------------------------------------------

int Analyze(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    printf("cannot open %s\n", path);
    return 2;
  }
  std::vector<uint8_t> file;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
  fclose(f);
  if (file.size() < 44 || memcmp(file.data(), "RIFF", 4) != 0) {
    printf("not a WAV file\n");
    return 2;
  }
  uint16_t channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t data_off = 0, data_len = 0;
  for (size_t off = 12; off + 8 <= file.size();) {
    uint32_t len;
    memcpy(&len, file.data() + off + 4, 4);
    if (memcmp(file.data() + off, "fmt ", 4) == 0) {
      memcpy(&channels, file.data() + off + 10, 2);
      memcpy(&rate, file.data() + off + 12, 4);
      memcpy(&bits, file.data() + off + 22, 2);
    } else if (memcmp(file.data() + off, "data", 4) == 0) {
      data_off = off + 8;
      data_len = std::min<size_t>(len, file.size() - data_off);
      break;
    }
    off += 8 + len + (len & 1);
  }
  if (bits != 16 || channels == 0 || rate == 0) {
    printf("unsupported WAV (%u bits)\n", bits);
    return 2;
  }
  size_t frames = data_len / (2 * channels);
  std::vector<float> left(frames), right(frames);
  for (size_t i = 0; i < frames; i++) {
    int16_t l, r;
    memcpy(&l, file.data() + data_off + i * 2 * channels, 2);
    memcpy(&r, file.data() + data_off + i * 2 * channels + (channels > 1 ? 2 : 0), 2);
    left[i] = l / 32768.0f;
    right[i] = r / 32768.0f;
  }
  printf("wav: %u Hz, %u channels, %.3f s\n", rate, channels, static_cast<double>(frames) / rate);
  // Sounding blocks of 5 ms; sounds are runs of them, apart by 50 ms or more.
  const size_t block = rate / 200;
  std::vector<bool> loud(frames / block + 1, false);
  for (size_t b = 0; b * block < frames; b++) {
    double e = 0;
    size_t end = std::min(frames, (b + 1) * block);
    for (size_t i = b * block; i < end; i++) e += left[i] * left[i] + right[i] * right[i];
    loud[b] = std::sqrt(e / (2.0 * (end - b * block))) > 0.003;
  }
  int index = 0;
  for (size_t b = 0; b < loud.size();) {
    if (!loud[b]) {
      b++;
      continue;
    }
    size_t e = b;
    size_t quiet = 0;
    for (size_t k = b; k < loud.size(); k++) {
      if (loud[k]) {
        e = k;
        quiet = 0;
      } else if (++quiet >= 10) {
        break;
      }
    }
    size_t s0 = b * block, s1 = std::min(frames, (e + 1) * block);
    // Zero crossings (upward) of the louder channel, interpolated. A crossing
    // counts only after the signal went below -30% of its rms (hysteresis):
    // the low-level noise lossy codecs leave around a tone's start and end
    // would otherwise add crossings (an MP3 tone of 523 Hz measured 540).
    double el = 0, er = 0;
    for (size_t i = s0; i < s1; i++) {
      el += left[i] * left[i];
      er += right[i] * right[i];
    }
    const std::vector<float>& x = el >= er ? left : right;
    const double threshold = -0.3 * std::sqrt(std::max(el, er) / (s1 - s0));
    double first = -1, last = -1;
    int crossings = 0;
    bool armed = false;
    for (size_t i = s0 + 1; i < s1; i++) {
      if (x[i] < threshold) armed = true;
      if (armed && x[i - 1] < 0 && x[i] >= 0) {
        armed = false;
        double t = (i - 1) + x[i - 1] / (x[i - 1] - x[i]);
        if (first < 0) first = t;
        last = t;
        crossings++;
      }
    }
    double hz = crossings > 1 ? (crossings - 1) * rate / (last - first) : 0;
    double rms_l = std::sqrt(el / (s1 - s0)), rms_r = std::sqrt(er / (s1 - s0));
    printf("sound %d: start %.3f s, duration %.3f s, %.1f Hz, rms %.3f %.3f\n", ++index,
           static_cast<double>(s0) / rate, static_cast<double>(s1 - s0) / rate, hz, rms_l, rms_r);
    b = e + 1;
  }
  printf("sounds: %d\n", index);
  return 0;
}

int Wasapi() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string error;
  auto sink = OpenWasapiSink(u"Gesher media probe", &error);
  if (sink == nullptr) {
    printf("wasapi: unavailable: %s\n", error.c_str());
    return 0;
  }
  printf("wasapi: %s\n", sink->Describe().c_str());
  // 200 ms of a quiet 440 Hz tone, through the sink as the engine would.
  class ToneMixer : public Mixer {
   public:
    explicit ToneMixer(Sink* s) : sink_(s) {}
    bool MixGroup(int group, float* out, size_t frames) override {
      uint32_t ch = sink_->channels();
      for (size_t i = 0; i < frames; i++, n_++) {
        float v = group == 0 ? amplitude_ * static_cast<float>(std::sin(2 * M_PI * 440 * n_ / sink_->rate())) : 0;
        for (uint32_t c = 0; c < ch; c++) out[i * ch + c] = v;
      }
      if (group == 0) rendered_ += frames;
      return group == 0;
    }
    bool GroupWanted(int group) override { return group == 0; }
    Sink* sink_;
    uint64_t n_ = 0;
    uint64_t rendered_ = 0;
    // Silent unless asked: the probe runs on people's machines too.
    float amplitude_ = getenv("GESHER_PROBE_AUDIBLE") != nullptr ? 0.05f : 0.0f;
  } mixer(sink.get());
  for (int i = 0; i < 20; i++) {
    if (!sink->Tick(&mixer)) {
      printf("wasapi: the output stopped\n");
      return 1;
    }
    Sleep(10);
  }
  printf("wasapi: rendered %llu frames\n", static_cast<unsigned long long>(mixer.rendered_));
  return 0;
}

// Which Media Foundation decoders this machine has (loaded at run time:
// Windows Server has Media Foundation only as a feature).
int MediaFoundation() {
  HMODULE mf = LoadLibraryW(L"mfplat.dll");
  if (mf == nullptr) {
    printf("mf: no mfplat.dll (error %lu)\n", GetLastError());
    return 0;
  }
  using Startup = HRESULT(WINAPI*)(ULONG, DWORD);
  using EnumEx = HRESULT(WINAPI*)(GUID, UINT32, const void*, const void*, IUnknown***, UINT32*);
  auto startup = reinterpret_cast<Startup>(reinterpret_cast<void*>(GetProcAddress(mf, "MFStartup")));
  auto enumEx = reinterpret_cast<EnumEx>(reinterpret_cast<void*>(GetProcAddress(mf, "MFTEnumEx")));
  if (startup == nullptr || enumEx == nullptr) {
    printf("mf: mfplat.dll lacks MFStartup/MFTEnumEx\n");
    return 0;
  }
  HRESULT hr = startup(0x20070 /* MF_VERSION */, 0);
  printf("mf: MFStartup 0x%08lx\n", static_cast<unsigned long>(hr));
  struct TypeInfo {
    GUID major, sub;
  };
  auto fourcc = [](uint32_t d1) {
    return GUID{d1, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
  };
  const GUID kAudioDecoder = {0x9EA73FB4, 0xEF7A, 0x4559, {0x8D, 0x5D, 0x71, 0x9D, 0x8F, 0x04, 0x26, 0xC7}};
  const GUID kVideoDecoder = {0xD6C02D4B, 0x6833, 0x45B4, {0x97, 0x1A, 0x05, 0xA4, 0xB0, 0x4B, 0xAB, 0x91}};
  const GUID kAudio = fourcc(0x73647561), kVideo = fourcc(0x73646976);
  struct Want {
    const char* name;
    bool video;
    uint32_t sub;
  } wants[] = {{"AAC", false, 0x1610}, {"MP3", false, 0x0055}, {"FLAC", false, 0xF1AC},
               {"Opus", false, 0x704F}, {"H.264", true, 0x34363248}, {"HEVC", true, 0x43564548},
               {"VP9", true, 0x30395056}, {"AV1", true, 0x31305641}, {"MPEG-4 part 2", true, 0x5634504D},
               {"VP8", true, 0x30385056}, {"H.263", true, 0x33363248}};
  for (const Want& w : wants) {
    TypeInfo in = {w.video ? kVideo : kAudio, fourcc(w.sub)};
    IUnknown** acts = nullptr;
    UINT32 count = 0;
    hr = enumEx(w.video ? kVideoDecoder : kAudioDecoder, 0x1 | 0x2 | 0x4 | 0x10 | 0x40, &in, nullptr,
                &acts, &count);
    printf("mf: %s decoders: %u (0x%08lx)\n", w.name, count, static_cast<unsigned long>(hr));
    for (UINT32 i = 0; i < count; i++) acts[i]->Release();
    if (acts != nullptr) CoTaskMemFree(acts);
  }
  // The encoders (media3, ADR 0059): MediaCodec's encoders use these where present.
  const GUID kAudioEncoder = {0x91C64BD0, 0xF91E, 0x4D8C, {0x92, 0x76, 0xDB, 0x24, 0x82, 0x79, 0xD9, 0x75}};
  const GUID kVideoEncoder = {0xF79EAC7D, 0xE545, 0x4387, {0xBD, 0xEE, 0xD6, 0x47, 0xD7, 0xBD, 0xE4, 0x2A}};
  const Want encoders[] = {{"AAC", false, 0x1610}, {"Opus", false, 0x704F}, {"H.264", true, 0x34363248},
                           {"HEVC", true, 0x43564548}, {"VP8", true, 0x30385056}, {"VP9", true, 0x30395056}};
  for (const Want& w : encoders) {
    TypeInfo out = {w.video ? kVideo : kAudio, fourcc(w.sub)};
    IUnknown** acts = nullptr;
    UINT32 count = 0;
    hr = enumEx(w.video ? kVideoEncoder : kAudioEncoder, 0x1 | 0x2 | 0x4 | 0x10 | 0x40, nullptr, &out,
                &acts, &count);
    printf("mf: %s encoders: %u (0x%08lx)\n", w.name, count, static_cast<unsigned long>(hr));
    for (UINT32 i = 0; i < count; i++) acts[i]->Release();
    if (acts != nullptr) CoTaskMemFree(acts);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && strcmp(argv[1], "tones") == 0) return Tones();
  if (argc >= 3 && strcmp(argv[1], "analyze") == 0) return Analyze(argv[2]);
  if (argc >= 2 && strcmp(argv[1], "wasapi") == 0) return Wasapi();
  if (argc >= 2 && strcmp(argv[1], "mf") == 0) return MediaFoundation();
  fprintf(stderr, "usage: media_probe tones | analyze <wav> | wasapi\n");
  return 2;
}
