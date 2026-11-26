// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#include "audio_resampler_weights_bench.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>
#include <vector>

uint64_t Now(clockid_t id) {
  timespec value{};
  if (clock_gettime(id, &value)) std::abort();
  return static_cast<uint64_t>(value.tv_sec) * 1000000000 + value.tv_nsec;
}
uint64_t Word(uint64_t hash, uint64_t value) {
  return (hash ^ value) * 1099511628211ULL;
}
struct Result { uint64_t hash = 1469598103934665603ULL, mixes = 0, inputs = 0; };
Result Cycle(const Spec& spec, const std::vector<float>& input, std::vector<float>& output) {
  Result result;
  void* track = track_create(&spec);
  if (spec.frames == 0) {
    // Valid one-frame-capacity owner, with no input or mix. This control
    // includes ordinary construction/start/stop/release/destruction only.
    track_start(track); track_stop(track);
    FinalState state = track_final(track);
    if (state.position || state.end || state.queued || state.underruns) std::abort();
    track_destroy(track);
    return result;
  }
  size_t committed = track_write(track, input.data(), spec.frames);
  track_start(track);
  bool stopped = false;
  if (!spec.streaming && committed != spec.frames) std::abort();
  while (!track_ended(track)) {
    if (spec.streaming) {
      if (committed < spec.frames)
        committed += track_write(track, input.data() + committed * spec.channels, spec.frames - committed);
      if (committed == spec.frames && !stopped) { track_stop(track); stopped = true; }
    }
    std::fill(output.begin(), output.end(), 0.0f);
    const int64_t time_ns = 1 + static_cast<int64_t>(result.mixes * spec.block) * 1000000000 / spec.output_rate;
    track_mix(track, output.data(), spec.block, spec.output_rate, time_ns);
    for (float sample : output) {
      uint32_t bits;
      std::memcpy(&bits, &sample, sizeof bits);
      result.hash = Word(result.hash, bits);
    }
    ++result.mixes;
    if (result.mixes > 100000) std::abort();
  }
  FinalState state = track_final(track);
  if (committed != spec.frames || state.position != spec.frames || state.end != 1 ||
      state.underruns != 0 || state.queued != 0) {
    std::fprintf(stderr, "bad transfer committed=%zu position=%llu end=%llu underruns=%llu queued=%llu\n",
        committed, static_cast<unsigned long long>(state.position), static_cast<unsigned long long>(state.end),
        static_cast<unsigned long long>(state.underruns), static_cast<unsigned long long>(state.queued));
    std::abort();
  }
  for (uint64_t value : {state.position, state.marker, state.updates, state.end, state.underruns, state.queued})
    result.hash = Word(result.hash, value);
  result.inputs = committed;
  track_destroy(track);
  return result;
}

int main(int argc, char** argv) {
  if (argc != 9) return 2;
  Spec spec{static_cast<unsigned>(std::stoul(argv[1])), static_cast<unsigned>(std::stoul(argv[2])),
            static_cast<unsigned>(std::stoul(argv[3])), static_cast<unsigned>(std::stoul(argv[4])),
            128, 960, std::strtof(argv[5], nullptr), std::stoi(argv[6]) != 0};
  const unsigned cycles = static_cast<unsigned>(std::stoul(argv[7]));
  spec.block = static_cast<unsigned>(std::stoul(argv[8]));
  if (!spec.channels || spec.channels > 24 || (!spec.frames && spec.streaming) || !spec.block || !cycles) return 2;
  std::vector<float> input(spec.frames * spec.channels);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<float>(static_cast<int>((i * 997 + i / spec.channels * 31) % 65535) - 32767) / 65536.0f;
  std::vector<float> output(spec.block * 2);
  Result expected;
  for (unsigned phase = 0; phase < 2; ++phase) {
    const unsigned count = phase == 0 ? 1 : cycles;
    const uint64_t wall_start = Now(CLOCK_MONOTONIC), process_start = Now(CLOCK_PROCESS_CPUTIME_ID),
                   cpu_start = Now(CLOCK_THREAD_CPUTIME_ID);
    Result aggregate;
    for (unsigned iteration = 0; iteration < count; ++iteration) {
      Result value = Cycle(spec, input, output);
      if (phase == 0) expected = value;
      else if (value.hash != expected.hash || value.mixes != expected.mixes || value.inputs != expected.inputs) std::abort();
      aggregate.hash = Word(aggregate.hash, value.hash);
      aggregate.mixes += value.mixes;
      aggregate.inputs += value.inputs;
    }
    const uint64_t cpu_ns = Now(CLOCK_THREAD_CPUTIME_ID) - cpu_start,
                   process_ns = Now(CLOCK_PROCESS_CPUTIME_ID) - process_start,
                   wall_ns = Now(CLOCK_MONOTONIC) - wall_start;
    if (process_ns < cpu_ns) {
      std::fprintf(stderr, "process CPU interval does not cover its sole timed thread\n"); return 3;
    }
    std::printf("{\"phase\":\"%s\",\"cycles\":%u,\"channels\":%u,\"input_rate\":%u,\"output_rate\":%u,\"frames\":%u,\"speed\":%.9g,\"streaming\":%s,\"cpu_ns\":%llu,\"process_ns\":%llu,\"wall_ns\":%llu,\"hash\":%llu,\"mixes\":%llu,\"inputs\":%llu}\n",
      phase ? "steady" : "first", count, spec.channels, spec.input_rate, spec.output_rate, spec.frames,
      spec.speed, spec.streaming ? "true" : "false", static_cast<unsigned long long>(cpu_ns),
      static_cast<unsigned long long>(process_ns), static_cast<unsigned long long>(wall_ns), static_cast<unsigned long long>(aggregate.hash),
      static_cast<unsigned long long>(aggregate.mixes), static_cast<unsigned long long>(aggregate.inputs));
  }
}
