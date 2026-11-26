// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CODEC_STANDALONE
#include <jni.h>
#endif

enum { kWidth = 128, kHeight = 96, kLumaBytes = kWidth * kHeight, kFrameBytes = kLumaBytes * 3 / 2 };
enum { kFrames = 3 };
static const uint8_t kLuma[kFrames] = {48, 112, 192};
static const uint64_t kFirstTime = UINT64_C(0x100001234);
static const uint64_t kInterval = 33333;

struct Packet {
  uint8_t* data;
  size_t size;
  uint64_t time;
};

static bool Feed(AMediaCodec* codec, const uint8_t* data, size_t size, uint64_t time, uint32_t flags) {
  const ssize_t slot = AMediaCodec_dequeueInputBuffer(codec, 2000000);
  if (slot < 0) {
    printf("input dequeue returned %zd\n", slot);
    return false;
  }
  size_t capacity = 0;
  uint8_t* bytes = AMediaCodec_getInputBuffer(codec, (size_t)slot, &capacity);
  const size_t offset = size != 0 && capacity >= size + 8 ? 8 : 0;
  if (bytes == NULL || capacity < offset + size) {
    printf("input slot %zd provides %zu bytes for %zu\n", slot, capacity, size);
    return false;
  }
  if (size != 0) memcpy(bytes + offset, data, size);
  const media_status_t status = AMediaCodec_queueInputBuffer(codec, (size_t)slot, (int64_t)offset,
                                                            size, time, flags);
  if (status != AMEDIA_OK) printf("queue input returned %d\n", status);
  return status == AMEDIA_OK;
}

static bool Dimensions(AMediaFormat* format) {
  int32_t width = 0, height = 0;
  return format != NULL && AMediaFormat_getInt32(format, "width", &width) &&
      AMediaFormat_getInt32(format, "height", &height) && width == kWidth && height == kHeight;
}

