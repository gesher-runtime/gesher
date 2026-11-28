// ABI-matched NDK MediaCodec definitions for Windows LLP64 hosts
// (compat/media/ndk/media_codec.cc).
//
// Avoids NdkMediaCodec.h's off_t (32-bit on Windows LLP64 vs 64-bit on LP64
// Android) and pins exact LP64 guest struct layouts and System V callback ABIs.
#pragma once

#include <cstddef>
#include <cstdint>

#include <media/NdkMediaError.h>

struct AMediaCodec;
struct AMediaCodecCryptoInfo;
struct AMediaCrypto;
struct AMediaFormat;
struct ANativeWindow;

namespace gesher::media::codec_abi {

#if defined(__x86_64__)
#define GESHER_CODEC_GUEST_ABI __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define GESHER_CODEC_GUEST_ABI
#else
#error Unsupported NDK codec host architecture
#endif

using guest_off_t = int64_t;
using guest_ssize_t = int64_t;
static_assert(sizeof(guest_off_t) == 8);
static_assert(sizeof(guest_ssize_t) == 8);

inline constexpr uint32_t kConfigureFlagEncode = 1;
inline constexpr uint32_t kBufferFlagKeyFrame = 1;
inline constexpr uint32_t kBufferFlagCodecConfig = 2;
inline constexpr uint32_t kBufferFlagEndOfStream = 4;

inline constexpr guest_ssize_t kInfoTryAgainLater = -1;
inline constexpr guest_ssize_t kInfoOutputFormatChanged = -2;
inline constexpr guest_ssize_t kInfoOutputBuffersChanged = -3;

inline constexpr int32_t kActionCodeTransient = 1;
inline constexpr int32_t kActionCodeRecoverable = 2;

struct BufferInfo {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
};

static_assert(sizeof(BufferInfo) == 24);
static_assert(offsetof(BufferInfo, offset) == 0);
static_assert(offsetof(BufferInfo, size) == 4);
static_assert(offsetof(BufferInfo, presentationTimeUs) == 8);
static_assert(offsetof(BufferInfo, flags) == 16);

using OnAsyncInputAvailable = void(GESHER_CODEC_GUEST_ABI*)(
    AMediaCodec* codec, void* userdata, int32_t index);
using OnAsyncOutputAvailable = void(GESHER_CODEC_GUEST_ABI*)(
    AMediaCodec* codec, void* userdata, int32_t index, BufferInfo* bufferInfo);
using OnAsyncFormatChanged = void(GESHER_CODEC_GUEST_ABI*)(
    AMediaCodec* codec, void* userdata, AMediaFormat* format);
using OnAsyncError = void(GESHER_CODEC_GUEST_ABI*)(
    AMediaCodec* codec, void* userdata, media_status_t error, int32_t actionCode,
    const char* detail);

using OnFrameRendered = void(GESHER_CODEC_GUEST_ABI*)(
    AMediaCodec* codec, void* userdata, int64_t mediaTimeUs, int64_t systemNano);

struct OnAsyncNotifyCallback {
  OnAsyncInputAvailable onAsyncInputAvailable;
  OnAsyncOutputAvailable onAsyncOutputAvailable;
  OnAsyncFormatChanged onAsyncFormatChanged;
  OnAsyncError onAsyncError;
};

}  // namespace gesher::media::codec_abi
