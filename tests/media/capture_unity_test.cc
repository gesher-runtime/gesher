// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The complete capture methods and paced WAV source are a separate real TU.
#include "capture_unity_common.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

uint64_t capture_channel_frames = 0;
int64_t capture_source_clock_ns = 1000000000;
namespace {
using namespace gesher::media;
size_t checks = 0;
bool efficiency_gate = true;
CaptureEngine engine;

void Check(bool condition, const char* reason) {
  ++checks;
  if (!condition) { std::fprintf(stderr, "CAPTURE_UNITY_FAIL: %s\n", reason); std::abort(); }
}
void Write(FILE* file, const void* bytes, size_t size) {
  if (size) Check(std::fwrite(bytes, 1, size, file) == size, "write complete transcript");
}
template<class T> void Field(FILE* file, const T& value) { Write(file, &value, sizeof(value)); }
void Record(FILE* file, CaptureStream& stream) {
  Field(file, stream.written_); Field(file, stream.read_); Field(file, stream.overruns_);
  Field(file, stream.overrun_count_); Field(file, stream.was_overrun_); Field(file, stream.last_ns_);
  Field(file, stream.phase_); Field(file, stream.primed_); Field(file, stream.started_);
  Field(file, stream.released_); Field(file, stream.error_); Field(file, stream.generation_);
  Field(file, stream.marker_reached_); Field(file, stream.next_update_);
  Field(file, stream.StateWakeSequence());
  Write(file, stream.last_.data(), stream.last_.size() * sizeof(float));
  Field(file, stream.converted_.size()); Field(file, stream.converted_.capacity());
  Write(file, stream.converted_.data(), stream.converted_.size() * sizeof(float));
  const size_t available = stream.Available(); Field(file, available);
  for (size_t i = 0; i < available; ++i) {
    const size_t slot = static_cast<size_t>((stream.read_ + i) % stream.capacity_frames());
    Write(file, stream.ring_.data() + slot * stream.frame_size(), stream.frame_size());
  }
}

std::vector<float> Pattern(size_t first, size_t frames, uint32_t channels) {
  std::vector<float> values(frames * channels);
  for (size_t i = 0; i < frames; ++i)
    for (uint32_t c = 0; c < channels; ++c)
      values[i * channels + c] = static_cast<float>(int((first + i * 13 + c * 29) % 251) - 125) / 128.0f;
  return values;
}

void EncodeExpected(float value, PcmFormat format, std::vector<uint8_t>* out) {
  const float v = std::clamp(value, -1.0f, 1.0f);
  auto bytes = [&](const void* p, size_t size) {
    const auto* b = static_cast<const uint8_t*>(p); out->insert(out->end(), b, b + size);
  };
  switch (format) {
    case PcmFormat::kU8: { uint8_t n = static_cast<uint8_t>(std::lround(v * 127.0f) + 128); bytes(&n, 1); break; }
    case PcmFormat::kS16: { int16_t n = static_cast<int16_t>(std::lround(v * 32767.0f)); bytes(&n, 2); break; }
    case PcmFormat::kS24Packed: {
      const int32_t n = static_cast<int32_t>(std::lround(v * 8388607.0f));
      const uint8_t b[]{static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n >> 16)};
      bytes(b, 3); break;
    }
    case PcmFormat::kS32: { int32_t n = static_cast<int32_t>(std::llround(v * 2147483647.0)); bytes(&n, 4); break; }
    case PcmFormat::kFloat: bytes(&v, 4); break;
  }
}