static int RunCodec(void) {
  int checks = 0, failures = 0;
#if defined(__aarch64__)
  puts("NDK codec ABI: arm64-v8a");
#elif defined(__x86_64__)
  puts("NDK codec ABI: x86_64");
#endif
#define CHECK(expression, label) do { \
  ++checks; \
  if (!(expression)) { \
    ++failures; \
    printf("NDK codec failure: %s\n", label); \
  } \
} while (0)
#define REQUIRED(expression, label) do { \
  const int previous = failures; \
  CHECK(expression, label); \
  if (failures != previous) goto cleanup; \
} while (0)

  AMediaCodec* encoder = NULL;
  AMediaCodec* decoder = NULL;
  AMediaCodec* named = NULL;
  AMediaFormat* configuration = NULL;
  AMediaFormat* encoded = NULL;
  char* name = NULL;
  struct Packet packets[kFrames] = {{0}};
  int packet_count = 0;
  bool eos = false, wrote_config = false;
  encoder = AMediaCodec_createEncoderByType("video/avc");
  decoder = AMediaCodec_createDecoderByType("video/avc");
  REQUIRED(encoder != NULL && decoder != NULL, "create AVC encoder and decoder by MIME");
  CHECK(AMediaCodec_createDecoderByType("application/gesher-fake-codec") == NULL, "unavailable MIME");
  CHECK(AMediaCodec_createCodecByName("gesher.nonexistent.codec") == NULL, "unavailable codec name");
  const media_status_t early_start = AMediaCodec_start(encoder);
  const ssize_t early_dequeue = AMediaCodec_dequeueInputBuffer(encoder, 0);
  printf("NDK codec idle state start=%d dequeue=%zd\n", early_start, early_dequeue);
  CHECK(early_start == AMEDIA_ERROR_UNKNOWN, "start before configure reports the Android state error");
  CHECK(early_dequeue == AMEDIA_ERROR_UNKNOWN, "dequeue before start reports the Android state error");

  configuration = AMediaFormat_new();
  REQUIRED(configuration != NULL, "allocate AVC configuration");
  AMediaFormat_setString(configuration, "mime", "video/avc");
  AMediaFormat_setInt32(configuration, "width", kWidth);
  AMediaFormat_setInt32(configuration, "height", kHeight);
  AMediaFormat_setInt32(configuration, "color-format", 0x7f420888);
  AMediaFormat_setInt32(configuration, "bitrate", 200000);
  AMediaFormat_setInt32(configuration, "frame-rate", 30);
  AMediaFormat_setInt32(configuration, "i-frame-interval", 1);
  AMediaFormat_setInt32(configuration, "profile", 1);
  REQUIRED(AMediaCodec_configure(encoder, configuration, NULL, NULL,
                                AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK, "configure AVC encoder");
  AMediaFormat_delete(configuration);
  configuration = AMediaCodec_getInputFormat(encoder);
  REQUIRED(Dimensions(configuration), "copy accepted input format after freeing caller format");
  REQUIRED(AMediaCodec_getName(encoder, &name) == AMEDIA_OK && name != NULL && *name != '\0',
           "obtain actual codec component name");
  named = AMediaCodec_createCodecByName(name);
  REQUIRED(named != NULL, "reopen encoder by reported name");
  AMediaCodec_releaseName(encoder, name);
  name = NULL;
  REQUIRED(AMediaCodec_start(encoder) == AMEDIA_OK, "start AVC encoder");

  uint8_t frame[kFrameBytes];
  for (int i = 0; i < kFrames; ++i) {
    memset(frame, kLuma[i], kLumaBytes);
    memset(frame + kLumaBytes, 128, sizeof(frame) - kLumaBytes);
    REQUIRED(Feed(encoder, frame, sizeof(frame), kFirstTime + i * kInterval, 0), "queue raw AVC frame");
  }
  REQUIRED(Feed(encoder, NULL, 0, kFirstTime + kFrames * kInterval,
                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM), "queue encoder EOS");

  for (int attempt = 0; attempt < 24 && !eos; ++attempt) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(encoder, &info, 250000);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat_delete(encoded);
      encoded = AMediaCodec_getOutputFormat(encoder);
      CHECK(Dimensions(encoded), "AVC output format change includes coded dimensions");
      continue;
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
    if (index < 0) printf("encoder dequeue returned %zd\n", index);
    REQUIRED(index >= 0, "dequeue encoded buffer");
    if (info.size > 0) {
      size_t size = 0;
      const uint8_t* bytes = AMediaCodec_getOutputBuffer(encoder, (size_t)index, &size);
      // Android API 35 and earlier reported an invalid output size. This
      // contract relies only on the documented per-buffer info range.
      REQUIRED(bytes != NULL && info.offset >= 0, "access encoded bytes");
      if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
        wrote_config = true;
      } else {
        REQUIRED(packet_count < kFrames, "bounded number of AVC access units");
        struct Packet* packet = &packets[packet_count++];
        packet->size = (size_t)info.size;
        packet->time = (uint64_t)info.presentationTimeUs;
        packet->data = malloc(packet->size);
        REQUIRED(packet->data != NULL, "retain independent compressed bytes");
        memcpy(packet->data, bytes + info.offset, packet->size);
      }
    }
    eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    CHECK(AMediaCodec_releaseOutputBuffer(encoder, (size_t)index, false) == AMEDIA_OK, "release encoded buffer");
  }
  REQUIRED(eos && wrote_config && packet_count == kFrames, "real AVC config, access units and EOS");
  void* sps = NULL;
  size_t sps_size = 0;
  REQUIRED(encoded != NULL && AMediaFormat_getBuffer(encoded, "csd-0", &sps, &sps_size)
               && sps != NULL && sps_size > 4, "decoder receives encoded codec-specific data");
  REQUIRED(AMediaCodec_stop(encoder) == AMEDIA_OK, "stop encoder");
  CHECK(AMediaCodec_dequeueInputBuffer(encoder, 0) == AMEDIA_ERROR_UNKNOWN, "stop invalidates slots");

  REQUIRED(AMediaCodec_configure(decoder, encoded, NULL, NULL, 0) == AMEDIA_OK, "configure AVC decoder");
  AMediaFormat_delete(encoded);
  encoded = NULL;
  REQUIRED(AMediaCodec_start(decoder) == AMEDIA_OK, "start AVC decoder");
  for (int i = 0; i < packet_count; ++i) {
    CHECK(packets[i].time == kFirstTime + i * kInterval, "encoder preserves full-width timestamp");
    REQUIRED(Feed(decoder, packets[i].data, packets[i].size, packets[i].time, 0), "queue encoded access unit");
  }
  REQUIRED(Feed(decoder, NULL, 0, kFirstTime + kFrames * kInterval,
                AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM), "queue decoder EOS");

  eos = false;
  unsigned decoded = 0;
  for (int attempt = 0; attempt < 24 && !eos; ++attempt) {
    AMediaCodecBufferInfo info = {0};
    const ssize_t index = AMediaCodec_dequeueOutputBuffer(decoder, &info, 250000);
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* format = AMediaCodec_getOutputFormat(decoder);
      CHECK(Dimensions(format), "decoded output reports visible dimensions");
      AMediaFormat_delete(format);
      continue;
    }
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER || index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
    if (index < 0) printf("decoder dequeue returned %zd\n", index);
    REQUIRED(index >= 0, "dequeue decoded buffer");
    if (info.size > 0) {
      const uint8_t* bytes = AMediaCodec_getOutputBuffer(decoder, (size_t)index, NULL);
      const uint64_t time = (uint64_t)info.presentationTimeUs;
      const uint64_t ordinal = (time - kFirstTime) / kInterval;
      REQUIRED(bytes != NULL && info.offset >= 0 && info.size >= kWidth, "access actual decoded pixels");
      REQUIRED(time >= kFirstTime && ordinal < kFrames && time == kFirstTime + ordinal * kInterval &&
                   (decoded & (1U << ordinal)) == 0, "one frame per submitted full-width timestamp");
      decoded |= 1U << ordinal;
      const int luma = bytes[info.offset + kWidth / 2];
      printf("NDK codec AVC frame pts=%" PRIu64 " luma=%d bytes=%d\n", time, luma, info.size);
      CHECK(abs(luma - kLuma[ordinal]) <= 8, "decoded luma agrees with supplied image");
      AMediaFormat* format = AMediaCodec_getBufferFormat(decoder, (size_t)index);
      CHECK(Dimensions(format), "owned output slot exposes its format");
      AMediaFormat_delete(format);
    }
    eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
    CHECK(AMediaCodec_releaseOutputBuffer(decoder, (size_t)index, false) == AMEDIA_OK, "release decoded buffer");
  }
  CHECK(eos && decoded == (1U << kFrames) - 1, "all actual frames and EOS");
  CHECK(AMediaCodec_flush(decoder) == AMEDIA_OK, "flush decoder after EOS");
  const ssize_t reused = AMediaCodec_dequeueInputBuffer(decoder, 2000000);
  CHECK(reused >= 0, "flushed decoder exposes input again");
  CHECK(AMediaCodec_stop(decoder) == AMEDIA_OK, "stop decoder");

cleanup:
  if (name != NULL) AMediaCodec_releaseName(encoder, name);
  if (configuration != NULL) AMediaFormat_delete(configuration);
  if (encoded != NULL) AMediaFormat_delete(encoded);
  if (encoder != NULL) AMediaCodec_delete(encoder);
  if (decoder != NULL) AMediaCodec_delete(decoder);
  if (named != NULL) AMediaCodec_delete(named);
  for (int i = 0; i < kFrames; ++i) free(packets[i].data);
  printf("NDK codec AVC: %d checks, %d failures\n", checks, failures);
  if (failures == 0) puts("NDK codec AVC: all passed");
  return failures == 0 ? 0 : 1;
#undef REQUIRED
#undef CHECK
}

#ifdef CODEC_STANDALONE
int main(void) { return RunCodec(); }
#else
JNIEXPORT jint JNICALL Java_gesher_NdkCodecTest_run(JNIEnv* env, jclass type) {
  (void)env;
  (void)type;
  return RunCodec();
}
#endif
