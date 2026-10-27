// Value types used by the LP64 native muxer bridge (ADR 0064), matching
// android-16.0.0_r4 NdkMediaCodec.h and NdkMediaMuxer.h. The full codec header
// also declares unrelated off_t APIs whose sizeof(long) assertion is not
// valid on Windows LLP64; these declarations do not claim to port those APIs.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gesher::media::muxer_abi {

struct BufferInfo {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
};

enum OutputFormat {
  AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 = 0,
  AMEDIAMUXER_OUTPUT_FORMAT_WEBM = 1,
  AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP = 2,
  AMEDIAMUXER_OUTPUT_FORMAT_HEIF = 3,
  AMEDIAMUXER_OUTPUT_FORMAT_OGG = 4,
};

static_assert(sizeof(BufferInfo) == 24);
static_assert(offsetof(BufferInfo, offset) == 0);
static_assert(offsetof(BufferInfo, size) == 4);
static_assert(offsetof(BufferInfo, presentationTimeUs) == 8);
static_assert(offsetof(BufferInfo, flags) == 16);
static_assert(sizeof(OutputFormat) == 4);
static_assert(AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4 == 0 && AMEDIAMUXER_OUTPUT_FORMAT_WEBM == 1 &&
              AMEDIAMUXER_OUTPUT_FORMAT_THREE_GPP == 2 && AMEDIAMUXER_OUTPUT_FORMAT_HEIF == 3 &&
              AMEDIAMUXER_OUTPUT_FORMAT_OGG == 4);

}  // namespace gesher::media::muxer_abi