void FiniteBlocks(FILE* transcript) {
  for (auto format : {PcmFormat::kU8, PcmFormat::kS16, PcmFormat::kS24Packed, PcmFormat::kS32, PcmFormat::kFloat}) {
    for (uint32_t ch : {1u, 2u, 8u}) {
      for (size_t cap : {size_t{1}, size_t{3}, size_t{17}, size_t{521}}) {
        CaptureParams params; params.channels = ch; params.capacity_frames = cap; params.format = format;
        CaptureStream stream(&engine, params); Check(stream.Start() == kOk, "actual capture activation succeeds");
        std::vector<uint8_t> queue;
        std::vector<float> last(ch);
        bool primed = false;
        uint64_t produced = 0, consumed = 0;
        uint32_t lost = 0;
        size_t block = 0;
        for (size_t frames : {size_t{1}, size_t{2}, size_t{7}, size_t{480}, size_t{0}, size_t{13}, size_t{480}}) {
          auto source = Pattern(block * 137, frames, ch); const auto original = source;
          const size_t room = cap - queue.size() / stream.frame_size();
          const size_t take = std::min(frames, room);
          if (frames) {
            if (!primed) { std::copy_n(source.begin(), ch, last.begin()); primed = true; }
            for (size_t i = 0; i < take; ++i)
              for (uint32_t c = 0; c < ch; ++c)
                EncodeExpected(i == 0 ? last[c] : source[(i - 1) * ch + c], format, &queue);
            std::copy_n(source.end() - ch, ch, last.begin());
          }
          stream.Push(source.data(), frames, 48000, ch, 1000 + static_cast<int64_t>(block));
          Check(source == original, "input samples remain untouched");
          produced += take; lost += static_cast<uint32_t>(frames - take);
          Check(stream.frames_captured() == produced && stream.overrun_frames() == lost,
                "capture count and lost frames retain full-block accounting");
          const size_t request = block % 2 ? queue.size() + stream.frame_size() - 1 : stream.frame_size();
          std::vector<uint8_t> got(request + 2, 0xa7);
          const size_t expected = std::min(queue.size(), request / stream.frame_size() * stream.frame_size());
          Check(stream.Read(got.data() + 1, request, false) == static_cast<ssize_t>(expected),
                "capture Read returns only complete available frames");
          Check(std::equal(queue.begin(), queue.begin() + expected, got.begin() + 1),
                "independent finite capture samples");
          Check(got.front() == 0xa7 && std::all_of(got.begin() + 1 + expected, got.end(),
                [](uint8_t b) { return b == 0xa7; }), "capture Read preserves output guards and suffix");
          Write(transcript, got.data(), got.size());
          queue.erase(queue.begin(), queue.begin() + expected);
          consumed += expected / stream.frame_size();
          Check(stream.frames_read() == consumed && stream.Available() == queue.size() / stream.frame_size(),
                "capture ring positions match independent queue");
          Record(transcript, stream); ++block;
        }
        stream.Release(); Record(transcript, stream);
      }
    }
  }
}

// A scalar point oracle keeps a runtime interpolation weight. Comparing the
// selected baseline also checks all NaN payload bits and floating-point flags.
[[gnu::noinline]] float Point(float before, float after, float weight) {
  volatile float fraction = weight;
  return std::clamp(before + (after - before) * fraction, -1.0f, 1.0f);
}

void FloatEdges(FILE* transcript) {
  constexpr uint32_t bits[]{0, 0x80000000u, 0x3f800000u, 0xbf800000u,
      1, 0x80000001u, 0x007fffffu, 0x00800000u, 0x7f7fffffu, 0xff7fffffu,
      0x7f800000u, 0xff800000u, 0x7fc00001u, 0xffc12345u, 0x7f800001u, 0xff800123u};
  const int original_round = std::fegetround();
  for (int rounding : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
    Check(std::fesetround(rounding) == 0, "set explicit float-oracle rounding mode");
    for (double phase : {0.0, -0.0, 0.25}) {
      for (uint32_t previous_bits : bits) {
        for (uint32_t current_bits : bits) {
          CaptureParams params; params.format = PcmFormat::kFloat; params.capacity_frames = 4;
          CaptureStream stream(&engine, params); stream.Start();
          stream.primed_ = true; stream.phase_ = phase;
          stream.last_[0] = std::bit_cast<float>(previous_bits);
          const std::array<float, 2> source{std::bit_cast<float>(current_bits), -0.125f};
          const auto saved = std::bit_cast<std::array<uint32_t, 2>>(source);
          std::array<float, 2> expected;
          double position = phase;
          for (size_t i = 0; i < expected.size(); ++i) {
            const size_t index = static_cast<size_t>(position);
            const float weight = static_cast<float>(position - static_cast<double>(index));
            expected[i] = Point(index == 0 ? stream.last_[0] : source[index - 1], source[index], weight);
            position += 1.0;
          }
          std::feclearexcept(FE_ALL_EXCEPT);
          stream.Push(source.data(), source.size(), 48000, 1, 917);
          const int raised = std::fetestexcept(FE_ALL_EXCEPT);
          Field(transcript, rounding); Field(transcript, phase); Field(transcript, raised);
          Check(std::bit_cast<std::array<uint32_t, 2>>(source) == saved, "float-edge source bits remain unchanged");
          std::array<float, 2> result{};
          Check(stream.Read(result.data(), sizeof(result), false) == static_cast<ssize_t>(sizeof(result)),
                "float-edge capture emits both frames");
          for (size_t i = 0; i < result.size(); ++i) {
            const bool same = std::isnan(expected[i]) ? std::isnan(result[i]) :
                std::bit_cast<uint32_t>(expected[i]) == std::bit_cast<uint32_t>(result[i]);
            if (!same) std::fprintf(stderr,
                "FLOAT_EDGE rounding=%d phase=%a before=%08x current=%08x frame=%zu expected=%08x actual=%08x\n",
                rounding, phase, previous_bits, current_bits, i,
                std::bit_cast<uint32_t>(expected[i]), std::bit_cast<uint32_t>(result[i]));
            Check(same, "IEEE float interpolation preserves values and signed zero");
          }
          Write(transcript, result.data(), sizeof(result)); Record(transcript, stream);
        }
      }
    }
  }
  Check(std::fesetround(original_round) == 0, "restore float rounding mode");
}

