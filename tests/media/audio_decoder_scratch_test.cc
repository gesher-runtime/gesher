// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete production AudioDecoder with the actual pinned software providers.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#include <opus.h>
#include <opus_multistream.h>
#include "pvmp3decoder_api.h"
#include "compat/media/player/decoder.h"
extern "C" {
#include <Tremolo/codec_internal.h>
}

namespace {
size_t checks = 0;
void Check(bool good, const char* condition, int line) {
  ++checks;
  if (!good) {
    std::fprintf(stderr, "Audio scratch check failed at %d: %s\n", line, condition);
    std::abort();
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)
uint64_t Hash(const void* data, size_t size, uint64_t value = 1469598103934665603ULL) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ULL;
  return value;
}
// Count every C++ allocation made during the complete product Decode/Drain
// workload, including caller-output growth. Provider C allocations, metadata,
// fixture storage and comparisons are outside this explicitly bounded census.
bool counting = false, poison = true;
size_t allocated_calls = 0, allocated_bytes = 0, provider_calls = 0;
size_t allocated_live = 0, allocated_peak = 0;
struct Allocation { void* pointer = nullptr; size_t bytes = 0; };
std::array<Allocation, 64> allocations{};
int audio_channels = 0;
struct Metadata {
  std::string mime;
  int32_t rate = 0, channels = 0;
  std::array<std::vector<uint8_t>, 3> csd;
};
}

void* operator new(size_t size) {
  void* data = std::malloc(size ? size : 1);
  if (!data) std::abort();
  if (counting) {
    bool recorded = false;
    for (auto& item : allocations) {
      if (!item.pointer) { item = {data, size}; recorded = true; break; }
    }
    CHECK(recorded);
    ++allocated_calls;
    allocated_bytes += size;
    allocated_live += size;
    allocated_peak = std::max(allocated_peak, allocated_live);
  }
  return data;
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* data) noexcept {
  if (data) for (auto& item : allocations) {
    if (item.pointer == data) { allocated_live -= item.bytes; item = {}; break; }
  }
  std::free(data);
}
void operator delete[](void* data) noexcept { ::operator delete(data); }
void operator delete(void* data, size_t) noexcept { ::operator delete(data); }
void operator delete[](void* data, size_t) noexcept { ::operator delete(data); }

// The AMediaFormat metadata transport is a declared fixture boundary. Values
// are the real encoded headers and packet/page metadata, never codec results.
namespace gesher::media {
Format::~Format() {}
std::string Format::Mime() const { return reinterpret_cast<const Metadata*>(get())->mime; }
bool Format::GetInt32(const char* name, int32_t* out) const {
  const auto& metadata = *reinterpret_cast<const Metadata*>(get());
  if (!std::strcmp(name, "sample-rate")) *out = metadata.rate;
  else if (!std::strcmp(name, "channel-count")) *out = metadata.channels;
  else return false;
  return true;
}
bool Format::GetBuffer(const char* name, std::vector<uint8_t>* out) const {
  CHECK(std::strlen(name) == 5 && !std::strncmp(name, "csd-", 4));
  int index = name[4] - '0';
  CHECK(index >= 0 && index < 3);
  const auto& bytes = reinterpret_cast<const Metadata*>(get())->csd[index];
  if (bytes.empty()) return false;
  *out = bytes;
  return true;
}
// Unused Media Foundation selection cannot fabricate a successful decoder.
bool MfAacAvailable() { std::abort(); }
std::unique_ptr<AudioDecoder> CreateMfAacDecoder(const Format&, std::string*) { std::abort(); }
}

