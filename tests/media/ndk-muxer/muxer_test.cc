// CI only: real typed NDK muxer calls through JNI and independent ELF callers.
#include <media/NdkMediaMuxer.h>
#include <jni.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Check the actual public NDK declarations on both Android caller ABIs;
// the Windows host's private bridge declarations carry the same assertions.
static_assert(sizeof(AMediaCodecBufferInfo) == 24);
static_assert(offsetof(AMediaCodecBufferInfo, offset) == 0);
static_assert(offsetof(AMediaCodecBufferInfo, size) == 4);
static_assert(offsetof(AMediaCodecBufferInfo, presentationTimeUs) == 8);
static_assert(offsetof(AMediaCodecBufferInfo, flags) == 16);
static_assert(sizeof(OutputFormat) == 4);
static_assert(AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 == 0 && AMEDIAMUXER_OUTPUT_FORMAT_WEBM == 1 &&
              AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP == 2 && AMEDIAMUXER_OUTPUT_FORMAT_HEIF == 3 &&
              AMEDIAMUXER_OUTPUT_FORMAT_OGG == 4);

struct Report {
  int failures = 0;
  std::string text;
  void Check(bool ok, const std::string& name) {
    text += "media4 muxer " + name + ": " + (ok ? "ok\n" : "FAILED\n");
    if (!ok) ++failures;
  }
};

struct Api {
  void* library = dlopen("libmediandk.so", RTLD_NOW);
  decltype(&AMediaMuxer_new) create = nullptr;
  decltype(&AMediaMuxer_delete) destroy = nullptr;
  decltype(&AMediaMuxer_setLocation) location = nullptr;
  decltype(&AMediaMuxer_setOrientationHint) orientation = nullptr;
  decltype(&AMediaMuxer_addTrack) add = nullptr;
  decltype(&AMediaMuxer_start) start = nullptr;
  decltype(&AMediaMuxer_stop) stop = nullptr;
  decltype(&AMediaMuxer_writeSampleData) write = nullptr;
  ssize_t (*count)(AMediaMuxer*) = nullptr;
  AMediaFormat* (*format)(AMediaMuxer*, size_t) = nullptr;
  Api() {
    if (library == nullptr) return;
#define LOAD(field, name) field = reinterpret_cast<decltype(field)>(dlsym(library, name))
    LOAD(create, "AMediaMuxer_new");
    LOAD(destroy, "AMediaMuxer_delete");
    LOAD(location, "AMediaMuxer_setLocation");
    LOAD(orientation, "AMediaMuxer_setOrientationHint");
    LOAD(add, "AMediaMuxer_addTrack");
    LOAD(start, "AMediaMuxer_start");
    LOAD(stop, "AMediaMuxer_stop");
    LOAD(write, "AMediaMuxer_writeSampleData");
    LOAD(count, "AMediaMuxer_getTrackCount");
    LOAD(format, "AMediaMuxer_getTrackFormat");
#undef LOAD
  }
  ~Api() { if (library != nullptr) dlclose(library); }
  bool valid() const {
    return create && destroy && location && orientation && add && start && stop && write && count && format;
  }
};

struct FormatDelete { void operator()(AMediaFormat* p) const { if (p) AMediaFormat_delete(p); } };
using Format = std::unique_ptr<AMediaFormat, FormatDelete>;
struct MuxerDelete {
  const Api* api;
  void operator()(AMediaMuxer* p) const { if (p) api->destroy(p); }
};
using Muxer = std::unique_ptr<AMediaMuxer, MuxerDelete>;

struct Reader {
  FILE* file;
  bool ok;
  explicit Reader(const std::string& path) : file(std::fopen(path.c_str(), "rb")), ok(file != nullptr) {}
  ~Reader() { if (file) std::fclose(file); }
  uint64_t Integer(size_t size) {
    uint8_t bytes[8] = {};
    if (!ok || size > 8 || std::fread(bytes, 1, size, file) != size) { ok = false; return 0; }
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
  }
  std::vector<uint8_t> Bytes(size_t size) {
    if (!ok || size > 16 * 1024 * 1024) { ok = false; return {}; }
    std::vector<uint8_t> bytes(size);
    if (size != 0 && std::fread(bytes.data(), 1, size, file) != size) ok = false;
    return bytes;
  }
  std::vector<uint8_t> Blob() { return Bytes(static_cast<size_t>(Integer(4))); }
};