void FallbackSamples(FILE* transcript) {
  struct Case {
    uint32_t source_channels, channels, rate;
    double phase;
    std::vector<float> expected;
  };
  const Case cases[]{
      {1, 2, 48000, 0, {0.5f, -0.25f, 0.25f, 0.25f, -0.5f, -0.5f}},
      {2, 1, 48000, 0, {0.5f, -0.125f, 0.4375f}},
      {2, 2, 96000, 0, {0.5f, -0.25f, 0.75f, 0.125f}},
      {2, 2, 48000, 0.5, {0.375f, -0.375f, 0.5f, -0.1875f, 0.25f, 0.3125f}}};
  // Extra initialized input is a guard: incorrect channel selection must fail
  // the value assertion rather than reading outside this test-owned array.
  const std::array<float, 16> source{0.25f, -0.5f, 0.75f, 0.125f, -0.25f, 0.5f,
      -0.75f, 0.375f, -0.125f, 0.875f, 0, 0, 0, 0, 0, 0};
  for (const auto& task : cases) {
    CaptureParams params; params.channels = task.channels; params.format = PcmFormat::kFloat;
    params.capacity_frames = 8;
    CaptureStream stream(&engine, params); stream.Start();
    stream.primed_ = true; stream.phase_ = task.phase;
    stream.last_[0] = 0.5f; if (task.channels == 2) stream.last_[1] = -0.25f;
    stream.Push(source.data(), 3, task.rate, task.source_channels, 101);
    std::array<float, 16> actual{};
    const auto count = stream.Read(actual.data(), sizeof(actual), false);
    Check(count == static_cast<ssize_t>(task.expected.size() * sizeof(float)) &&
          std::equal(task.expected.begin(), task.expected.end(), actual.begin()),
          "independent channel rate and fractional-phase fallback samples");
    Field(transcript, count); Write(transcript, actual.data(), static_cast<size_t>(count));
    Record(transcript, stream);
  }
}

