// Skia's CPU raster backend on Windows: shapes, antialiasing, text through
// FreeType, and the PNG, JPEG and WebP codecs. Run in CI on Windows
// (tests/windows/tracks/30-graphics.ps1); prints "all passed" when every
// check holds, and writes what it drew to skia_probe.png.
//
//   skia_probe [font.ttf] [out.png]
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "include/codec/SkCodec.h"
#include "include/codec/SkJpegDecoder.h"
#include "include/codec/SkPngDecoder.h"
#include "include/codec/SkWebpDecoder.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"
#include "include/ports/SkFontMgr_empty.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) failures++;
}

static SkColor at(const SkPixmap& pm, int x, int y) { return pm.getColor(x, y); }

static bool near(SkColor a, SkColor b, int tolerance) {
  return std::abs(int(SkColorGetR(a)) - int(SkColorGetR(b))) <= tolerance &&
         std::abs(int(SkColorGetG(a)) - int(SkColorGetG(b))) <= tolerance &&
         std::abs(int(SkColorGetB(a)) - int(SkColorGetB(b))) <= tolerance;
}

// Encodes and decodes the surface's pixels; checks the red square survived.
static void roundTrip(const char* name, sk_sp<SkData> data,
                      std::unique_ptr<SkCodec> (*decode)(sk_sp<SkData>, SkCodec::Result*,
                                                         SkCodecs::DecodeContext),
                      int tolerance) {
  char what[96];
  std::snprintf(what, sizeof what, "%s: encode and decode", name);
  if (!data) {
    check(false, what);
    return;
  }
  SkCodec::Result result;
  std::unique_ptr<SkCodec> codec = decode(data, &result, nullptr);
  if (!codec) {
    check(false, what);
    return;
  }
  auto [image, r] = codec->getImage();
  SkPixmap pm;
  bool ok = image && r == SkCodec::kSuccess;
  sk_sp<SkImage> raster = ok ? image->makeRasterImage(nullptr) : nullptr;
  ok = raster && raster->peekPixels(&pm) && near(at(pm, 30, 30), SK_ColorRED, tolerance) &&
       near(at(pm, 120, 110), SK_ColorWHITE, tolerance);
  check(ok, what);
}

int main(int argc, char** argv) {
  const char* fontPath = argc > 1 ? argv[1] : "C:/Windows/Fonts/arial.ttf";
  const char* outPath = argc > 2 ? argv[2] : "skia_probe.png";

  sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(256, 128));
  check(surface != nullptr, "raster surface 256x128");
  if (!surface) return 1;
  SkCanvas* canvas = surface->getCanvas();
  canvas->clear(SK_ColorWHITE);

  SkPaint red;
  red.setColor(SK_ColorRED);
  canvas->drawRect(SkRect::MakeLTRB(10, 10, 60, 60), red);

  SkPaint blue;
  blue.setColor(SK_ColorBLUE);
  blue.setAntiAlias(true);
  canvas->drawCircle(150, 40, 30, blue);

  SkPaint green;
  green.setColor(SK_ColorGREEN);
  green.setAntiAlias(true);
  SkPath triangle;
  triangle.moveTo(200, 10).lineTo(250, 60).lineTo(200, 60).close();
  canvas->drawPath(triangle, green);

  sk_sp<SkFontMgr> fontMgr = SkFontMgr_New_Custom_Empty();
  sk_sp<SkTypeface> typeface = fontMgr ? fontMgr->makeFromFile(fontPath) : nullptr;
  check(typeface != nullptr, "FreeType loads a TrueType font");
  if (typeface) {
    SkFont font(typeface, 28);
    SkPaint black;
    black.setColor(SK_ColorBLACK);
    black.setAntiAlias(true);
    canvas->drawString("Gesher", 10, 110, font, black);
  }

  SkPixmap pm;
  check(surface->peekPixels(&pm), "surface pixels readable");
  check(at(pm, 30, 30) == SK_ColorRED, "filled rect: red inside");
  check(at(pm, 5, 5) == SK_ColorWHITE, "filled rect: white outside");
  check(at(pm, 150, 40) == SK_ColorBLUE, "antialiased circle: blue at the centre");
  // On the circle's edge, coverage is partial: neither white nor blue.
  bool blended = false;
  for (int x = 115; x < 125 && !blended; x++) {
    SkColor c = at(pm, x, 40);
    blended = c != SK_ColorWHITE && c != SK_ColorBLUE;
  }
  check(blended, "antialiased circle: partial coverage on the edge");
  check(at(pm, 210, 55) == SK_ColorGREEN && at(pm, 245, 20) == SK_ColorWHITE,
        "path: triangle filled on its side of the diagonal");
  int dark = 0;
  for (int y = 80; y < 120; y++)
    for (int x = 10; x < 120; x++)
      if (SkColorGetR(at(pm, x, y)) < 128) dark++;
  std::printf("INFO  text pixels: %d\n", dark);
  check(dark > 200 && dark < 3000, "text: glyphs drawn");

  sk_sp<SkImage> snapshot = surface->makeImageSnapshot();
  roundTrip("PNG", SkPngEncoder::Encode(nullptr, snapshot.get(), {}), SkPngDecoder::Decode, 0);
  SkJpegEncoder::Options jpeg;
  jpeg.fQuality = 95;
  roundTrip("JPEG", SkJpegEncoder::Encode(nullptr, snapshot.get(), jpeg), SkJpegDecoder::Decode,
            24);
  SkWebpEncoder::Options webp;
  webp.fCompression = SkWebpEncoder::Compression::kLossless;
  roundTrip("WebP", SkWebpEncoder::Encode(nullptr, snapshot.get(), webp), SkWebpDecoder::Decode,
            0);

  SkFILEWStream out(outPath);
  check(out.isValid() && SkPngEncoder::Encode(&out, pm, {}), "wrote the picture");

  std::printf(failures ? "%d failed\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
