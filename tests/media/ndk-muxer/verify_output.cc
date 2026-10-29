// CI only: independently read real muxer output with AOSP extractors and decoders.
// This executable links the reader/decoder backend, without the container writers.
#include "compat/media/player/decoder.h"
#include "compat/media/video/video_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace gesher::media;
int failures = 0;

void Check(bool ok, const std::string& name) {
  std::printf("media4 muxer verify %s: %s\n", name.c_str(), ok ? "ok" : "FAILED");
  if (!ok) ++failures;
}

uint64_t Big(const std::vector<uint8_t>& bytes, size_t at, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[at + i];
  return value;
}

struct Box { size_t payload, end; std::string type; };
bool Boxes(const std::vector<uint8_t>& bytes, size_t begin, size_t end, std::vector<Box>* out) {
  if (end > bytes.size()) return false;
  while (begin < end) {
    if (end - begin < 8) return false;
    uint64_t size = Big(bytes, begin, 4);
    size_t header = 8;
    const std::string type(reinterpret_cast<const char*>(bytes.data() + begin + 4), 4);
    if (size == 1) {
      if (end - begin < 16) return false;
      size = Big(bytes, begin + 8, 8);
      header = 16;
    } else if (size == 0) { size = end - begin; }
    if (size < header || size > end - begin) return false;
    out->push_back({begin + header, begin + static_cast<size_t>(size), type});
    begin += static_cast<size_t>(size);
  }
  return begin == end;
}

bool Mp4Metadata(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::vector<Box> top;
  if (bytes.empty() || !Boxes(bytes, 0, bytes.size(), &top)) return false;
  unsigned rotations = 0, locations = 0;
  for (const auto& moov : top) {
    if (moov.type != "moov") continue;
    std::vector<Box> children;
    if (!Boxes(bytes, moov.payload, moov.end, &children)) return false;
    for (const auto& child : children) {
      if (child.type == "trak") {
        std::vector<Box> track;
        if (!Boxes(bytes, child.payload, child.end, &track)) return false;
        for (const auto& box : track) {
          if (box.type != "tkhd") continue;
          if (box.end - box.payload < 84 || bytes[box.payload] != 0) return false;
          const std::array<uint32_t, 9> expected = {0, 0x10000, 0, 0xffff0000, 0, 0, 0, 0, 0x40000000};
          bool rotated = true;
          for (size_t i = 0; i < expected.size(); ++i) {
            rotated = rotated && Big(bytes, box.payload + 40 + 4 * i, 4) == expected[i];
          }
          rotations += rotated;
        }
      } else if (child.type == "udta") {
        std::vector<Box> metadata;
        if (!Boxes(bytes, child.payload, child.end, &metadata)) return false;
        for (const auto& box : metadata) {
          if (box.type != "\xA9xyz") continue;
          const std::string expected = "+45.1250-075.2500/";
          if (box.end - box.payload != expected.size() + 4 ||
              Big(bytes, box.payload, 2) != expected.size() ||
              Big(bytes, box.payload + 2, 2) != 0x15c7 ||
              std::memcmp(bytes.data() + box.payload + 4, expected.data(), expected.size()) != 0) return false;
          ++locations;
        }
      }
    }
  }
  return rotations == 1 && locations == 1;
}

