// compat/media/video/yuv_rgba.h: the row converter gives exactly the reference
// formula's pixel for every Y, U, V (all 2^24 triples through the generic and,
// where the CPU has it, the AVX2 variant), for odd widths and pairs at any
// position; and how long a 1280x720 frame takes each way (the per-pixel
// reference loop the decoder used before, the generic and the AVX2 rows).
//
//   yuv_rgba_test.exe   ->  "yuv_rgba_test all passed" (exit 0)
#include "compat/media/video/yuv_rgba.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace yuv = gesher::media::yuv;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

using RowFn = void (*)(const uint8_t*, const uint8_t*, const uint8_t*, uint32_t*, int);

// Every (Y, U, V): rows of 512 pixels, each pair sharing one (U, V).
bool Exhaustive(RowFn row) {
  std::vector<uint8_t> y(512), u(256), v(256);
  std::vector<uint32_t> d(512);
  for (int vv = 0; vv < 256; vv++) {
    for (int uu = 0; uu < 256; uu++) {
      for (int i = 0; i < 256; i++) {
        y[2 * i] = static_cast<uint8_t>(i);
        y[2 * i + 1] = static_cast<uint8_t>(255 - i);
        u[i] = static_cast<uint8_t>(uu);
        v[i] = static_cast<uint8_t>(vv);
      }
      row(y.data(), u.data(), v.data(), d.data(), 512);
      for (int i = 0; i < 512; i++) {
        if (d[i] != yuv::PixelToRgba(y[i], uu, vv)) {
          std::printf("  mismatch at Y %d U %d V %d: %08x, want %08x\n", y[i], uu, vv, d[i],
                      yuv::PixelToRgba(y[i], uu, vv));
          return false;
        }
      }
    }
  }
  return true;
}

// Random rows of every width 1..67 (odd ones end on a pixel with a chroma
// sample of its own).
bool Widths(RowFn row) {
  std::srand(7);
  for (int w = 1; w < 68; w++) {
    std::vector<uint8_t> y(w), u((w + 1) / 2), v((w + 1) / 2);
    for (auto& b : y) b = static_cast<uint8_t>(std::rand());
    for (auto& b : u) b = static_cast<uint8_t>(std::rand());
    for (auto& b : v) b = static_cast<uint8_t>(std::rand());
    std::vector<uint32_t> d(w + 1, 0x12345678u);
    row(y.data(), u.data(), v.data(), d.data(), w);
    for (int x = 0; x < w; x++) {
      if (d[x] != yuv::PixelToRgba(y[x], u[x / 2], v[x / 2])) return false;
    }
    if (d[w] != 0x12345678u) return false;  // nothing written past the row
  }
  return true;
}

double MsPerFrame(RowFn row, bool reference) {
  const int w = 1280, h = 720, cw = w / 2;
  std::vector<uint8_t> y(static_cast<size_t>(w) * h), u(static_cast<size_t>(cw) * h / 2),
      v(static_cast<size_t>(cw) * h / 2);
  for (size_t i = 0; i < y.size(); i++) y[i] = static_cast<uint8_t>(i * 7);
  for (size_t i = 0; i < u.size(); i++) u[i] = static_cast<uint8_t>(i * 3), v[i] = static_cast<uint8_t>(i * 5);
  std::vector<uint32_t> d(static_cast<size_t>(w) * h);
  const int frames = 30;
  const auto start = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; f++) {
    for (int r = 0; r < h; r++) {
      const uint8_t* yr = &y[static_cast<size_t>(r) * w];
      const uint8_t* ur = &u[static_cast<size_t>(r / 2) * cw];
      const uint8_t* vr = &v[static_cast<size_t>(r / 2) * cw];
      uint32_t* dr = &d[static_cast<size_t>(r) * w];
      if (reference) {
        for (int x = 0; x < w; x++) dr[x] = yuv::PixelToRgba(yr[x], ur[x / 2], vr[x / 2]);
      } else {
        row(yr, ur, vr, dr, w);
      }
    }
  }
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  volatile uint32_t sink = d[12345];
  (void)sink;
  return ms / frames;
}

}  // namespace

int main() {
  Check(Exhaustive(&yuv::RowToRgbaGeneric), "generic rows: every Y, U, V gives the reference pixel");
  Check(Widths(&yuv::RowToRgbaGeneric), "generic rows: widths 1..67, nothing written past a row");
#if defined(__x86_64__) && defined(__clang__)
  if (yuv::HasAvx2()) {
    Check(Exhaustive(&yuv::RowToRgbaAvx2), "AVX2 rows: every Y, U, V gives the reference pixel");
    Check(Widths(&yuv::RowToRgbaAvx2), "AVX2 rows: widths 1..67, nothing written past a row");
  } else {
    std::printf("INFO  no AVX2 on this CPU: the AVX2 rows are not run\n");
  }
#endif
  std::printf("INFO  1280x720 frame: per pixel %.2f ms, generic rows %.2f ms, dispatched rows %.2f ms\n",
              MsPerFrame(nullptr, true), MsPerFrame(&yuv::RowToRgbaGeneric, false),
              MsPerFrame(&yuv::RowToRgba, false));
  if (failures == 0) {
    std::printf("yuv_rgba_test all passed\n");
    return 0;
  }
  std::printf("yuv_rgba_test: %d check(s) failed\n", failures);
  return 1;
}
