// Gesher: the extractors' aconfig flags (frameworks/av media/module/extractors/
// extractor.aconfig), at their defaults: off (docs/media.md).
#pragma once

namespace com::android::media::extractor::flags {
inline bool extractor_sniff_midi_optimizations() { return false; }
inline bool extractor_mp4_enable_apv() { return false; }
inline bool extractor_mp4_enable_iamf() { return false; }
}  // namespace com::android::media::extractor::flags