static void Poison(int16_t* pcm, size_t samples) {
  if (poison) std::fill(pcm, pcm + samples, provider_calls & 1 ? int16_t{0x5a5a} : int16_t{-23131});
  ++provider_calls;
}
static ERROR_CODE ObservedMp3(tPVMP3DecoderExternal* config, void* memory) {
  int capacity = config->outputFrameSize;
  CHECK(capacity == 4608);
  Poison(config->pOutputBuffer, static_cast<size_t>(capacity));
  ERROR_CODE result = pvmp3_framedecoder(config, memory);
  CHECK(config->outputFrameSize >= 0 && config->outputFrameSize <= capacity);
  CHECK(config->inputBufferUsedLength >= 0 && config->inputBufferUsedLength <= config->inputBufferCurrentLength);
  return result;
}
static int ObservedVorbis(vorbis_dsp_state* state, ogg_int16_t* pcm, int capacity) {
  CHECK(capacity == 8192 && state->vi->channels == audio_channels);
  Poison(pcm, static_cast<size_t>(capacity) * audio_channels);
  int frames = vorbis_dsp_pcmout(state, pcm, capacity);
  CHECK(frames <= capacity);
  return frames;
}
static int ObservedOpus(OpusMSDecoder* state, const unsigned char* data, opus_int32 size,
                        opus_int16* pcm, int capacity, int fec) {
  CHECK(capacity == 5760 && fec == 0);
  Poison(pcm, static_cast<size_t>(capacity) * audio_channels);
  int frames = opus_multistream_decode(state, data, size, pcm, capacity, fec);
  CHECK(frames <= capacity);
  return frames;
}
#define pvmp3_framedecoder ObservedMp3
#define vorbis_dsp_pcmout ObservedVorbis
#define opus_multistream_decode ObservedOpus
#ifndef GESHER_AUDIO_DECODE_SOURCE
#define GESHER_AUDIO_DECODE_SOURCE "compat/media/player/decoder.cc"
#endif
#include GESHER_AUDIO_DECODE_SOURCE
#undef pvmp3_framedecoder
#undef vorbis_dsp_pcmout
#undef opus_multistream_decode

