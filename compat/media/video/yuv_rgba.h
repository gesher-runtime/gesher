// Gesher media: rows of I420 (BT.601, limited range) to RGBA_8888 (R in the
// low byte, alpha 255), as a decoder's frame is drawn into a Surface
// (render.cc) or an Image is read (video_decoder.cc's I420ToRgba).
//
// One formula, 10-bit fixed point:
//   c = (Y - 16) * 1192
//   R = (c + 1634 (V - 128) + 512) >> 10
//   G = (c - 833 (V - 128) - 400 (U - 128) + 512) >> 10
//   B = (c + 2066 (U - 128) + 512) >> 10, each clamped to 0..255.
// RowToRgba gives exactly what PixelToRgba gives for each pixel; it works on
// pixel pairs (one chroma sample) so the compiler vectorizes it, and on
// x86-64 it is compiled a second time for AVX2, used where the CPU has it (a
// 1280x720 frame: about 1 ms in place of 6 to 10 ms a pixel at a time).
#pragma once

#include <stdint.h>

namespace gesher::media::yuv {

inline uint32_t Clamp8(int v) { return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

// The channels' sums (before the shift) packed as RGBA.
inline uint32_t Pack(int r, int g, int b) {
  return 0xff000000u | (Clamp8(b >> 10) << 16) | (Clamp8(g >> 10) << 8) | Clamp8(r >> 10);
}

// The reference: one pixel.
inline uint32_t PixelToRgba(int y, int u, int v) {
  const int c = (y - 16) * 1192;
  const int uu = u - 128, vv = v - 128;
  return Pack(c + 1634 * vv + 512, c - 833 * vv - 400 * uu + 512, c + 2066 * uu + 512);
}

// `w` pixels of a row: y[0..w), chroma u, v[0..(w + 1) / 2), the first chroma
// sample being the first pixel's (the row starts at an even column).
__attribute__((always_inline)) inline void RowToRgbaBody(const uint8_t* __restrict y,
                                                         const uint8_t* __restrict u,
                                                         const uint8_t* __restrict v,
                                                         uint32_t* __restrict d, int w) {
  const int pairs = w / 2;
  for (int i = 0; i < pairs; i++) {
    const int uu = u[i] - 128, vv = v[i] - 128;
    const int ra = 1634 * vv + 512;
    const int ga = -833 * vv - 400 * uu + 512;
    const int ba = 2066 * uu + 512;
    const int c0 = (y[2 * i] - 16) * 1192;
    const int c1 = (y[2 * i + 1] - 16) * 1192;
    d[2 * i] = Pack(c0 + ra, c0 + ga, c0 + ba);
    d[2 * i + 1] = Pack(c1 + ra, c1 + ga, c1 + ba);
  }
  if (w & 1) d[w - 1] = PixelToRgba(y[w - 1], u[pairs], v[pairs]);
}

inline void RowToRgbaGeneric(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint32_t* d,
                             int w) {
  RowToRgbaBody(y, u, v, d, w);
}

#if defined(__x86_64__) && defined(__clang__)
__attribute__((target("avx2"))) inline void RowToRgbaAvx2(const uint8_t* y, const uint8_t* u,
                                                          const uint8_t* v, uint32_t* d, int w) {
  RowToRgbaBody(y, u, v, d, w);
}

inline bool HasAvx2() {
  static const bool avx2 = __builtin_cpu_supports("avx2");
  return avx2;
}
#endif

inline void RowToRgba(const uint8_t* y, const uint8_t* u, const uint8_t* v, uint32_t* d, int w) {
#if defined(__x86_64__) && defined(__clang__)
  if (HasAvx2()) {
    RowToRgbaAvx2(y, u, v, d, w);
    return;
  }
#endif
  RowToRgbaGeneric(y, u, v, d, w);
}

}  // namespace gesher::media::yuv
