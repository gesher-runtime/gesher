// Ordinary complete-component timings. The converter, QueueSurface/RenderFrame
// and this common caller are separate translation units; no work counters,
// replacement allocator, sanitizer, LTO or provider-success stubs are used.
#define main I420CropContractMain
#include "i420_crop_test.cc"
#undef main

#include <ctime>

namespace {
struct Case {
  const char* name;
  int w, h, left, top, out_w, out_h, repeats, frames;
  bool direct_scaled;
};
constexpr Case kCases[] = {
    {"owner-empty", 0, 0, 0, 0, 0, 0, 2000, 0, false},
    {"single-odd", 1, 1, 1, 1, 1, 1, 500, 4, false},
    {"odd-17x9", 17, 9, 1, 1, 17, 9, 300, 4, false},
    {"odd-65x37", 65, 37, 3, 3, 65, 37, 100, 4, false},
    {"odd-720p", 1280, 720, 1, 1, 1280, 720, 2, 4, false},
    {"odd-1080p", 1920, 1080, 1, 1, 1920, 1080, 1, 4, false},
    {"odd-4k", 3840, 2160, 3, 3, 3840, 2160, 1, 2, false},
    {"even-720p", 1280, 720, 2, 1, 1280, 720, 2, 4, false},
    {"zero-720p", 1280, 720, 0, 0, 1280, 720, 2, 4, false},
    {"scaled-up", 640, 360, 1, 1, 1280, 720, 2, 4, true},
    {"scaled-down", 1280, 720, 1, 1, 640, 360, 4, 4, true},
};

[[noreturn]] void Fail(const char* message) {
  std::fprintf(stderr, "I420 benchmark failure: %s\n", message);
  std::exit(1);
}
uint64_t Clock(clockid_t kind) {
  timespec t{};
  if (clock_gettime(kind, &t) != 0) Fail("clock_gettime");
  return static_cast<uint64_t>(t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
bool Equal(const std::shared_ptr<const SurfaceFrame>& frame,
           const std::vector<uint32_t>& expected, const Case& c) {
  return frame && frame->width == c.out_w && frame->height == c.out_h &&
         frame->pixels.size() == expected.size() &&
         std::memcmp(frame->pixels.data(), expected.data(), expected.size() * 4) == 0;
}

// Every timed lifecycle includes construction, the real producer/consumer
// handoff, full-byte output checks, previous-owner retention, abandon and final
// destruction. Inputs and independent expected pixels are prepared before it.
uint64_t Lifecycle(const Case& c, const VideoFrame& input,
                   const std::vector<uint32_t>& expected, int frames) {
  uint64_t checksum = 0;
  std::shared_ptr<const SurfaceFrame> held;
  {
    auto queue = android::sp<QueueSurface>::make(2, 2);
    int callbacks = 0;
    queue->SetListener([&] {
      ++callbacks;
      if (!queue->Latest()) Fail("callback before publication");
    });
    for (int n = 0; n < frames; ++n) {
      const int64_t timestamp = 7000000LL + n * 33333333LL;
      const int mode = n % 2 + 1;
      if (c.direct_scaled) {
        // RenderFrame selects the visible dimensions. This separately labeled
        // fallback control directly converts into the real locked queue buffer.
        if (ANativeWindow_setBuffersGeometry(queue.get(), c.out_w, c.out_h, 1) != 0)
          Fail("scaled geometry");
        ANativeWindow_Buffer buffer{};
        if (ANativeWindow_lock(queue.get(), &buffer, nullptr) != 0) Fail("scaled lock");
        I420ToRgba(input, static_cast<uint32_t*>(buffer.bits), buffer.stride,
                   buffer.width, buffer.height);
        if (queue->UnlockAndPostWithTimestamp(timestamp, mode) != 0) Fail("scaled post");
      } else if (RenderFrame(queue.get(), input, timestamp, mode) != 0) {
        Fail("RenderFrame");
      }
      if (held && !Equal(held, expected, c)) Fail("prior owner mutated");
      auto current = queue->Latest();
      if (!Equal(current, expected, c) || current->timestamp_ns != timestamp ||
          current->number != static_cast<uint64_t>(n + 1) ||
          current->video_scaling_mode != mode || callbacks != n + 1) Fail("full output");
      checksum += current->pixels.front() + static_cast<uint64_t>(current->pixels.back()) +
                  current->number + timestamp + mode;
      held = std::move(current);
    }
    queue->Abandon();
    if (queue->Latest() || callbacks != frames) Fail("abandon publication");
  }
  if (held && !Equal(held, expected, c)) Fail("owner after surface destruction");
  return checksum + static_cast<uint64_t>(frames);
}

void Measure(const char* phase, const Case& c, const VideoFrame& input,
             const std::vector<uint32_t>& expected, int repeats, int frames, int sample) {
  const uint64_t wall_start = Clock(CLOCK_MONOTONIC);
  const uint64_t cpu_start = Clock(CLOCK_PROCESS_CPUTIME_ID);
  uint64_t checksum = 0;
  for (int i = 0; i < repeats; ++i) checksum += Lifecycle(c, input, expected, frames);
  const uint64_t cpu_ns = Clock(CLOCK_PROCESS_CPUTIME_ID) - cpu_start;
  const uint64_t wall_ns = Clock(CLOCK_MONOTONIC) - wall_start;
  std::printf("{\"case\":\"%s\",\"phase\":\"%s\",\"sample\":%d,"
              "\"lifecycles\":%d,\"frames_per_lifecycle\":%d,\"output_pixels\":%zu,"
              "\"cpu_ns\":%llu,\"wall_ns\":%llu,\"checksum\":%llu}\n",
              c.name, phase, sample, repeats, frames, expected.size(),
              static_cast<unsigned long long>(cpu_ns), static_cast<unsigned long long>(wall_ns),
              static_cast<unsigned long long>(checksum));
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--contracts") == 0)
    return I420CropContractMain(1, argv);
  if (argc != 2) Fail("expected exact case name or --contracts");
  const Case* selected = nullptr;
  for (const auto& c : kCases) if (std::strcmp(argv[1], c.name) == 0) selected = &c;
  if (!selected) Fail("unknown case");
  const Case& c = *selected;
  const auto input = c.w ? Frame(c.w, c.h, c.left, c.top, 13, 7) : VideoFrame{};
  std::vector<uint32_t> expected(static_cast<size_t>(c.out_w) * c.out_h);
  if (c.w) Scalar(input, expected.data(), c.out_w, c.out_w, c.out_h);
  Measure("first", c, input, expected, 1, c.frames ? 1 : 0, 0);
  for (int sample = 0; sample < 3; ++sample)
    Measure("steady", c, input, expected, c.repeats, c.frames, sample);
  return 0;
}