bool Video(Extractor* extractor, size_t track, const Format& format, const std::string& path) {
  std::string error;
  auto decoder = CreateSoftVideoDecoder(format, &error);
  auto reader = extractor->OpenTrack(track);
  if (!decoder || !reader) { std::printf("video setup: %s\n", error.c_str()); return false; }
  std::vector<VideoFrame> frames;
  Sample sample;
  int status = 0;
  size_t samples = 0;
  while ((status = reader->Read(&sample)) == 0) {
    if (++samples > 300 || !decoder->Decode(sample, &frames)) {
      std::printf("video decode failure: %s; sample %zu time %lld size %zu decoder %s\n",
          path.c_str(), samples, static_cast<long long>(sample.time_us), sample.data.size(), decoder->Name().c_str());
      return false;
    }
  }
  if (status != 1) { std::printf("video read failure: %s status %d\n", path.c_str(), status); return false; }
  decoder->Drain(&frames);
  const std::array<std::array<int, 3>, 4> colors = {{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}}};
  std::array<unsigned, 4> counts = {};
  bool ok = frames.size() == 60 && samples == 60;
  int64_t previous = -1;
  for (size_t i = 0; i < frames.size(); ++i) {
    const auto& frame = frames[i];
    if (frame.visible_width() != 320 || frame.visible_height() != 240 ||
        frame.time_us < previous || frame.data.empty()) { ok = false; continue; }
    previous = frame.time_us;
    uint32_t rgba = 0;
    I420ToRgba(frame, &rgba, 1, 1, 1);
    const std::array<int, 3> actual = {static_cast<int>(rgba & 255), static_cast<int>((rgba >> 8) & 255),
                                        static_cast<int>((rgba >> 16) & 255)};
    const size_t expected = std::min<size_t>(i / 15, 3);
    bool color_matches = true;
    for (size_t c = 0; c < 3; ++c) color_matches = color_matches && std::abs(actual[c] - colors[expected][c]) <= 30;
    if (color_matches) ++counts[expected]; else ok = false;
  }
  ok = ok && std::all_of(counts.begin(), counts.end(), [](unsigned count) { return count == 15; });
  std::printf("media4 muxer video trace: %s; decoder %s; samples %zu frames %zu; colors %u/%u/%u/%u\n",
      path.c_str(), decoder->Name().c_str(), samples, frames.size(), counts[0], counts[1], counts[2], counts[3]);
  return ok;
}

bool Audio(Extractor* extractor, size_t track, const Format& format, double frequency, const std::string& path, bool short_audio) {
  std::string error;
  auto decoder = AudioDecoder::Create(format, &error);
  auto reader = extractor->OpenTrack(track);
  if (!decoder || !reader) { std::printf("audio setup: %s\n", error.c_str()); return false; }
  std::vector<uint8_t> pcm;
  Sample sample;
  int status = 0;
  size_t samples = 0;
  while ((status = reader->Read(&sample)) == 0) {
    if (++samples > 1000 || !decoder->Decode(sample, &pcm) || pcm.size() > 8 * 1024 * 1024) {
      std::printf("audio decode failure: %s; sample %zu time %lld size %zu PCM %zu\n",
          path.c_str(), samples, static_cast<long long>(sample.time_us), sample.data.size(), pcm.size());
      return false;
    }
  }
  if (status != 1) { std::printf("audio read failure: %s status %d\n", path.c_str(), status); return false; }
  decoder->Drain(&pcm);
  const auto info = decoder->info();
  if (info.rate == 0 || info.channels != 2 ||
      (info.format != PcmFormat::kS16 && info.format != PcmFormat::kFloat)) return false;
  const size_t bytes = BytesPerSample(info.format);
  const size_t stride = bytes * info.channels;
  const size_t frames = pcm.size() / stride;
  if (pcm.size() % stride != 0 || frames < info.rate * (short_audio ? 8 : 19) / 10 || frames > info.rate * (short_audio ? 12 : 21) / 10) {
    std::printf("audio duration failure: %s; rate %u channels %u frames %zu bytes %zu stride %zu\n",
        path.c_str(), info.rate, info.channels, frames, pcm.size(), stride);
    return false;
  }
  const size_t begin = info.rate / 4, end = frames - info.rate / 4;
  bool negative = false;
  size_t crossings = 0;
  double energy = 0;
  for (size_t i = begin; i < end; ++i) {
    double value;
    if (info.format == PcmFormat::kS16) {
      int16_t integer;
      std::memcpy(&integer, pcm.data() + i * stride, sizeof(integer));
      value = integer / 32768.0;
    } else {
      float scalar;
      std::memcpy(&scalar, pcm.data() + i * stride, sizeof(scalar));
      value = scalar;
    }
    if (!std::isfinite(value)) return false;
    energy += value * value;
    if (value < -0.02) negative = true;
    if (value > 0.02 && negative) { ++crossings; negative = false; }
  }
  const double measured = static_cast<double>(crossings) * info.rate / (end - begin);
  const double rms = std::sqrt(energy / (end - begin));
  std::printf("media4 muxer audio trace: %s; %uHz/%u channels frames %zu samples %zu; tone %.3f expected %.3f RMS %.6f\n",
      path.c_str(), info.rate, info.channels, frames, samples, measured, frequency, rms);
  return std::fabs(measured - frequency) <= frequency * 0.02 && rms > 0.1;
}