void LifecycleTransitions(FILE* transcript) {
  CaptureParams params; params.channels = 2; params.format = PcmFormat::kFloat;
  params.capacity_frames = 7;
  CaptureStream stream(&engine, params);
  const auto source = Pattern(19, 8, 8);
  std::vector<uint32_t> callbacks;
  stream.SetMarker(4); stream.SetUpdatePeriod(2);
  stream.SetEvents({[&](uint32_t p) { callbacks.push_back(0x10000000 | p); },
      [&](uint32_t p) { callbacks.push_back(0x20000000 | p); },
      [&] { callbacks.push_back(0x30000000); }});
  stream.Push(source.data(), 3, 48000, 2, 1);
  Check(stream.Available() == 0 && callbacks.empty(), "stopped capture ignores input and callbacks");
  Check(stream.Start() == kOk, "lifecycle capture starts");
  const auto first_generation = stream.generation_;
  stream.Push(source.data(), 3, 48000, 2, 2, first_generation);
  Check(stream.frames_captured() == 3 && callbacks.size() == 2, "current generation publishes data and events");
  const auto prior_history = stream.last_;
  stream.Push(source.data(), 3, 48000, 2, 3, first_generation - 1);
  Check(stream.frames_captured() == 3 && stream.last_ == prior_history && stream.last_ns_ == 2 &&
        callbacks.size() == 2, "stale capture generation preserves all pending samples and events");
  Record(transcript, stream);
  stream.Stop(); const auto wake = stream.StateWakeSequence();
  stream.Push(source.data(), 3, 48000, 2, 4, first_generation);
  Check(stream.frames_captured() == 3 && stream.StateWakeSequence() == wake,
        "stopped generation cannot publish or fabricate another state wake");
  Check(stream.Start() == kOk && stream.Available() == 0 && !stream.primed_ && stream.phase_ == 0,
        "restart drops old queued samples and resets resampling history");
  const auto generation = stream.generation_;
  stream.Push(source.data(), 3, 48000, 2, 5, first_generation);
  Check(stream.Available() == 0 && !stream.primed_, "previous start cannot prime the restarted capture");
  std::array<float, 16> output{};
  size_t at = 0;
  for (auto input : {std::pair{48000u, 2u}, {44100u, 2u}, {48000u, 2u},
                     {48000u, 1u}, {96000u, 2u}, {48000u, 8u}, {48000u, 2u}}) {
    stream.Push(source.data(), 5, input.first, input.second, 10 + at++, generation);
    const auto count = stream.Read(output.data(), sizeof(output), false);
    Check(count > 0 && count % static_cast<ssize_t>(stream.frame_size()) == 0,
          "source format transition emits only complete real frames");
    Field(transcript, count); Write(transcript, output.data(), static_cast<size_t>(count));
    Record(transcript, stream);
  }
  stream.Push(source.data(), 3, 48000, 2, 25, generation);
  const auto available = stream.Available(), produced = stream.frames_captured();
  const auto event_count = callbacks.size();
  stream.Fail(kCaptureDeadObject);
  stream.Push(source.data(), 3, 48000, 2, 26, generation);
  Check(stream.frames_captured() == produced && callbacks.size() == event_count &&
        !stream.started() && stream.error() == kCaptureDeadObject,
        "terminal source failure cannot publish further samples or events");
  Check(stream.Read(output.data(), sizeof(output), false) ==
        static_cast<ssize_t>(available * stream.frame_size()), "failure preserves already captured samples");
  Write(transcript, output.data(), available * stream.frame_size());
  Check(stream.Read(output.data(), sizeof(output), false) == kCaptureDeadObject &&
        stream.Start() == kCaptureDeadObject, "terminal failure remains visible to reads and restart");
  Record(transcript, stream);
  stream.Release(); const auto released_wake = stream.StateWakeSequence();
  stream.Push(source.data(), 3, 48000, 2, 27);
  stream.Release();
  Check(stream.Start() == kInvalidOperation && stream.frames_captured() == produced &&
        stream.converted_.capacity() == 0 && stream.StateWakeSequence() == released_wake,
        "release returns scratch storage and is terminal and idempotent");
  Record(transcript, stream);
  Field(transcript, callbacks.size()); Write(transcript, callbacks.data(), callbacks.size() * sizeof(uint32_t));
}

void WorkBudget() {
  for (uint32_t ch : {1u, 2u, 8u}) {
    CaptureParams params; params.channels = ch; params.capacity_frames = 1024;
    CaptureStream stream(&engine, params); stream.Start();
    auto input = Pattern(0, 480, ch);
    std::vector<uint8_t> output(480 * stream.frame_size());
    stream.Push(input.data(), 480, 48000, ch, 1); stream.Read(output.data(), output.size(), false);
    const auto before = capture_channel_frames;
    stream.Push(input.data(), 480, 48000, ch, 2);
#ifdef GESHER_COUNT_CAPTURE
    if (efficiency_gate) Check(capture_channel_frames - before == 1, "unity capture extracts only final channel history");
#endif
    Check(stream.Read(output.data(), output.size(), false) == static_cast<ssize_t>(output.size()),
          "budgeted capture emits every real frame");
    std::printf("CAPTURE_WORK channels=%u frames=480 channel_frames=%llu\n", ch,
                static_cast<unsigned long long>(capture_channel_frames - before));
  }
}

