// Gesher: Skia's Fontations backend (Rust) is not built (docs/graphics.md), but
// hwui's host build picks it by default (FeatureFlags.h:
// use_fontation_by_default() is true off Android) in makeSkTypeface. These
// definitions hand the font to FreeType instead, which reads the same files.
#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/ports/SkFontMgr_empty.h"
#include "include/ports/SkTypeface_fontations.h"

namespace {

sk_sp<SkFontMgr> FreeType() {
  static sk_sp<SkFontMgr> mgr = SkFontMgr_New_Custom_Empty();
  return mgr;
}

}  // namespace

sk_sp<SkTypeface> SkTypeface_Make_Fontations(std::unique_ptr<SkStreamAsset> fontData,
                                             const SkFontArguments& args) {
  return FreeType()->makeFromStream(std::move(fontData), args);
}

sk_sp<SkTypeface> SkTypeface_Make_Fontations(sk_sp<SkData> fontData, const SkFontArguments& args) {
  return FreeType()->makeFromStream(std::make_unique<SkMemoryStream>(std::move(fontData)), args);
}