struct Packet {
  size_t track;
  int64_t time;
  uint32_t flags;
  std::vector<uint8_t> data;
};
struct Dataset {
  std::vector<Format> tracks;
  std::vector<Packet> packets;
};

bool Load(const std::string& path, Dataset* out) {
  Reader input(path);
  const auto magic = input.Bytes(8);
  if (!input.ok || std::string(magic.begin(), magic.end()) != "GSMUX001") return false;
  const uint64_t tracks = input.Integer(4);
  if (!input.ok || tracks == 0 || tracks > 4) return false;
  for (uint64_t i = 0; i < tracks; ++i) {
    Format format(AMediaFormat_new());
    if (!format) return false;
    const uint64_t fields = input.Integer(4);
    if (fields == 0 || fields > 32) return false;
    for (uint64_t j = 0; j < fields; ++j) {
      const auto raw_key = input.Blob();
      const std::string key(raw_key.begin(), raw_key.end());
      const uint64_t kind = input.Integer(4);
      const auto value = input.Blob();
      if (!input.ok || key.empty()) return false;
      if (kind == 1 && value.size() == 4) {
        int32_t integer;
        std::memcpy(&integer, value.data(), 4);
        AMediaFormat_setInt32(format.get(), key.c_str(), integer);
      } else if (kind == 3) {
        const std::string text(value.begin(), value.end());
        AMediaFormat_setString(format.get(), key.c_str(), text.c_str());
      } else if (kind == 4) {
        AMediaFormat_setBuffer(format.get(), key.c_str(), value.data(), value.size());
      } else { return false; }
    }
    out->tracks.push_back(std::move(format));
  }
  const uint64_t samples = input.Integer(4);
  if (!input.ok || samples == 0 || samples > 5000) return false;
  for (uint64_t i = 0; i < samples; ++i) {
    Packet packet;
    packet.track = static_cast<size_t>(input.Integer(4));
    const uint64_t time = input.Integer(8);
    std::memcpy(&packet.time, &time, 8);
    packet.flags = static_cast<uint32_t>(input.Integer(4));
    packet.data = input.Blob();
    if (!input.ok || packet.track >= tracks || packet.time < 0 || packet.data.empty()) return false;
    out->packets.push_back(std::move(packet));
  }
  return input.ok && std::fgetc(input.file) == EOF;
}