// The independently generated input contains the exact source packet bytes and
// timestamps. Compare actual extractor samples, including the tail emitted by Stop.
uint64_t Little(std::istream& in, size_t n) {
  uint64_t result = 0;
  for (size_t i = 0; i < n; ++i) {
    const int c = in.get();
    if (c == EOF) { in.setstate(std::ios::failbit); return 0; }
    result |= static_cast<uint64_t>(c) << (8 * i);
  }
  return result;
}
std::vector<uint8_t> Blob(std::istream& in) {
  const auto size = Little(in, 4);
  if (size > 16 * 1024 * 1024) { in.setstate(std::ios::failbit); return {}; }
  std::vector<uint8_t> data(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
  return data;
}
struct ExpectedSample { int64_t time; std::vector<uint8_t> data; };
bool PacketOracle(const std::string& fixture, const std::string& output, bool short_audio) {
  std::ifstream in(fixture, std::ios::binary);
  char magic[8] = {};
  in.read(magic, 8);
  if (!in || std::memcmp(magic, "GSMUX001", 8) != 0 || Little(in, 4) != 2) return false;
  std::array<bool, 2> audio = {};
  for (size_t track = 0; track < 2; ++track) {
    const auto fields = Little(in, 4);
    if (fields > 32) return false;
    for (size_t j = 0; j < fields; ++j) {
      const auto key = Blob(in); Little(in, 4); const auto value = Blob(in);
      if (std::string(key.begin(), key.end()) == "mime")
        audio[track] = std::string(value.begin(), value.end()).find("audio/") == 0;
    }
  }
  if (audio[0] == audio[1]) return false;
  std::array<std::vector<ExpectedSample>, 2> expected;
  const auto count = Little(in, 4);
  if (count > 5000) return false;
  for (size_t i = 0; i < count; ++i) {
    const auto track = Little(in, 4); const auto time = Little(in, 8); Little(in, 4);
    auto bytes = Blob(in);
    if (!in || track >= 2 || time > INT64_MAX) return false;
    if (!short_audio || !audio[track] || time < 1000000)
      expected[track].push_back({static_cast<int64_t>(time), std::move(bytes)});
  }
  std::string error;
  auto source = OpenFileSource(output, &error);
  auto extractor = source ? Extractor::Open(std::move(source), &error) : nullptr;
  if (!extractor || extractor->TrackCount() != 2 || expected[0].empty() || expected[1].empty()) return false;
  const int64_t base = std::min(expected[0][0].time, expected[1][0].time);
  bool ok = true;
  for (size_t track = 0; track < 2; ++track) {
    auto reader = extractor->OpenTrack(track);
    if (!reader) return false;
    Sample sample;
    size_t index = 0;
    while (reader->Read(&sample) == 0) {
      if (index >= expected[track].size()) { ok = false; break; }
      const auto& e = expected[track][index++];
      if (sample.data != e.data || std::llabs(sample.time_us - (e.time - base)) > 1000) {
        std::printf("webm packet mismatch %s track %zu sample %zu actual %lld expected %lld bytes %zu/%zu\n",
            output.c_str(), track, index, static_cast<long long>(sample.time_us),
            static_cast<long long>(e.time - base), sample.data.size(), e.data.size());
        ok = false;
      }
    }
    ok = ok && index == expected[track].size();
    std::printf("webm packet trace %s track %zu read %zu expected %zu\n", output.c_str(), track, index, expected[track].size());
    reader.reset();
    if (!audio[track]) {
      auto seek_reader = extractor->OpenTrack(track);
      Format format = extractor->TrackFormat(track);
      auto decoder = CreateSoftVideoDecoder(format, &error);
      Sample seek_sample;
      const int seek_status = seek_reader ? seek_reader->Read(&seek_sample, 1000000, SeekMode::kPreviousSync) : -1;
      std::vector<VideoFrame> frames;
      const bool seek_ok = seek_status == 0 && seek_sample.sync && seek_sample.time_us <= 1000000 &&
          decoder && decoder->Decode(seek_sample, &frames);
      if (decoder) decoder->Drain(&frames);
      ok = ok && seek_ok && !frames.empty();
      std::printf("webm seek trace %s status %d sync %d time %lld decoded %zu\n", output.c_str(),
          seek_status, seek_sample.sync, static_cast<long long>(seek_sample.time_us), frames.size());
    }
  }
  return ok;
}

void Verify(const std::string& path, const std::string& video_mime, const std::string& audio_mime,
            double frequency, bool mp4, bool short_audio = false) {
  std::string error;
  auto source = OpenFileSource(path, &error);
  auto extractor = source ? Extractor::Open(std::move(source), &error) : nullptr;
  if (!extractor) { Check(false, "extract " + path + ": " + error); return; }
  const size_t expected = video_mime.empty() ? 1 : 2;
  bool ok = extractor->TrackCount() == expected;
  unsigned video = 0, audio = 0;
  for (size_t i = 0; i < extractor->TrackCount(); ++i) {
    Format format = extractor->TrackFormat(i);
    const std::string mime = format.Mime();
    if (!video_mime.empty() && mime == video_mime) {
      ++video;
      ok = Video(extractor.get(), i, format, path) && ok;
    } else if (mime == audio_mime) {
      ++audio;
      ok = Audio(extractor.get(), i, format, frequency, path, short_audio) && ok;
    } else { ok = false; }
  }
  if (mp4) ok = Mp4Metadata(path) && ok;
  ok = ok && audio == 1 && video == (video_mime.empty() ? 0u : 1u);
  std::printf("media4 muxer container trace: %s; extractor %s; tracks %zu video %u audio %u; metadata %s\n",
      path.c_str(), extractor->Name().c_str(), extractor->TrackCount(), video, audio,
      mp4 ? "90-degree matrix and exact ISO-6709 required" : "container codec metadata required");
  Check(ok, "actual-container-decode " + path);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string root = argv[1];
  struct Case { const char* stem; const char* extension; const char* video; const char* audio; double hz; bool mp4; };
  const Case cases[] = {{"h264-aac", "mp4", "video/avc", "audio/mp4a-latm", 1661, true},
    {"h264-aac", "3gp", "video/avc", "audio/mp4a-latm", 1661, true},
    {"vp9-opus", "webm", "video/x-vnd.on2.vp9", "audio/opus", 880, false},
    {"vp8-vorbis", "webm", "video/x-vnd.on2.vp8", "audio/vorbis", 660, false},
    {"opus", "ogg", "", "audio/opus", 880, false}};
  for (const auto& test : cases) {
    for (const char* order : {"interleaved", "blocked"}) {
      if (std::strcmp(test.extension, "ogg") == 0 && std::strcmp(order, "blocked") == 0) continue;
      Verify(root + "/" + test.stem + "-" + order + "." + test.extension,
          test.video, test.audio, test.hz, test.mp4);
    }
  }
  for (const auto& test : cases) {
    if (std::strcmp(test.extension, "webm") != 0) continue;
    for (const char* order : {"interleaved", "blocked", "short-interleaved", "short-blocked"}) {
      const bool short_audio = std::strncmp(order, "short-", 6) == 0;
      const std::string path = root + "/" + test.stem + "-" + order + ".webm";
      if (short_audio) Verify(path, test.video, test.audio, test.hz, false, true);
      Check(PacketOracle(std::string(argv[2]) + "/" + test.stem + ".bin", path, short_audio),
          "webm-input-packets-pts " + path);
    }
  }
  Verify(root + "/h264-aac-delete-active.mp4", "video/avc", "audio/mp4a-latm", 1661, true);
  std::printf("media4 muxer independent verification: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