uint64_t CpuNanos() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  Check(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user), "read process CPU clock");
  ULARGE_INTEGER k, u; k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
  u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
  return (k.QuadPart + u.QuadPart) * 100;
#else
  timespec t{}; Check(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) == 0, "read process CPU clock");
  return uint64_t(t.tv_sec) * 1000000000 + uint64_t(t.tv_nsec);
#endif
}
uint64_t WallNanos() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr size_t kClipFrames = 4093;
void MakeWav(const std::string& path, uint32_t channels) {
  const auto samples = Pattern(0, kClipFrames, channels);
  FILE* file = std::fopen(path.c_str(), "wb"); Check(file != nullptr, "create explicit owned WAV fixture");
  Write(file, "RIFF", 4); Field<uint32_t>(file, 36 + static_cast<uint32_t>(samples.size() * 4));
  Write(file, "WAVEfmt ", 8); Field<uint32_t>(file, 16); Field<uint16_t>(file, 3);
  Field<uint16_t>(file, static_cast<uint16_t>(channels)); Field<uint32_t>(file, 48000);
  Field<uint32_t>(file, 48000 * channels * 4); Field<uint16_t>(file, static_cast<uint16_t>(channels * 4));
  Field<uint16_t>(file, 32); Write(file, "data", 4); Field<uint32_t>(file, static_cast<uint32_t>(samples.size() * 4));
  Write(file, samples.data(), samples.size() * 4);
  Check(std::fclose(file) == 0, "close complete WAV fixture");
}

struct Workload {
  const char* name;
  uint32_t source_channels, channels, rate;
  PcmFormat format;
  size_t capacity, blocks;
  size_t instances = 1;
};
void RunWorkload(const Workload& task, const std::string& path, FILE* transcript) {
  std::vector<uint8_t> output(task.capacity * task.channels * BytesPerSample(task.format));
  uint64_t checksum = 0, read_frames = 0;
  CaptureStats stats;
  const auto before_channels = capture_channel_frames;
  const auto cpu = CpuNanos(), wall = WallNanos();
  for (size_t instance = 0; instance < task.instances; ++instance) {
    uint64_t stream_read_frames = 0;
    std::string error;
    auto source = OpenFileCaptureFixture(path, &error);
    Check(source != nullptr, error.c_str());
    Check(source->rate() == 48000 && source->channels() == task.source_channels,
          "actual WAV provider publishes encoded format");
    CaptureParams params; params.channels = task.channels; params.sample_rate = task.rate;
    params.format = task.format; params.capacity_frames = task.capacity;
    CaptureStream stream(&engine, params);
    capture_source_clock_ns = 1000000000;
    Check(source->Start() && stream.Start() == kOk, "actual source and capture stream start");
    std::vector<float> input;
    for (size_t block = 0; block < task.blocks; ++block) {
      capture_source_clock_ns = 1000000000 + static_cast<int64_t>(block + 1) * 10000000;
      input.clear();
      Check(source->Pull(&input) && input.size() == 480 * task.source_channels,
            "actual paced WAV source returns the complete due block");
      stream.Push(input.data(), 480, source->rate(), source->channels(), capture_source_clock_ns);
      const auto got = stream.Read(output.data(), output.size(), false);
      Check(got >= 0 && static_cast<size_t>(got) % stream.frame_size() == 0,
            "whole-workload reader consumes only complete frames");
      stream_read_frames += static_cast<size_t>(got) / stream.frame_size();
      read_frames += static_cast<size_t>(got) / stream.frame_size();
      checksum += static_cast<uint64_t>(got);
      if (got) checksum += output[block % static_cast<size_t>(got)];
      if (transcript) {
        if (task.rate == 48000 && task.source_channels == task.channels) {
          std::vector<uint8_t> expected;
          for (size_t frame = 0; frame < std::min<size_t>(480, task.capacity); ++frame) {
            const size_t index = block == 0 && frame == 0 ? 0 : (block * 480 + frame - 1) % kClipFrames;
            const auto sample = Pattern(index * 13, 1, task.channels);
            for (float value : sample) EncodeExpected(value, task.format, &expected);
          }
          Check(expected.size() == static_cast<size_t>(got) && std::equal(expected.begin(), expected.end(), output.begin()),
                "actual paced source and Push Read match every independent sample");
        }
        Field(transcript, got); Write(transcript, output.data(), static_cast<size_t>(got)); Record(transcript, stream);
      }
    }
    const auto current_stats = stream.stats();
    Check(current_stats.captured_frames == stream_read_frames && current_stats.read_frames == stream_read_frames,
          "whole-workload counters equal all emitted and consumed frames");
    stats.captured_frames += current_stats.captured_frames;
    stats.read_frames += current_stats.read_frames;
    stats.lost_frames += current_stats.lost_frames;
    source->Stop(); stream.Release(); source.reset();
    if (transcript) Record(transcript, stream);
  }
  const auto wall_ns = WallNanos() - wall, cpu_ns = CpuNanos() - cpu;
  std::printf("CAPTURE_STREAM %s instances=%zu blocks=%zu frames=%llu lost=%u channel_frames=%llu cpu_ns=%llu wall_ns=%llu checksum=%llu\n",
      task.name, task.instances, task.blocks, static_cast<unsigned long long>(read_frames), stats.lost_frames,
      static_cast<unsigned long long>(capture_channel_frames - before_channels),
      static_cast<unsigned long long>(cpu_ns), static_cast<unsigned long long>(wall_ns),
      static_cast<unsigned long long>(checksum));
}