void Descriptors(const Api& api, const std::string& root, Report* report) {
  const std::string path = root + "/descriptor-control.bin";
  int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) { report->Check(false, "descriptor-create"); return; }
  const uint8_t seed[32] = {};
  const bool seeded = write(fd, seed, sizeof(seed)) == static_cast<ssize_t>(sizeof(seed));
  close(fd);
  const int anchor = open(path.c_str(), O_RDWR);
  const bool anchor_ready = anchor >= 0 && lseek(anchor, 11, SEEK_SET) == 11;
  Muxer negative(api.create(-1, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4), MuxerDelete{&api});
  Muxer huge(api.create(INT_MAX, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4), MuxerDelete{&api});
  const off_t anchor_after = anchor_ready ? lseek(anchor, 0, SEEK_CUR) : -1;
  bool ok = seeded && anchor_ready && !negative && !huge && anchor_after == 11;
  if (anchor >= 0) close(anchor);
  report->text += "media4 muxer invalid-fd trace: unrelated caller offset before 11 after " +
      std::to_string(anchor_after) + "\n";
  for (int invalid : {-1, 3, 99}) {
    fd = open(path.c_str(), O_RDWR);
    if (fd < 0) { ok = false; continue; }
    const off_t before = lseek(fd, 9, SEEK_SET);
    Muxer unsupported(api.create(fd, static_cast<OutputFormat>(invalid)), MuxerDelete{&api});
    const off_t after = lseek(fd, 0, SEEK_CUR);
    ok = ok && before == 9 && !unsupported && after == 9;
    report->text += "media4 muxer unsupported trace: format " + std::to_string(invalid) +
        " offset before " + std::to_string(before) + " after " + std::to_string(after) + " expected 9\n";
    close(fd);
  }
  for (OutputFormat output : {AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4, AMEDIAMUXER_OUTPUT_FORMAT_WEBM,
                             AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP, AMEDIAMUXER_OUTPUT_FORMAT_OGG}) {
    for (int mode : {O_RDONLY, O_WRONLY, O_RDWR}) {
      fd = open(path.c_str(), mode);
      if (fd < 0) { ok = false; continue; }
      const off_t before = lseek(fd, 7, SEEK_SET);
      Muxer muxer(api.create(fd, output), MuxerDelete{&api});
      const bool expected = mode != O_RDONLY && (output != AMEDIAMUXER_OUTPUT_FORMAT_WEBM || mode == O_RDWR);
      const off_t after = lseek(fd, 0, SEEK_CUR);
      const off_t expected_offset = mode == O_RDONLY ? 7 : 0;
      struct stat status;
      const bool alive = fstat(fd, &status) == 0;
      report->text += "media4 muxer fd trace: format " + std::to_string(output) + " mode " +
          std::to_string(mode) + " expected " + std::to_string(expected) + " actual " +
          std::to_string(muxer != nullptr) + " caller-alive " + std::to_string(alive) +
          " offset before " + std::to_string(before) + " after " + std::to_string(after) +
          " expected-after " + std::to_string(expected_offset) + "\n";
      ok = ok && (muxer != nullptr) == expected && alive && before == 7 && after == expected_offset;
      muxer.reset();
      close(fd);
    }
  }
  fd = open(path.c_str(), O_RDWR);
  close(fd);
  Muxer closed(api.create(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4), MuxerDelete{&api});
  ok = ok && !closed;
  int pipes[2];
  if (pipe(pipes) != 0) { ok = false; } else {
    Muxer muxer(api.create(pipes[1], AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4), MuxerDelete{&api});
    ok = ok && !muxer;
    close(pipes[0]); close(pipes[1]);
  }
  report->Check(ok, "descriptor-access-ownership-seekability");
}

void EmptyLifecycle(const Api& api, const std::string& root, Report* report) {
  bool ok = true;
  for (OutputFormat output : {AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4, AMEDIAMUXER_OUTPUT_FORMAT_WEBM,
                             AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP, AMEDIAMUXER_OUTPUT_FORMAT_OGG}) {
    const int fd = open((root + "/empty-" + std::to_string(output)).c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) { ok = false; continue; }
    Muxer muxer(api.create(fd, output), MuxerDelete{&api});
    close(fd);
    if (!muxer) { ok = false; continue; }
    ok = ok && api.count(muxer.get()) == 0 && api.format(muxer.get(), 0) == nullptr &&
        api.stop(muxer.get()) == AMEDIA_ERROR_INVALID_OPERATION &&
        api.orientation(muxer.get(), -90) == AMEDIA_ERROR_UNKNOWN;
    const media_status_t expected = output == AMEDIAMUXER_OUTPUT_FORMAT_OGG ?
        AMEDIA_ERROR_UNKNOWN : AMEDIA_ERROR_INVALID_OPERATION;
    const media_status_t started = api.start(muxer.get());
    ok = ok && started == expected && api.count(muxer.get()) == 0 &&
        api.orientation(muxer.get(), -90) == AMEDIA_ERROR_INVALID_OPERATION &&
        api.start(muxer.get()) == AMEDIA_ERROR_INVALID_OPERATION &&
        api.stop(muxer.get()) == AMEDIA_OK && api.count(muxer.get()) == -1 &&
        api.stop(muxer.get()) == AMEDIA_ERROR_INVALID_OPERATION;
    report->text += "media4 muxer empty trace: format " + std::to_string(output) +
        " start " + std::to_string(started) + " expected " + std::to_string(expected) + "\n";
  }
  report->Check(ok, "empty-lifecycle-and-error-order");
}

