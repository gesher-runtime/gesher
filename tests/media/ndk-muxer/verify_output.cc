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
struct ExpectedSample { int64_t time; uint32_t flags; std::vector<uint8_t> data; };
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
    const auto track = Little(in, 4); const auto time = Little(in, 8); const auto flags = Little(in, 4);
    auto bytes = Blob(in);
    if (!in || track >= 2 || time > INT64_MAX) return false;
    if (!short_audio || !audio[track] || time < 1000000)
      expected[track].push_back({static_cast<int64_t>(time), static_cast<uint32_t>(flags), std::move(bytes)});
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
    int status = 0;
    while ((status = reader->Read(&sample)) == 0) {
      if (index >= expected[track].size()) { ok = false; break; }
      const auto& e = expected[track][index++];
      if (sample.data != e.data || std::llabs(sample.time_us - (e.time - base)) > 1000) {
        std::printf("webm packet mismatch %s track %zu sample %zu actual %lld expected %lld bytes %zu/%zu\n",
            output.c_str(), track, index, static_cast<long long>(sample.time_us),
            static_cast<long long>(e.time - base), sample.data.size(), e.data.size());
        ok = false;
      }
    }
    ok = ok && status == 1 && index == expected[track].size();
    std::printf("webm packet trace %s track %zu read %zu expected %zu\n", output.c_str(), track, index, expected[track].size());
    reader.reset();
    if (!audio[track]) {
      auto seek_reader = extractor->OpenTrack(track);
      Format format = extractor->TrackFormat(track);
      auto decoder = CreateSoftVideoDecoder(format, &error);
      Sample seek_sample;
      const int seek_status = seek_reader ? seek_reader->Read(&seek_sample, 1000000, SeekMode::kPreviousSync) : -1;
      std::vector<VideoFrame> frames;
      const ExpectedSample* key = nullptr;
      for (const auto& e : expected[track]) {
        if ((e.flags & 1) && e.time - base <= 1000000) key = &e;
      }
      const bool seek_ok = key && key->time - base > 0 && seek_status == 0 && seek_sample.sync &&
          std::llabs(seek_sample.time_us - (key->time - base)) <= 1000 && seek_sample.data == key->data &&
          decoder && decoder->Decode(seek_sample, &frames);
      if (decoder) decoder->Drain(&frames);
      ok = ok && seek_ok && !frames.empty();
      std::printf("webm seek trace %s status %d sync %d time %lld decoded %zu\n", output.c_str(),
          seek_status, seek_sample.sync, static_cast<long long>(seek_sample.time_us), frames.size());
    }
  }
  return ok;
}

bool TimedMetadata(Extractor* extractor, size_t track) {
  const std::array<Sample, 3> expected = {{
      {{0x01, 0x00, 0xfe}, 215000},
      {{0x02, 0x17}, 248333},
      {{0x03, 0x42, 0xff, 0x00}, 312175},
  }};
  auto reader = extractor->OpenTrack(track);
  if (!reader) return false;
  Sample actual;
  for (const auto& sample : expected) {
    if (reader->Read(&actual) != 0 || actual.data != sample.data ||
        std::llabs(actual.time_us - sample.time_us) > 11) {
      return false;
    }
  }
  return reader->Read(&actual) == 1;
}

bool ChildBox(const std::vector<uint8_t>& bytes, const Box& parent, const char* type, Box* out) {
  std::vector<Box> children;
  if (!Boxes(bytes, parent.payload, parent.end, &children)) return false;
  for (const Box& child : children) {
    if (child.type == type) { *out = child; return true; }
  }
  return false;
}

bool HeaderDuration(const std::vector<uint8_t>& bytes, const Box& box, size_t offset,
                    uint8_t version, uint64_t expected) {
  if (version == 1) offset += 8;
  const size_t field_size = version == 1 ? 8 : 4;
  return box.end - box.payload >= offset + field_size && bytes[box.payload] == version &&
      Big(bytes, box.payload + offset, field_size) == expected;
}

