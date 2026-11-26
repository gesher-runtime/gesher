// Actual I420 conversion and QueueSurface/RenderFrame contracts. The Windows
// target uses real hostgraphics objects; the host runner declares its boundary.
#ifdef GESHER_I420_HOST
#include "i420_crop_boundary.h"
#else
#include "compat/media/surface/queue_surface.h"
#include "compat/media/video/video_decoder.h"
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifdef GESHER_I420_OBSERVE
extern "C" void i420_crop_reset_counts();
extern "C" void i420_crop_read_counts(uint64_t* rows, uint64_t* first, uint64_t* scalar);
#endif

namespace {
using gesher::media::I420ToRgba;
using gesher::media::QueueSurface;
using gesher::media::RenderFrame;
using gesher::media::SurfaceFrame;
using gesher::media::VideoFrame;
int checks = 0, failures = 0;
constexpr uint32_t kGuard = 0x13579bdfu;

void Check(bool okay, const char* reason) {
  ++checks;
  if (!okay) { ++failures; std::fprintf(stderr, "FAIL I420 crop: %s\n", reason); }
}

VideoFrame Frame(int w, int h, int left, int top, int y_pad = 0, int uv_pad = 0) {
  VideoFrame f;
  f.width = w + left + 3; f.height = h + top + 2;
  f.stride_y = f.width + y_pad; f.stride_uv = (f.width + 1) / 2 + uv_pad;
  f.crop_left = left; f.crop_top = top;
  f.crop_right = left + w - 1; f.crop_bottom = top + h - 1;
  f.time_us = 1234567;
  f.data.resize(static_cast<size_t>(f.stride_y) * f.height +
                2 * static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2));
  uint32_t random = 0x56a420u;
  for (auto& b : f.data) { random = random * 1664525u + 1013904223u; b = random >> 24; }
  return f;
}

// Independent arithmetic: a negative numerator clamps to zero, so unsigned
// division on the remaining nonnegative range avoids copying a signed shift.
uint32_t Channel(int64_t n) {
  if (n <= 0) return 0;
  return static_cast<uint32_t>(std::min<int64_t>(255, n / 1024));
}
uint32_t Pixel(int y, int u, int v) {
  const int64_t base = (y - 16) * 1192LL + 512;
  return 0xff000000u | Channel(base + 1634LL * (v - 128)) |
         (Channel(base - 833LL * (v - 128) - 400LL * (u - 128)) << 8) |
         (Channel(base + 2066LL * (u - 128)) << 16);
}
void Scalar(const VideoFrame& f, uint32_t* out, int stride, int w, int h) {
  const int vw = std::max(f.visible_width(), 1), vh = std::max(f.visible_height(), 1);
  const size_t y_bytes = static_cast<size_t>(f.stride_y) * f.height;
  const size_t uv_bytes = static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
  for (int y = 0; y < h; ++y) {
    const int sy = f.crop_top + static_cast<int>(static_cast<int64_t>(y) * vh / h);
    for (int x = 0; x < w; ++x) {
      const int sx = f.crop_left + static_cast<int>(static_cast<int64_t>(x) * vw / w);
      const size_t uv = static_cast<size_t>(sy / 2) * f.stride_uv + sx / 2;
      out[static_cast<size_t>(y) * stride + x] = Pixel(
          f.data[static_cast<size_t>(sy) * f.stride_y + sx],
          f.data[y_bytes + uv], f.data[y_bytes + uv_bytes + uv]);
    }
  }
}

void ResetWork() {
#ifdef GESHER_I420_OBSERVE
  i420_crop_reset_counts();
#endif
}

