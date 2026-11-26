// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstddef>
#include <cstdint>

struct Spec {
  unsigned channels;
  unsigned input_rate;
  unsigned output_rate;
  unsigned frames;
  unsigned block;
  unsigned capacity;
  float speed;
  bool streaming;
};
struct FinalState {
  uint64_t position;
  uint64_t marker;
  uint64_t updates;
  uint64_t end;
  uint64_t underruns;
  uint64_t queued;
};
extern "C" {
void* track_create(const Spec* spec);
size_t track_write(void* handle, const float* data, size_t frames);
void track_start(void* handle);
void track_stop(void* handle);
void track_mix(void* handle, float* output, unsigned frames, unsigned rate, int64_t time_ns);
bool track_ended(void* handle);
FinalState track_final(void* handle);
void track_destroy(void* handle);
}
