// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Production factory, declared backend availability/creation observers.
#include "compat/media/encode/encoder.h"
#include <cassert>
#include <iostream>
namespace gesher::media {
int queried = 0, created = 0;
int mf_video_created = 0, soft_video_created = 0;
bool MfH264EncoderAvailable() { return true; }
bool MfAacEncoderAvailable() { ++queried; return true; }
std::unique_ptr<VideoEncoder> CreateMfH264Encoder(const VideoEncoderParams&, std::string* error) {
  ++mf_video_created;
  *error = "stub MFT rejected the format";
  return nullptr;
}
std::unique_ptr<VideoEncoder> CreateSoftAvcEncoder(const VideoEncoderParams& p, std::string*) {
  assert(p.profile == 0 || p.profile == kAvcProfileBaseline);
  ++soft_video_created;
  return nullptr;
}
std::unique_ptr<AudioEncoder> CreateOpusEncoder(const AudioEncoderParams&, std::string*) { return nullptr; }
std::unique_ptr<AudioEncoder> CreateMfAacEncoder(const AudioEncoderParams& p, std::string*) {
  assert(p.aac_profile == 2); ++created; return nullptr;
}
std::vector<std::pair<int, int>> ProfileLevels(const std::string&, bool, int);
}
int main(int argc, char** argv) {
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
  assert(ProfileLevels("audio/mp4a-latm", true, 0) == encoder);
  assert(ProfileLevels("audio/mp4a-latm", false, 0) == decoder);

  assert(argc == 2);
  const bool soft = std::string(argv[1]) == "soft";
  const EncoderInfo& avc = AvailableEncoders().front();
  assert(avc.mime == "video/avc" && avc.backend == (soft ? "soft" : "mf"));
  std::vector<std::pair<int, int>> expected = {{kAvcProfileBaseline, kAvcLevel41}};
  if (!soft) {
    expected.emplace_back(kAvcProfileMain, kAvcLevel41);
    expected.emplace_back(kAvcProfileHigh, kAvcLevel41);
  }
  assert(ProfileLevels(avc.mime, true, avc.avc_profiles) == expected);
  VideoEncoderParams video;
  video.width = 320;
  video.height = 240;
  for (int profile : {-1, 3, 16, 0x10000, 0x80000}) {
    video.profile = profile;
    error.clear();
    assert(!VideoEncoder::Create(avc.mime, video, &error) && !error.empty());
    assert(mf_video_created == 0 && soft_video_created == 0);
  }
  for (int profile : {kAvcProfileMain, kAvcProfileHigh}) {
    video.profile = profile;
    error.clear();
    assert(!VideoEncoder::Create(avc.mime, video, &error) && !error.empty());
    assert(soft_video_created == 0);
  }
  assert(mf_video_created == (soft ? 0 : 2));
  for (int profile : {0, kAvcProfileBaseline}) {
    video.profile = profile;
    VideoEncoder::Create(avc.mime, video, &error);
  }
  assert(mf_video_created == (soft ? 0 : 4) && soft_video_created == 2);
  std::cout << "Encoder selection: AAC roles and " << avc.backend
            << " AVC declarations, admission and fallback passed\n";
}