bool MetadataBoundary(const std::string& path, const std::string& kind) {
  std::string error;
  auto source = OpenFileSource(path, &error);
  auto extractor = source ? Extractor::Open(std::move(source), &error) : nullptr;
  const bool long_offset = kind == "long-offset";
  const bool long_trim = kind == "long-trim";
  const bool offset = long_offset || kind == "offset";
  const size_t tracks = offset ? 2 : 1;
  if (!extractor || extractor->TrackCount() != tracks) return false;
  for (size_t track = 0; track < tracks; ++track) {
    Format format = extractor->TrackFormat(track);
    if (format.Mime() != "application/gyro") return false;
    std::vector<int64_t> times = {0, 33333, 97175};
    if (kind == "long") times = {0, 30000000000LL};
    if (kind == "negative") times = {-33000, 0, 33333};
    if (long_trim) times = {-30000000000LL, 0, 33333};
    if (offset) {
      times = track == 0 ? std::vector<int64_t>{0, 1000000} : std::vector<int64_t>{215567, 248900, 312742};
      if (long_offset && track == 1) {
        for (int64_t& time : times) time += 430000000000LL;
      }
    }
    auto reader = extractor->OpenTrack(track);
    if (!reader) return false;
    Sample sample;
    for (int64_t time : times) {
      if (reader->Read(&sample) != 0 || sample.data != std::vector<uint8_t>{0x12, 0x34, 0x56} ||
          std::llabs(sample.time_us - time) > (offset ? 56 : 11)) return false;
    }
    if (reader->Read(&sample) != 1) return false;
  }

  // The reader above checks decoded times and payloads. Independently inspect
  // duration and legacy MIME fields which a current extractor could overlook.
  std::ifstream input(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  Box root{0, bytes.size(), ""}, moov;
  if (!ChildBox(bytes, root, "moov", &moov)) return false;
  constexpr uint64_t kWideOffset = 4300002156ULL;
  constexpr uint64_t kOffsetSegment = 1610;
  constexpr uint64_t kTrimSegment = 667;
  if (long_offset || long_trim || kind == "negative") {
    Box mvhd;
    const uint64_t expected = long_offset ? kWideOffset + kOffsetSegment : kTrimSegment;
    if (!ChildBox(bytes, moov, "mvhd", &mvhd) || !HeaderDuration(bytes, mvhd, 16, long_offset, expected)) {
      return false;
    }
  }
  std::vector<Box> children;
  if (!Boxes(bytes, moov.payload, moov.end, &children)) return false;
  size_t found = 0;
  for (const Box& trak : children) {
    if (trak.type != "trak") continue;
    if ((long_offset && found == 1) || long_trim || kind == "negative") {
      Box tkhd, edts, elst;
      const uint64_t expected = long_offset ? kWideOffset + kOffsetSegment : kTrimSegment;
      if (!ChildBox(bytes, trak, "tkhd", &tkhd) || !HeaderDuration(bytes, tkhd, 20, long_offset, expected) ||
          !ChildBox(bytes, trak, "edts", &edts) || !ChildBox(bytes, edts, "elst", &elst)) return false;
      const bool wide = long_offset || long_trim;
      const size_t entries = long_offset ? 2 : 1;
      const size_t field_size = wide ? 8 : 4;
      const size_t entry_size = 2 * field_size + 4;
      if (elst.end - elst.payload != 8 + entries * entry_size || bytes[elst.payload] != wide ||
          Big(bytes, elst.payload + 4, 4) != entries) return false;
      size_t entry = elst.payload + 8;
      if (long_offset) {
        if (Big(bytes, entry, 8) != kWideOffset || Big(bytes, entry + 8, 8) != UINT64_MAX) return false;
        entry += entry_size;
      }
      const uint64_t media_start = long_offset ? 0 : long_trim ? 2700000000ULL : 2970;
      if (Big(bytes, entry, field_size) != (long_offset ? kOffsetSegment : kTrimSegment) ||
          Big(bytes, entry + field_size, field_size) != media_start) return false;
    }
    Box mdia, mdhd, minf, stbl, stsd, stts;
    if (!ChildBox(bytes, trak, "mdia", &mdia) || !ChildBox(bytes, mdia, "mdhd", &mdhd) ||
        !ChildBox(bytes, mdia, "minf", &minf) || !ChildBox(bytes, minf, "stbl", &stbl) ||
        !ChildBox(bytes, stbl, "stsd", &stsd) || !ChildBox(bytes, stbl, "stts", &stts)) return false;
    if (stsd.end - stsd.payload < 8) return false;
    Box entries{stsd.payload + 8, stsd.end, ""}, mett;
    if (!ChildBox(bytes, entries, "mett", &mett)) return false;
    constexpr char mime[] = "application/gyro";
    if (mett.end - mett.payload != 2 * sizeof(mime) ||
        std::memcmp(bytes.data() + mett.payload, mime, sizeof(mime)) != 0 ||
        std::memcmp(bytes.data() + mett.payload + sizeof(mime), mime, sizeof(mime)) != 0) return false;
    if (mdhd.end - mdhd.payload < 24 || stts.end - stts.payload < 8) return false;
    const unsigned version = bytes[mdhd.payload];
    uint64_t duration = 0;
    if (version == 1) {
      if (mdhd.end - mdhd.payload < 36 || Big(bytes, mdhd.payload + 20, 4) != 90000) return false;
      duration = Big(bytes, mdhd.payload + 24, 8);
    } else if (version == 0 && Big(bytes, mdhd.payload + 12, 4) == 90000) {
      duration = Big(bytes, mdhd.payload + 16, 4);
    } else { return false; }
    const uint64_t runs = Big(bytes, stts.payload + 4, 4);
    if (runs > (stts.end - stts.payload - 8) / 8) return false;
    uint64_t sum = 0, samples = 0;
    for (size_t i = 0; i < runs; ++i) {
      const uint64_t count = Big(bytes, stts.payload + 8 + 8 * i, 4);
      samples += count;
      sum += count * Big(bytes, stts.payload + 12 + 8 * i, 4);
    }
    if (sum != duration) return false;
    if (kind == "eos" && (duration != 18000 || samples != 3)) return false;
    if (kind == "long" && (duration != 5400000000ULL || version != 1 || samples != 2)) return false;
    ++found;
  }
  return found == tracks;
}

void Verify(const std::string& path, const std::string& video_mime, const std::string& audio_mime,
            double frequency, bool mp4, bool short_audio = false) {
  std::string error;
  auto source = OpenFileSource(path, &error);
  auto extractor = source ? Extractor::Open(std::move(source), &error) : nullptr;
  if (!extractor) { Check(false, "extract " + path + ": " + error); return; }
  const size_t expected = (video_mime.empty() ? 1 : 2) + (mp4 ? 1 : 0);
  bool ok = extractor->TrackCount() == expected;
  unsigned video = 0, audio = 0, metadata = 0;
  for (size_t i = 0; i < extractor->TrackCount(); ++i) {
    Format format = extractor->TrackFormat(i);
    const std::string mime = format.Mime();
    if (!video_mime.empty() && mime == video_mime) {
      ++video;
      ok = Video(extractor.get(), i, format, path) && ok;
    } else if (mime == audio_mime) {
      ++audio;
      ok = Audio(extractor.get(), i, format, frequency, path, short_audio) && ok;
    } else if (mp4 && mime == "application/gyro") {
      ++metadata;
      ok = TimedMetadata(extractor.get(), i) && ok;
    } else { ok = false; }
  }
  if (mp4) ok = Mp4Metadata(path) && ok;
  ok = ok && audio == 1 && video == (video_mime.empty() ? 0u : 1u) && metadata == (mp4 ? 1u : 0u);
  std::printf("media4 muxer container trace: %s; extractor %s; tracks %zu video %u audio %u timed metadata %u; %s\n",
      path.c_str(), extractor->Name().c_str(), extractor->TrackCount(), video, audio, metadata,
      mp4 ? "90-degree matrix and exact ISO-6709 required" : "container codec metadata required");
  Check(ok, "actual-container-decode " + path);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string root = argv[1];
  for (const char* suffix : {".mp4", ".3gp"}) {
    for (const char* kind : {"eos", "long", "negative", "offset", "long-offset", "long-trim"}) {
      const std::string path = root + "/metadata-" + kind + suffix;
      Check(MetadataBoundary(path, kind), "metadata-android-boundary " + path);
    }
  }
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