void Workloads(const std::string& directory, FILE* transcript, bool measured) {
  for (uint32_t channels : {1u, 2u, 8u})
    MakeWav(directory + "/capture-unity-" + std::to_string(channels) + ".wav", channels);
  const Workload cases[]{
      {"cold-stereo", 2, 2, 48000, PcmFormat::kS16, 1024, 1},
      {"small-streams", 2, 2, 48000, PcmFormat::kS16, 1024, 1, 1000},
      {"mono48-s16", 1, 1, 48000, PcmFormat::kS16, 1024, 30000},
      {"stereo48-s16", 2, 2, 48000, PcmFormat::kS16, 1024, 30000},
      {"eight48-s16", 8, 8, 48000, PcmFormat::kS16, 1024, 10000},
      {"stereo48-float", 2, 2, 48000, PcmFormat::kFloat, 1024, 30000},
      {"resample44-control", 2, 2, 44100, PcmFormat::kS16, 1024, 10000},
      {"downmix-control", 2, 1, 48000, PcmFormat::kS16, 1024, 30000},
      {"overflow-stereo", 2, 2, 48000, PcmFormat::kS16, 37, 10000}};
  for (auto task : cases) {
    if (!measured) { task.blocks = std::min<size_t>(task.blocks, 12); task.instances = std::min<size_t>(task.instances, 3); }
    RunWorkload(task, directory + "/capture-unity-" + std::to_string(task.source_channels) + ".wav", transcript);
  }
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc <= 3, "supported fixture arguments");
  const std::string mode = argc > 1 ? argv[1] : "gate";
  const std::string directory = argc > 2 ? argv[2] : ".";
  Check(mode == "gate" || mode == "reference" || mode == "measure", "explicit fixture mode");
  efficiency_gate = mode != "reference";
  if (mode == "measure") {
    Workloads(directory, nullptr, true);
  } else {
    const std::string file = directory + "/transcript.bin";
    FILE* transcript = std::fopen(file.c_str(), "wb"); Check(transcript != nullptr, "open capture transcript");
    FiniteBlocks(transcript);
    FloatEdges(transcript);
    FallbackSamples(transcript);
    LifecycleTransitions(transcript);
    WorkBudget();
    Workloads(directory, transcript, false);
    Check(std::fclose(transcript) == 0, "close capture transcript");
  }
  std::printf("CAPTURE_UNITY_PASS checks=%zu\n", checks);
  return 0;
}