void Compare(VideoFrame f, int w, int h, int stride, bool require_work) {
  const auto original = f.data;
  const size_t extent = h > 0 && w > 0 ? static_cast<size_t>(h - 1) * stride + w : 0;
  std::vector<uint32_t> actual(extent + 8, kGuard), expected = actual;
  Scalar(f, expected.data() + 4, stride, w, h);
  ResetWork();
  I420ToRgba(f, actual.data() + 4, stride, w, h);
  Check(actual == expected, "complete cropped pixels and destination guards");
  Check(f.data == original, "input pixels and source padding unchanged");
#ifdef GESHER_I420_OBSERVE
  if (require_work && w == f.visible_width() && h == f.visible_height() && stride >= w) {
    const int first = f.crop_left & 1;
    uint64_t rows, leading, scalar;
    i420_crop_read_counts(&rows, &leading, &scalar);
    Check(rows == static_cast<uint64_t>(first < w ? h : 0) &&
          leading == static_cast<uint64_t>(first ? h : 0) && scalar == 0,
          "same-size odd crop uses aligned row conversion");
  }
#else
  (void)require_work;
#endif
}

void Geometry(bool require_work) {
  for (int w = 1; w <= 131; ++w) for (int h : {1, 2, 3, 7}) {
    for (int left : {0, 1, 2, 3}) for (int top : {0, 1, 3}) {
      for (int pad : {0, 3, 13}) Compare(Frame(w, h, left, top, pad, pad / 2), w, h, w + pad,
                                      require_work);
    }
  }
  for (int left : {0, 1, 3}) {
    auto f = Frame(17, 11, left, 1, 7, 5);
    for (auto size : {std::pair{1, 1}, {9, 5}, {17, 4}, {7, 11}, {31, 23}})
      Compare(f, size.first, size.second, size.first + 7, require_work);
    Compare(f, 0, 11, 1, false); Compare(f, 17, 0, 17, false);
    // Overlapping output rows remain a valid bounded scalar case for odd crops.
    if (left & 1) Compare(f, 17, 11, 3, false);
  }
}

void Aliasing() {
  for (int left : {1, 3}) for (int w : {1, 2, 3, 7, 16, 33}) for (int h : {1, 3, 7}) {
    for (size_t offset : {size_t{0}, size_t{4}, size_t{32}}) {
      auto actual = Frame(w, h, left, 1, 3, 2);
      actual.data.resize(std::max(actual.data.size(), offset + static_cast<size_t>(w + 2) * h * 4 + 32),
                         0xa5);
      auto expected = actual;
      Scalar(expected, reinterpret_cast<uint32_t*>(expected.data.data() + offset), w + 2, w, h);
      ResetWork();
      I420ToRgba(actual, reinterpret_cast<uint32_t*>(actual.data.data() + offset), w + 2, w, h);
      Check(actual.data == expected.data, "aliased odd-crop source retains scalar pass order");
#ifdef GESHER_I420_OBSERVE
      uint64_t rows, leading, scalar;
      i420_crop_read_counts(&rows, &leading, &scalar);
      Check(rows == 0 && scalar == static_cast<uint64_t>(w) * h,
            "aliased odd-crop source does not enter restrict row helper");
#endif
    }
  }
}

void Exhaustive() {
  auto f = Frame(257, 1, 1, 1, 3, 4);
  const size_t y_bytes = static_cast<size_t>(f.stride_y) * f.height;
  const size_t uv_bytes = static_cast<size_t>(f.stride_uv) * ((f.height + 1) / 2);
  for (int x = 0; x < 257; ++x) f.data[f.stride_y + 1 + x] = static_cast<uint8_t>(x);
  std::vector<uint32_t> actual(259, kGuard);
  bool okay = true;
  for (int v = 0; v < 256; ++v) for (int u = 0; u < 256; ++u) {
    std::fill_n(f.data.begin() + y_bytes, uv_bytes, static_cast<uint8_t>(u));
    std::fill(f.data.begin() + y_bytes + uv_bytes, f.data.end(), static_cast<uint8_t>(v));
    I420ToRgba(f, actual.data() + 1, 257, 257, 1);
    for (int y = 0; y < 257; ++y) okay &= actual[y + 1] == Pixel(y & 255, u, v);
    okay &= actual.front() == kGuard && actual.back() == kGuard;
  }
  Check(okay, "all 16777216 YUV triples through odd-crop converter and row tail");
}

