// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Production factory, declared backend availability/creation observers.
#include "compat/media/encode/encoder.h"
#include <cassert>
#include <iostream>
namespace gesher::media {
int queried = 0, created = 0;
bool MfH264EncoderAvailable() { return false; }
bool MfAacEncoderAvailable() { ++queried; return true; }
std::unique_ptr<VideoEncoder> CreateMfH264Encoder(const VideoEncoderParams&, std::string*) { return nullptr; }
std::unique_ptr<VideoEncoder> CreateSoftAvcEncoder(const VideoEncoderParams&, std::string*) { return nullptr; }
std::unique_ptr<AudioEncoder> CreateOpusEncoder(const AudioEncoderParams&, std::string*) { return nullptr; }
std::unique_ptr<AudioEncoder> CreateMfAacEncoder(const AudioEncoderParams& p, std::string*) {
  assert(p.aac_profile == 2); ++created; return nullptr;
}
std::vector<std::pair<int, int>> ProfileLevels(const std::string&, bool);
}
int main() {
  using namespace gesher::media;
  for (int profile : {-1, 0, 1, 5, 29, 39, 77}) {
    AudioEncoderParams p; p.aac_profile = profile;
    std::string error;
    assert(!AudioEncoder::Create("audio/mp4a-latm", p, &error));
    assert(!error.empty() && queried == 0 && created == 0);
  }
  AudioEncoderParams lc;
  std::string error;
  AudioEncoder::Create("audio/mp4a-latm", lc, &error);
  assert(queried == 1 && created == 1);
  const std::vector<std::pair<int, int>> encoder = {{2, 0}}, decoder = {{2, 0}, {5, 0}, {29, 0}};
  assert(ProfileLevels("audio/mp4a-latm", true) == encoder);
  assert(ProfileLevels("audio/mp4a-latm", false) == decoder);
  std::cout << "Encoder selection: unsupported AAC profiles rejected before backend; LC delegated; capabilities match role\n";
}