void Write(const Api& api, const Dataset& data, const std::string& path, OutputFormat output,
           bool blocked, bool delete_active, Report* report) {
  const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
  if (fd < 0) { report->Check(false, "create-output " + path); return; }
  Muxer muxer(api.create(fd, output), MuxerDelete{&api});
  close(fd);  // Every actual write must use the muxer's independent descriptor.
  if (!muxer) { report->Check(false, "create-muxer " + path); return; }
  bool ok = api.orientation(muxer.get(), 90) == AMEDIA_OK;
  const bool mp4 = output == AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 || output == AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP;
  ok = ok && api.location(muxer.get(), 45.125f, -75.25f) ==
      (mp4 ? AMEDIA_OK : AMEDIA_ERROR_INVALID_OPERATION);
  if (mp4) ok = ok && api.location(muxer.get(), 91, 0) == AMEDIA_ERROR_UNKNOWN;
  Format retained;
  for (size_t i = 0; i < data.tracks.size(); ++i) {
    Format original(AMediaFormat_new());
    if (!original) { report->Check(false, "track-format-allocation"); return; }
    AMediaFormat_copy(original.get(), data.tracks[i].get());
    const ssize_t index = api.add(muxer.get(), original.get());
    Format query(api.format(muxer.get(), i));
    const char* before = nullptr;
    const bool mime = query && AMediaFormat_getString(query.get(), "mime", &before);
    ok = ok && index == static_cast<ssize_t>(i) && api.count(muxer.get()) == index + 1 &&
        query.get() != original.get() && mime;
    if (query) AMediaFormat_setString(query.get(), "gesher-contract-alias", "retained");
    const char* alias = nullptr;
    ok = ok && AMediaFormat_getString(original.get(), "gesher-contract-alias", &alias) &&
        std::strcmp(alias, "retained") == 0;
    original.reset();
    const char* after = nullptr;
    ok = ok && query && AMediaFormat_getString(query.get(), "mime", &after);
    if (i == 0) retained = std::move(query);
  }
  ok = ok && api.format(muxer.get(), SIZE_MAX) == nullptr && api.start(muxer.get()) == AMEDIA_OK;
  ok = ok && api.add(muxer.get(), data.tracks[0].get()) == AMEDIA_ERROR_INVALID_OPERATION &&
      api.orientation(muxer.get(), 37) == AMEDIA_ERROR_INVALID_OPERATION &&
      api.location(muxer.get(), 91, 0) == AMEDIA_ERROR_INVALID_OPERATION;
  const auto& first = data.packets[0];
  AMediaCodecBufferInfo invalid_track = {0, static_cast<int32_t>(first.data.size()), first.time, first.flags};
  ok = ok && api.write(muxer.get(), SIZE_MAX, first.data.data(), &invalid_track) == AMEDIA_ERROR_UNKNOWN;
  std::vector<size_t> order(data.packets.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (blocked && data.packets[a].track != data.packets[b].track) return data.packets[a].track < data.packets[b].track;
    return data.packets[a].time < data.packets[b].time;
  });
  for (size_t index : order) {
    const Packet& packet = data.packets[index];
    std::vector<uint8_t> padded(packet.data.size() + 32, 0xa5);
    std::memcpy(padded.data() + 16, packet.data.data(), packet.data.size());
    AMediaCodecBufferInfo info = {16, static_cast<int32_t>(packet.data.size()), packet.time, packet.flags};
    const media_status_t status = api.write(muxer.get(), packet.track, padded.data(), &info);
    if (status != AMEDIA_OK) {
      report->text += "media4 muxer write failure: track " + std::to_string(packet.track) +
          " time " + std::to_string(packet.time) + " status " + std::to_string(status) + "\n";
      ok = false;
      break;
    }
    ok = ok && std::all_of(padded.begin(), padded.begin() + 16, [](uint8_t c) { return c == 0xa5; }) &&
        std::all_of(padded.end() - 16, padded.end(), [](uint8_t c) { return c == 0xa5; });
    if (output == AMEDIAMUXER_OUTPUT_FORMAT_WEBM) {
      AMediaCodecBufferInfo invalid = info;
      invalid.presentationTimeUs = packet.time > 0 ? packet.time - 1 : -1;
      ok = ok && api.write(muxer.get(), packet.track, padded.data(), &invalid) == AMEDIA_ERROR_UNKNOWN;
      invalid.presentationTimeUs = INT64_MAX;
      ok = ok && api.write(muxer.get(), packet.track, padded.data(), &invalid) == AMEDIA_ERROR_UNKNOWN;
    }
  }
  if (!delete_active) {
    ok = ok && api.stop(muxer.get()) == AMEDIA_OK && api.count(muxer.get()) == -1 &&
        api.format(muxer.get(), 0) == nullptr && api.start(muxer.get()) == AMEDIA_ERROR_INVALID_OPERATION &&
        api.stop(muxer.get()) == AMEDIA_ERROR_INVALID_OPERATION;
  }
  muxer.reset();
  const char* mime = nullptr;
  ok = ok && retained && AMediaFormat_getString(retained.get(), "mime", &mime) && mime != nullptr;
  report->Check(ok, std::string("typed-track-owner-offset-write-stop ") +
      (delete_active ? "delete-active " : (blocked ? "blocked " : "interleaved ")) + path);
  report->text += "media4 muxer output: " + path + "; format " + std::to_string(output) +
      "; tracks " + std::to_string(data.tracks.size()) + "; samples " + std::to_string(data.packets.size()) + "\n";
}

