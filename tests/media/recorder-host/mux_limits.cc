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
  int64_t delay_us = 1000000;
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
    if (c.delayed && v) time += c.delay_us;
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

uint64_t MetadataSize(const std::string& path, int64_t limit, bool eos_rejected,
                      const std::vector<uint8_t>& avc) {
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
  std::string error;
  auto mux = Muxer::Create(fd, MuxerFormat::kMpeg4, &error); assert(mux);
  Format video, metadata, late;
  video.get()->mime = "video/avc";
  video.get()->integers = {{"width", 32}, {"height", 32}};
  metadata.get()->mime = "application/fixture";
  late.get()->mime = "application/late-fixture";
  const int vt = mux->AddTrack(video, &error);
  const int mt = mux->AddTrack(metadata, &error);
  const int lt = mux->AddTrack(late, &error);
  assert(vt == 0 && mt == 1 && lt == 2);
  assert(mux->SetFileSizeLimit(limit) && mux->Start(&error));
  const uint8_t bytes[] = {1, 2, 3};
  // Negative preroll, a duration requiring wide mdhd, and an offset
  // requiring a wide edit list and movie/track headers.
  assert(mux->WriteSample(vt, avc.data(), avc.size(), 0, 1, &error));
  assert(mux->WriteSample(mt, bytes, sizeof(bytes), -1000000, 0, &error));
  assert(mux->WriteSample(mt, bytes, sizeof(bytes), 30000000000, 0, &error));
  assert(mux->WriteSample(mt, bytes, sizeof(bytes), 50000000000, 0, &error));
  assert(mux->WriteSample(lt, bytes, sizeof(bytes), 500000000000, 0, &error));
  assert(!mux->FileSizeLimitReached());
  // Explicit EOS replaces the last metadata interval with a new stts run.
  assert(mux->WriteSample(mt, nullptr, 0, 50001000000, 4, &error));
  assert(mux->FileSizeLimitReached() == eos_rejected);
  assert(mux->Stop(&error));
  struct stat info {}; assert(fstat(fd, &info) == 0);
  assert(mux->BytesWritten() == info.st_size);
  assert(limit <= 0 || info.st_size < limit);
  close(fd);
  return static_cast<uint64_t>(info.st_size);
}

int main(int argc, char** argv) {
  assert(argc == 2);
  std::string work = argv[1];
  const auto aac = Units(work + "/aac.units"), opus = Units(work + "/opus.units");
  const auto head = Read(work + "/opus.head"), avc = Read(work + "/frame.h264");
  const uint64_t metadata_size = MetadataSize(work + "/metadata-baseline.mp4", 0, false, avc);
  assert(MetadataSize(work + "/metadata-exact.mp4", metadata_size + 1, false, avc) == metadata_size);
  assert(Read(work + "/metadata-exact.mp4") == Read(work + "/metadata-baseline.mp4"));
  assert(MetadataSize(work + "/metadata-limited.mp4", metadata_size, true, avc) + 8 == metadata_size);
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
  // A few real packets exercise wide timing headers without recording or
  // decoding a long timeline. Every limit is checked against actual file size.
  Case timing[] = {
    {"wide-audio.mp4", MuxerFormat::kMpeg4},
    {"wide-video.mp4", MuxerFormat::kMpeg4, false, true},
    {"wide-edit.mp4", MuxerFormat::kMpeg4, false, false, true, false, true},
    {"submillisecond.mp4", MuxerFormat::kMpeg4, false, false, true, false, true},
  };
  timing[0].interval_us = 50000000000;
  timing[1].interval_us = 30000000000;
  timing[2].delay_us = 5000000000000;
  timing[3].interval_us = 0;
  timing[3].delay_us = 500;
  for (const Case& c : timing) {
    for (size_t count : {size_t{3}, size_t{17}}) {
      auto baseline = Run(work + "/timing-baseline-" + c.name, c, 0, count,
                          aac, opus, head, avc, false);
      auto exact = Run(work + "/timing-exact-" + c.name, c, baseline.bytes + 1, count + 1,
                       aac, opus, head, avc, true);
      assert(exact.count == count && exact.bytes == baseline.bytes);
    }
  }
  // A two-packet input exercises the timestamp range without a long run or a
  // large allocation. Failure leaves the accepted sample/file unchanged and
  // a following representable timestamp remains writable.
  for (int64_t first : {int64_t{0}, INT64_MIN + 1}) {
    const std::string path = work + (first == 0 ? "/timestamp-gap.mp4" : "/timestamp-span.mp4");
    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
    std::string error;
    auto mux = Muxer::Create(fd, MuxerFormat::kMpeg4, &error); assert(mux);
    Format audio; audio.get()->mime = "audio/mp4a-latm";
    audio.get()->integers = {{"sample-rate", 48000}, {"channel-count", 1}};
    audio.get()->buffers["csd-0"] = {0x11, 0x88};
    int track = mux->AddTrack(audio, &error); assert(track >= 0 && mux->Start(&error));
    const auto& packet = aac.front();
    assert(mux->WriteSample(track, packet.data(), packet.size(), first, 0, &error));
    const int64_t before = mux->BytesWritten();
    assert(!mux->WriteSample(track, packet.data(), packet.size(), INT64_MAX, 0, &error));
    assert(mux->BytesWritten() == before);
    assert(mux->WriteSample(track, packet.data(), packet.size(), first + 20000, 0, &error));
    assert(mux->Stop(&error));
    close(fd);
  }
  // Preserve metadata budgets when composing with Recorder's A/V admission
  // controls. Supplying the limit before or after the track has identical output.
  for (bool limit_first : {false, true}) {
    const std::string path = work + (limit_first ? "/metadata-limit-first.mp4" : "/metadata-limit-last.mp4");
    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
    std::string error;
    auto mux = Muxer::Create(fd, MuxerFormat::kMpeg4, &error); assert(mux);
    if (limit_first) assert(mux->SetFileSizeLimit(4096));
    Format metadata; metadata.get()->mime = "application/octet-stream";
    int track = mux->AddTrack(metadata, &error); assert(track >= 0);
    if (!limit_first) assert(mux->SetFileSizeLimit(4096));
    assert(mux->Start(&error));
    const uint8_t payload[] = {1, 2, 3};
    assert(mux->WriteSample(track, payload, sizeof(payload), 0, 0, &error));
    assert(mux->Stop(&error));
    struct stat info {}; assert(fstat(fd, &info) == 0 && info.st_size > 0 && info.st_size < 4096);
    assert(mux->BytesWritten() == info.st_size && !mux->FileSizeLimitReached());
    close(fd);
  }
  assert(Read(work + "/metadata-limit-first.mp4") == Read(work + "/metadata-limit-last.mp4"));
  std::cout << "Recorder metadata limit-order controls passed\n";
  std::cout << "Recorder completed-container budgets passed\n";
}