void QueueOwnership() {
  std::vector<std::shared_ptr<const SurfaceFrame>> held;
  std::vector<std::vector<uint32_t>> expected;
  int callbacks = 0;
  {
    auto queue = android::sp<QueueSurface>::make(2, 2);
    queue->SetListener([&] { ++callbacks; Check(queue->Latest() != nullptr, "callback observes published owner"); });
    for (auto size : {std::pair{17, 9}, {1, 1}, {32, 7}, {7, 3}, {17, 9}}) {
      auto f = Frame(size.first, size.second, callbacks % 4, callbacks % 3, 5, 3);
      std::vector<uint32_t> pixels(static_cast<size_t>(size.first) * size.second);
      Scalar(f, pixels.data(), size.first, size.first, size.second);
      const int64_t timestamp = 1000000LL + callbacks * 33333333LL;
      const int mode = callbacks % 2 + 1;
      Check(RenderFrame(queue.get(), f, timestamp, mode) == 0, "actual RenderFrame publishes cropped frame");
      auto frame = queue->Latest();
      Check(frame && frame->pixels == pixels && frame->width == size.first && frame->height == size.second,
            "actual queue contains every converted pixel");
      Check(frame && frame->timestamp_ns == timestamp && frame->number == static_cast<uint64_t>(callbacks) &&
            frame->video_scaling_mode == mode, "actual queue retains time, order and scaling mode");
      held.push_back(frame); expected.push_back(pixels);
      std::fill(f.data.begin(), f.data.end(), 0x43);
      for (size_t n = 0; n < held.size(); ++n)
        Check(held[n]->pixels == expected[n], "retained output survives input mutation and next geometry");
    }
    auto valid = Frame(4, 3, 1, 1);
    Check(RenderFrame(nullptr, valid) == -ENODEV, "null surface retains NO_INIT");
    auto empty = valid; empty.crop_right = empty.crop_left - 1;
    Check(RenderFrame(queue.get(), empty) == -EINVAL, "empty crop retains BAD_VALUE");
    queue->Abandon();
    Check(RenderFrame(queue.get(), valid) == -ENODEV, "closed surface retains NO_INIT");
    Check(callbacks == 5 && !queue->Latest(), "failed deliveries publish no frame or callback");
  }
  for (size_t n = 0; n < held.size(); ++n)
    Check(held[n]->pixels == expected[n], "retained output survives surface destruction");
}

void Concurrent() {
  const auto f = Frame(129, 37, 3, 1, 7, 5);
  std::vector<uint32_t> expected(129 * 37);
  Scalar(f, expected.data(), 129, 129, 37);
  std::atomic<bool> okay{true};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
    std::vector<uint32_t> output(expected.size());
    for (int n = 0; n < 32; ++n) {
      I420ToRgba(f, output.data(), 129, 129, 37);
      if (output != expected) okay = false;
    }
  });
  for (auto& t : threads) t.join();
  Check(okay.load(), "shared immutable input with independent concurrent destinations");
}
}  // namespace

int main(int argc, char** argv) {
  const bool reference = argc > 1 && std::strcmp(argv[1], "--reference") == 0;
  if (argc > 1 && std::strcmp(argv[1], "--negative") == 0) {
    if (argc > 2 && std::strcmp(argv[2], "alias") == 0) {
      Aliasing();
    } else {
      const bool height = argc > 2 && std::strcmp(argv[2], "height") == 0;
      Compare(Frame(17, 7, 1, 1, 5, 3), 17, height ? 3 : 7, 23, !height);
    }
  } else {
    Geometry(!reference); Aliasing(); Exhaustive(); QueueOwnership(); Concurrent();
  }
  std::printf("I420 crop contracts: %d checks, %d failures\n", checks, failures);
  if (failures == 0) std::puts("I420 cropped rows, exact pixels, guards and retained surfaces passed");
  return failures != 0;
}