void OggBoundaries(const Api& api, const Dataset& data, const std::string& root, Report* report) {
  std::array<bool, 3> results{};
  std::array<bool, 3> payload_eos{};
  std::array<std::thread, 3> workers;
  std::atomic<unsigned> ready{0};
  std::atomic<bool> go{false};
  for (size_t count : {0u, 1u, 2u}) {
    workers[count] = std::thread([&, count] {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      const std::string path = root + "/opus-boundary-" + std::to_string(count) + ".ogg";
      const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
      Muxer muxer(fd >= 0 ? api.create(fd, AMEDIAMUXER_OUTPUT_FORMAT_OGG) : nullptr, MuxerDelete{&api});
      if (fd >= 0) close(fd);
      bool ok = muxer && data.tracks.size() == 1 && data.packets.size() >= count;
      ok = ok && api.add(muxer.get(), data.tracks[0].get()) == 0 && api.start(muxer.get()) == AMEDIA_OK;
      for (size_t index = 0; ok && index < count; ++index) {
        const Packet& packet = data.packets[index];
        AMediaCodecBufferInfo info = {0, static_cast<int32_t>(packet.data.size()), packet.time, packet.flags};
        if (count == 2 && index + 1 == count) info.flags |= 4;
        ok = api.write(muxer.get(), 0, packet.data.data(), &info) == AMEDIA_OK;
        if (info.flags & 4) payload_eos[count] = ok;
      }
      // The explicit zero-byte EOS buffer must not become an encoded packet.
      uint8_t dummy = 0;
      AMediaCodecBufferInfo eos = {0, 0, 0, 4};
      ok = ok && api.write(muxer.get(), 0, &dummy, &eos) == AMEDIA_OK && api.stop(muxer.get()) == AMEDIA_OK;
      muxer.reset();
      results[count] = ok;
    });
  }
  while (ready.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto& worker : workers) worker.join();
  for (size_t count : {0u, 1u, 2u}) {
    report->text += "media4 muxer Ogg boundary: packets " + std::to_string(count) +
        " result=" + (results[count] ? "ok" : "FAILED") +
        " payload-eos=" + (payload_eos[count] ? "1\n" : "0\n");
    if (!results[count]) ++report->failures;
  }
}

