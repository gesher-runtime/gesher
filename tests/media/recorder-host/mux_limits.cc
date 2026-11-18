// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Production container writers, genuine encoded fixtures; only Format is a host seam.
#include "compat/media/mux/muxer.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>

struct AMediaFormat {
  std::string mime;
  std::map<std::string, int32_t> integers;
  std::map<std::string, std::vector<uint8_t>> buffers;
};
namespace gesher::media {
Format::Format() : f_(new AMediaFormat) {}
Format::~Format() { delete f_; }
std::string Format::Mime() const { return f_->mime; }
bool Format::GetInt32(const char* key, int32_t* out) const {
  auto it = f_->integers.find(key);
  if (it == f_->integers.end()) return false;
  *out = it->second; return true;
}
bool Format::GetBuffer(const char* key, std::vector<uint8_t>* out) const {
  auto it = f_->buffers.find(key);
  if (it == f_->buffers.end()) return false;
  *out = it->second; return true;
}
}
using namespace gesher::media;
using Packets = std::vector<std::vector<uint8_t>>;
std::vector<uint8_t> Read(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  assert(in);
  return {std::istreambuf_iterator<char>(in), {}};
}
Packets Units(const std::string& path) {
  auto bytes = Read(path);
  Packets result;
  for (size_t at = 0; at < bytes.size();) {
    assert(at + 4 <= bytes.size());
    const size_t size = bytes[at] | (bytes[at+1] << 8) | (bytes[at+2] << 16) | (bytes[at+3] << 24);
    at += 4; assert(size && size <= bytes.size() - at);
    result.emplace_back(bytes.begin() + at, bytes.begin() + at + size); at += size;
  }
  assert(!result.empty()); return result;
}
struct Case {
  const char* name;
  MuxerFormat format;
  bool opus = false, video = false, mixed = false, irregular = false, delayed = false, location = false;
  int64_t interval_us = 20000;
};
struct Result { uint64_t bytes; size_t count; };
Result Run(const std::string& path, const Case& c, int64_t limit, size_t maximum,
           const Packets& aac, const Packets& opus, const std::vector<uint8_t>& head,
           const std::vector<uint8_t>& avc, bool require_limit) {
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
  std::string error;
  auto mux = Muxer::Create(fd, c.format, &error); assert(mux);
  Format audio, video;
  audio.get()->mime = c.opus ? "audio/opus" : "audio/mp4a-latm";
  audio.get()->integers = {{"sample-rate", 48000}, {"channel-count", 1}};
  if (c.opus) audio.get()->buffers["csd-0"] = head;
  else audio.get()->buffers["csd-0"] = {0x11, 0x88};
  video.get()->mime = "video/avc";
  video.get()->integers = {{"width", 32}, {"height", 32}};
  int at = -1, vt = -1;
  if (!c.video || c.mixed) at = mux->AddTrack(audio, &error);
  if (c.video || c.mixed) vt = mux->AddTrack(video, &error);
  assert((at >= 0 || vt >= 0) && (!c.mixed || (at >= 0 && vt >= 0)));
  if (c.location) mux->SetLocation(-899999, 1799999);
  mux->SetOrientationHint(90);
  assert(mux->SetFileSizeLimit(limit));
  assert(mux->Start(&error));
  size_t written = 0;
  for (size_t i = 0; i < maximum; ++i) {
    const bool v = c.video || (c.mixed && i % 3 == 1);
    const auto& packets = c.opus ? opus : aac;
    const auto& data = v ? avc : packets[i % packets.size()];
    int64_t time = static_cast<int64_t>(i) * c.interval_us;
    if (c.irregular) time += static_cast<int64_t>(i % 3) * 1000;
    if (c.delayed && v) time += 1000000;
    assert(mux->WriteSample(v ? vt : at, data.data(), data.size(), time,
                            v && i % 2 == 0 ? 1 : 0, &error));
    if (mux->FileSizeLimitReached()) break;
    ++written;
  }
  if (require_limit) assert(mux->FileSizeLimitReached());
  assert(written > 0);
  assert(mux->Stop(&error));
  struct stat info {}; assert(fstat(fd, &info) == 0);
  assert(mux->BytesWritten() == info.st_size);
  assert(limit <= 0 || info.st_size < limit);
  close(fd);
  std::cout << c.name << " limit=" << limit << " bytes=" << info.st_size << " packets=" << written << '\n';
  return {static_cast<uint64_t>(info.st_size), written};
}
int main(int argc, char** argv) {
  assert(argc == 2);
  std::string work = argv[1];
  const auto aac = Units(work + "/aac.units"), opus = Units(work + "/opus.units");
  const auto head = Read(work + "/opus.head"), avc = Read(work + "/frame.h264");
  const Case cases[] = {
    {"aac.mp4", MuxerFormat::kMpeg4},
    {"irregular.mp4", MuxerFormat::kMpeg4, false, false, false, true, false, true},
    {"opus.mp4", MuxerFormat::kMpeg4, true},
    {"audio.3gp", MuxerFormat::kThreeGpp},
    {"video.mp4", MuxerFormat::kMpeg4, false, true, false, true},
    {"mixed.mp4", MuxerFormat::kMpeg4, false, false, true, true, true, true},
    {"audio.ogg", MuxerFormat::kOgg, true},
    {"audio.aac", MuxerFormat::kAdts},
    {"audio.webm", MuxerFormat::kWebm, true},
    {"clusters.webm", MuxerFormat::kWebm, true, false, false, false, false, false, 40000000},
  };
  for (const Case& c : cases) {
    // Two tracks need room for both codec headers and at least one packet
    // each; use 4 KiB for that case across FFmpeg/x264 fixture versions.
    for (int64_t limit : {c.mixed ? 4096 : 2048, 8192, 131072})
      Run(work + "/" + std::to_string(limit) + "-" + c.name, c, limit, 250000, aac, opus, head, avc, true);
    // MP4's incremental table count is exact, including the first-duration
    // replacement, stts runs, stss keys, codec metadata, location and late tracks.
    if (c.format == MuxerFormat::kMpeg4 || c.format == MuxerFormat::kThreeGpp) {
      for (size_t count : {size_t{1}, size_t{2}, size_t{3}, size_t{17}}) {
        if (c.mixed && count < 3) continue;
        auto baseline = Run(work + "/baseline-" + c.name, c, 0, count, aac, opus, head, avc, false);
        auto exact = Run(work + "/exact-" + c.name, c, baseline.bytes + 1, count + 1, aac, opus, head, avc, true);
        assert(exact.count == count && exact.bytes == baseline.bytes);
      }
    }
  }
  std::cout << "Recorder completed-container budgets passed\n";
}