namespace {
using gesher::media::AudioDecoder;
using gesher::media::Format;
using gesher::media::Sample;
template<typename T> T Read(FILE* file) {
  T value{};
  CHECK(std::fread(&value, sizeof(value), 1, file) == 1);
  return value;
}
std::vector<uint8_t> ReadBytes(FILE* file, size_t size) {
  std::vector<uint8_t> bytes(size);
  CHECK(size == 0 || std::fread(bytes.data(), 1, size, file) == size);
  return bytes;
}
struct Fixture {
  Metadata metadata;
  uint32_t codec = 0, expected_frames = 0;
  uint64_t delay = 0, preroll = 0;
  std::vector<Sample> packets;
  std::vector<uint8_t> reference;
};
Fixture Load(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file != nullptr);
  CHECK(Read<uint32_t>(file) == 0x44554147);
  Fixture fixture;
  fixture.codec = Read<uint32_t>(file);
  CHECK(fixture.codec >= 1 && fixture.codec <= 3);
  fixture.metadata.mime = fixture.codec == 1 ? "audio/mpeg" : fixture.codec == 2 ? "audio/vorbis" : "audio/opus";
  fixture.metadata.rate = Read<int32_t>(file);
  fixture.metadata.channels = Read<int32_t>(file);
  uint32_t count = Read<uint32_t>(file), head0 = Read<uint32_t>(file), head1 = Read<uint32_t>(file);
  fixture.delay = Read<uint64_t>(file);
  fixture.preroll = Read<uint64_t>(file);
  fixture.expected_frames = Read<uint32_t>(file);
  uint32_t reference = Read<uint32_t>(file);
  fixture.metadata.csd[0] = ReadBytes(file, head0);
  fixture.metadata.csd[1] = ReadBytes(file, head1);
  if (fixture.codec == 3) {
    fixture.metadata.csd[1].resize(8);
    fixture.metadata.csd[2].resize(8);
    std::memcpy(fixture.metadata.csd[1].data(), &fixture.delay, 8);
    std::memcpy(fixture.metadata.csd[2].data(), &fixture.preroll, 8);
  }
  fixture.reference = ReadBytes(file, reference);
  for (uint32_t i = 0; i < count; ++i) {
    Sample packet;
    packet.time_us = Read<int64_t>(file);
    packet.valid_samples = Read<int32_t>(file);
    packet.data = ReadBytes(file, Read<uint32_t>(file));
    fixture.packets.push_back(std::move(packet));
  }
  CHECK(std::fgetc(file) == EOF && std::fclose(file) == 0);
  return fixture;
}
std::unique_ptr<AudioDecoder> Create(const Fixture& fixture) {
  Format metadata(reinterpret_cast<AMediaFormat*>(const_cast<Metadata*>(&fixture.metadata)));
  std::string error;
  auto decoder = AudioDecoder::Create(metadata, &error);
  CHECK(decoder && error.empty());
  CHECK(decoder->info().rate == static_cast<uint32_t>(fixture.metadata.rate));
  CHECK(decoder->info().channels == static_cast<uint32_t>(fixture.metadata.channels));
  audio_channels = fixture.metadata.channels;
  return decoder;
}
void Write(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  CHECK(file != nullptr);
  CHECK(bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  CHECK(std::fclose(file) == 0);
}

std::vector<uint8_t> Contracts(const Fixture& fixture, const std::string& name, const std::string& directory) {
  auto decoder = Create(fixture);
  std::vector<std::vector<uint8_t>> retained;
  std::vector<uint64_t> hashes;
  std::vector<uint8_t> joined;
  for (const auto& original : fixture.packets) {
    Sample packet = original;
    std::vector<uint8_t> output(16, 0xa5);
    CHECK(decoder->Decode(packet, &output));
    CHECK(std::all_of(output.begin(), output.begin() + 16, [](uint8_t byte) { return byte == 0xa5; }));
    // Compressed bytes cease to be available immediately after the call.
    std::fill(packet.data.begin(), packet.data.end(), 0xcc);
    joined.insert(joined.end(), output.begin() + 16, output.end());
    hashes.push_back(Hash(output.data(), output.size()));
    retained.push_back(std::move(output));
  }
  std::vector<uint8_t> tail;
  decoder->Drain(&tail);
  joined.insert(joined.end(), tail.begin(), tail.end());
  CHECK(joined.size() == static_cast<size_t>(fixture.expected_frames) * audio_channels * 2);
  CHECK(joined.size() >= fixture.reference.size());
  if (fixture.codec != 3) CHECK(joined.size() == fixture.reference.size());
  else CHECK(joined.size() - fixture.reference.size() < static_cast<size_t>(5760 * audio_channels * 2));
  int maximum = 0;
  uint64_t total_error = 0;
  for (size_t i = 0; i < fixture.reference.size(); i += 2) {
    int16_t actual = 0, expected = 0;
    std::memcpy(&actual, joined.data() + i, 2);
    std::memcpy(&expected, fixture.reference.data() + i, 2);
    int difference = std::abs(static_cast<int>(actual) - expected);
    maximum = std::max(maximum, difference);
    total_error += difference;
  }
  std::printf("oracle,%s,frames=%zu,max_sample_error=%d,total_absolute_error=%llu\n", name.c_str(),
      fixture.reference.size() / (audio_channels * 2), maximum, static_cast<unsigned long long>(total_error));
  std::fflush(stdout);
  // FFmpeg's floating-point implementations round differently from Android's
  // pinned fixed-point providers. Both peak error and whole-clip error are
  // bounded; all candidate/baseline PCM is additionally compared byte-for-byte.
  CHECK(maximum <= (fixture.codec == 3 ? (audio_channels > 2 ? 192 : 64) : 2));
  CHECK(total_error <= fixture.reference.size());  // mean <= 2 signed-16 units
  Write(directory + "/" + name + ".actual.s16", joined);

  std::vector<uint8_t> after_drain;
  CHECK(decoder->Decode(fixture.packets.back(), &after_drain));
  Write(directory + "/" + name + ".after-drain.s16", after_drain);

  // Restart the used decoder, then compare every packet with the first decode.
  // Different poison on every provider call exposes unwritten/stale samples.
  decoder->Flush(0);
  for (size_t i = 0; i < fixture.packets.size(); ++i) {
    std::vector<uint8_t> output;
    CHECK(decoder->Decode(fixture.packets[i], &output));
    CHECK(output.size() + 16 == retained[i].size());
    CHECK(std::equal(output.begin(), output.end(), retained[i].begin() + 16));
    CHECK(Hash(retained[i].data(), retained[i].size()) == hashes[i]);
  }
  std::vector<uint8_t> second_tail;
  decoder->Drain(&second_tail);
  CHECK(second_tail == tail);

  if (fixture.codec == 1) {
    // A demuxer may supply several complete MP3 frames in one Sample.
    decoder->Flush(0);
    Sample combined;
    for (const auto& packet : fixture.packets)
      combined.data.insert(combined.data.end(), packet.data.begin(), packet.data.end());
    std::vector<uint8_t> output;
    CHECK(decoder->Decode(combined, &output));
    decoder->Drain(&output);
    CHECK(output == joined);
  }

  // Nonzero seek uses Opus preroll and restores Vorbis page accounting.
  decoder->Flush(1000000);
  std::vector<uint8_t> sought;
  for (auto packet : fixture.packets) {
    packet.time_us += 1000000;
    CHECK(decoder->Decode(packet, &sought));
  }
  decoder->Drain(&sought);
  size_t extra_skip = fixture.codec == 3 ? static_cast<size_t>(
      gesher::media::NsToSamples(fixture.preroll) - gesher::media::NsToSamples(fixture.delay)) * audio_channels * 2 : 0;
  CHECK(extra_skip <= joined.size());
  CHECK(sought.size() + extra_skip == joined.size());
  CHECK(std::equal(sought.begin(), sought.end(), joined.begin() + extra_skip));

  decoder->Flush(0);
  Sample broken;
  broken.time_us = 1;
  broken.data = fixture.codec == 3 ? std::vector<uint8_t>{0x03, 0x3f} :
      fixture.codec == 1 ? std::vector<uint8_t>{0, 0, 0, 0} : std::vector<uint8_t>{0x01};
  std::vector<uint8_t> untouched(13, 0x3c);
  bool accepted = decoder->Decode(broken, &untouched);
  CHECK(accepted == (fixture.codec == 2));
  CHECK(untouched == std::vector<uint8_t>(13, 0x3c));
  if (fixture.codec == 1) {
    broken.data = {1};  // Actual provider's recoverable incomplete-header path.
    CHECK(decoder->Decode(broken, &untouched));
    CHECK(untouched == std::vector<uint8_t>(13, 0x3c));
  }
  decoder->Flush(0);
  for (size_t i = 0; i < std::min<size_t>(12, fixture.packets.size()); ++i) {
    std::vector<uint8_t> output;
    CHECK(decoder->Decode(fixture.packets[i], &output));
    CHECK(output.size() + 16 == retained[i].size());
    CHECK(std::equal(output.begin(), output.end(), retained[i].begin() + 16));
  }
  if (fixture.codec == 3) {
    decoder->Flush(0);
    Sample lost;
    lost.time_us = 1;
    std::vector<uint8_t> concealment;
    CHECK(decoder->Decode(lost, &concealment));
    CHECK(concealment.size() == (5760 - gesher::media::NsToSamples(fixture.delay)) * audio_channels * 2);
    CHECK(std::all_of(concealment.begin(), concealment.end(), [](uint8_t byte) { return byte == 0; }));

    // Exercise Android's unified CSD route with its real parser/writer.
    Fixture unified = fixture;
    android::OpusHeader header{};
    CHECK(android::ParseOpusHeader(fixture.metadata.csd[0].data(), fixture.metadata.csd[0].size(), &header));
    if (audio_channels <= 2) {
      unified.metadata.csd[0].resize(1024);
      int written = android::WriteOpusHeaders(header, fixture.metadata.rate, unified.metadata.csd[0].data(),
          unified.metadata.csd[0].size(), fixture.delay, fixture.preroll);
      CHECK(written > 0);
      unified.metadata.csd[0].resize(static_cast<size_t>(written));
    } else {
      // The AOSP writer describes its own uncoupled encoder. Preserve this
      // independently encoded stream's actual coupling/map in the container.
      unified.metadata.csd[0].clear();
      auto add = [&](const char* marker, const std::vector<uint8_t>& bytes) {
        auto& csd = unified.metadata.csd[0];
        uint64_t size = bytes.size();
        csd.insert(csd.end(), marker, marker + 8);
        const auto* length = reinterpret_cast<const uint8_t*>(&size);
        csd.insert(csd.end(), length, length + 8);
        csd.insert(csd.end(), bytes.begin(), bytes.end());
      };
      add("AOPUSHDR", fixture.metadata.csd[0]);
      add("AOPUSDLY", fixture.metadata.csd[1]);
      add("AOPUSPRL", fixture.metadata.csd[2]);
    }
    unified.metadata.csd[1].clear();
    unified.metadata.csd[2].clear();
    auto other = Create(unified);
    std::vector<uint8_t> output;
    for (const auto& packet : fixture.packets) CHECK(other->Decode(packet, &output));
    other->Drain(&output);
    CHECK(output == joined);

    if (audio_channels > 2) {
      // Mapping 255 is an actual libopus muted output channel. Poisoning the
      // whole destination verifies that the provider explicitly writes zeros.
      Fixture muted = fixture;
      muted.metadata.csd[0][21 + audio_channels - 1] = 255;
      other = Create(muted);
      output.clear();
      for (const auto& packet : fixture.packets) CHECK(other->Decode(packet, &output));
      CHECK(output.size() == joined.size());
      bool correct = true;
      for (size_t i = 0; i < output.size(); ++i)
        correct = correct && output[i] == ((i / 2) % audio_channels == static_cast<size_t>(audio_channels - 1)
                                          ? 0 : joined[i]);
      CHECK(correct);
    }
  }
  decoder.reset();
  for (size_t i = 0; i < retained.size(); ++i) CHECK(Hash(retained[i].data(), retained[i].size()) == hashes[i]);
  return joined;
}

void Measure(const Fixture& fixture, const std::string& name, const std::vector<uint8_t>& reference, bool gate) {
  auto decoder = Create(fixture);
  poison = false;
  allocated_calls = allocated_bytes = 0;
  CHECK(allocated_live == 0);
  allocated_peak = 0;
  size_t bytes = 0, outputs = 0;
  uint64_t hash = 1469598103934665603ULL;
  for (const auto& packet : fixture.packets) {
    // MediaPlayer and MediaCodec both release their packet-local PCM after
    // submitting/copying it. Include that actual caller-output allocation.
    std::vector<uint8_t> output;
    counting = true;
    bool success = decoder->Decode(packet, &output);
    counting = false;
    CHECK(success);
    bytes += output.size();
    if (!output.empty()) ++outputs;
    hash = Hash(output.data(), output.size(), hash);
  }
  const size_t active_scratch = allocated_live;
  std::vector<uint8_t> tail;
  counting = true;
  decoder->Drain(&tail);
  counting = false;
  bytes += tail.size();
  if (!tail.empty()) ++outputs;
  hash = Hash(tail.data(), tail.size(), hash);
  CHECK(bytes == reference.size() && hash == Hash(reference.data(), reference.size()));
  std::vector<uint8_t>().swap(tail);
  const size_t scratch = fixture.codec == 1 ? 4608 * 2 :
      static_cast<size_t>(fixture.codec == 2 ? 8192 : 5760) * audio_channels * 2;
  const size_t drain_scratch = fixture.codec == 1 ? 529 * audio_channels * 2 : 0;
  std::printf("metric,%s,packets=%zu,outputs=%zu,cpp_allocations=%zu,cpp_allocated_bytes=%zu,"
              "pcm_bytes=%zu,active_scratch_bytes=%zu,drained_scratch_bytes=%zu,peak_cpp_live_bytes=%zu\n",
              name.c_str(), fixture.packets.size(), outputs,
              allocated_calls, allocated_bytes, bytes, active_scratch, allocated_live, allocated_peak);
  std::fflush(stdout);
  if (gate) {
    CHECK(allocated_calls <= outputs + 1 + (fixture.codec == 1 ? 1 : 0));
    CHECK(allocated_bytes <= bytes + scratch + drain_scratch);
    CHECK(active_scratch == scratch && allocated_live == 0);
  }
  decoder.reset();
  CHECK(allocated_live == 0);
  poison = true;
}
}  // namespace

int main(int argc, char** argv) {
  CHECK(argc == 3);
  bool gate = std::string(argv[1]) == "gate";
  CHECK(gate || std::string(argv[1]) == "measure");
  const std::string directory = argv[2];
  for (const char* name : {"mp3-mono", "mp3-stereo", "mp3-mpeg2", "vorbis-mono", "vorbis-stereo",
                          "opus-mono", "opus-stereo", "opus-long", "opus-surround"}) {
    Fixture fixture = Load(directory + "/" + name + ".audt");
    auto reference = Contracts(fixture, name, directory);
    Measure(fixture, name, reference, gate);
  }
  std::printf("Real audio decoder scratch, retained PCM, seek, trim and error contracts passed: %zu checks\n", checks);
}