std::string Run() {
  Report report;
#if defined(__aarch64__)
  report.text = "media4 muxer ABI: arm64-v8a\n";
#else
  report.text = "media4 muxer ABI: x86_64\n";
#endif
#ifdef MEDIA_MUXER_EXECUTABLE
  report.text += "media4 muxer caller: executable\n";
#else
  report.text += "media4 muxer caller: JNI\n";
#endif
  const char* fixtures = std::getenv("GESHER_MUXER_FIXTURES");
  const char* output = std::getenv("GESHER_MUXER_OUT");
  Api api;
  report.Check(api.valid() && api.destroy(nullptr) == AMEDIA_OK, "ten-typed-dynamic-symbols-and-null-delete");
  if (api.valid() && fixtures != nullptr && output != nullptr) {
    Descriptors(api, output, &report);
    EmptyLifecycle(api, output, &report);
    struct Case { const char* input; const char* suffix; OutputFormat format; };
    const Case cases[] = {{"h264-aac", "mp4", AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4},
      {"h264-aac", "3gp", AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP},
      {"vp9-opus", "webm", AMEDIAMUXER_OUTPUT_FORMAT_WEBM},
      {"vp8-vorbis", "webm", AMEDIAMUXER_OUTPUT_FORMAT_WEBM},
      {"opus", "ogg", AMEDIAMUXER_OUTPUT_FORMAT_OGG}};
    for (const auto& test : cases) {
      Dataset data;
      const bool loaded = Load(std::string(fixtures) + "/" + test.input + ".bin", &data);
      report.Check(loaded, std::string("independent-encoded-fixture ") + test.input);
      if (!loaded) continue;
      // Race the first Ogg writers, before any ordinary Ogg output warms CRC.
      if (test.format == AMEDIAMUXER_OUTPUT_FORMAT_OGG) OggBoundaries(api, data, output, &report);
      for (bool blocked : {false, true}) {
        if (blocked && test.format == AMEDIAMUXER_OUTPUT_FORMAT_OGG) continue;
        const std::string path = std::string(output) + "/" + test.input +
            (blocked ? "-blocked." : "-interleaved.") + test.suffix;
        Write(api, data, path, test.format, blocked, false, &report);
      }
      if (test.format == AMEDIAMUXER_OUTPUT_FORMAT_WEBM) {
        // Retain all video but end audio at one second. The writer must flush
        // the remaining video after the shorter track has ended.
        size_t audio = data.tracks.size();
        for (size_t i = 0; i < data.tracks.size(); ++i) {
          const char* mime = nullptr;
          if (AMediaFormat_getString(data.tracks[i].get(), "mime", &mime) &&
              mime != nullptr && std::strncmp(mime, "audio/", 6) == 0) audio = i;
        }
        report.Check(audio < data.tracks.size(), "webm-short-audio-track");
        if (audio < data.tracks.size()) {
          data.packets.erase(std::remove_if(data.packets.begin(), data.packets.end(),
              [audio](const Packet& p) { return p.track == audio && p.time >= 1000000; }), data.packets.end());
          for (bool blocked : {false, true}) {
            Write(api, data, std::string(output) + "/" + test.input +
                (blocked ? "-short-blocked.webm" : "-short-interleaved.webm"),
                test.format, blocked, false, &report);
          }
        }
      }
      if (test.format == AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4) {
        Write(api, data, std::string(output) + "/h264-aac-delete-active.mp4", test.format, false, true, &report);
      }
    }
  } else { report.Check(false, "fixture-and-output-environment"); }
  report.text += report.failures == 0 ? "media4 muxer: all passed\n" :
      "media4 muxer: FAILED " + std::to_string(report.failures) + "\n";
  return report.text;
}

}  // namespace

#ifdef MEDIA_MUXER_EXECUTABLE
int main(int argc, char** argv) {
  if (argc != 1 || std::strcmp(argv[0], "ndk-muxer-test") != 0) return 42;
  const std::string report = Run();
  std::fputs(report.c_str(), stdout);
  std::fflush(stdout);
  return report.find("media4 muxer: all passed\n") != std::string::npos ? 0 : 1;
}
#else
extern "C" JNIEXPORT jstring JNICALL Java_gesher_MediaMuxerTest_run(JNIEnv* env, jclass) {
  const std::string report = Run();
  return env->NewStringUTF(report.c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_gesher_mediamuxer_MediaMuxerActivity_run(JNIEnv* env, jclass type) {
  return Java_gesher_MediaMuxerTest_run(env, type);
}
#endif
