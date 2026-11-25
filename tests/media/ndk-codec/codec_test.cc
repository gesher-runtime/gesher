// NDK AMediaCodec contract test (tests/media/ndk-codec/codec_test.cc).
// Compiled with Android NDK API 29 for x86_64 and arm64-v8a as both a
// standalone ELF executable and a JNI shared library.

#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char* kCodecSymbols[] = {
    "AMediaCodecActionCode_isRecoverable",
    "AMediaCodecActionCode_isTransient",
    "AMediaCodec_configure",
    "AMediaCodec_createCodecByName",
    "AMediaCodec_createDecoderByType",
    "AMediaCodec_createEncoderByType",
    "AMediaCodec_createInputSurface",
    "AMediaCodec_createPersistentInputSurface",
    "AMediaCodec_delete",
    "AMediaCodec_dequeueInputBuffer",
    "AMediaCodec_dequeueOutputBuffer",
    "AMediaCodec_flush",
    "AMediaCodec_getBufferFormat",
    "AMediaCodec_getInputBuffer",
    "AMediaCodec_getInputFormat",
    "AMediaCodec_getName",
    "AMediaCodec_getOutputBuffer",
    "AMediaCodec_getOutputFormat",
    "AMediaCodec_queueInputBuffer",
    "AMediaCodec_queueSecureInputBuffer",
    "AMediaCodec_releaseCrypto",
    "AMediaCodec_releaseName",
    "AMediaCodec_releaseOutputBuffer",
    "AMediaCodec_releaseOutputBufferAtTime",
    "AMediaCodec_setAsyncNotifyCallback",
    "AMediaCodec_setInputSurface",
    "AMediaCodec_setOutputSurface",
    "AMediaCodec_setParameters",
    "AMediaCodec_signalEndOfInputStream",
    "AMediaCodec_start",
    "AMediaCodec_stop",
};

struct Checker {
  int failures = 0;
  void Check(bool ok, const std::string& label) {
    if (!ok) {
      std::cerr << "FAIL: " << label << "\n";
      ++failures;
    }
  }
};

struct Packet {
  std::vector<uint8_t> data;
  int64_t time_us = 0;
  uint32_t flags = 0;
};

}  // namespace

int RunCodecTests() {
  Checker c;
  void* library = dlopen("libmediandk.so", RTLD_NOW);
  c.Check(library != nullptr, "dlopen libmediandk.so");
  for (const char* name : kCodecSymbols) {
    void* sym = library != nullptr ? dlsym(library, name) : nullptr;
    c.Check(sym != nullptr && sym == dlsym(RTLD_DEFAULT, name), std::string("symbol ") + name);
  }

  c.Check(AMediaCodec_createDecoderByType("video/x-not-supported") == nullptr,
          "unsupported decoder returns null");
  c.Check(AMediaCodec_createEncoderByType("audio/x-not-supported") == nullptr,
          "unsupported encoder returns null");
  c.Check(AMediaCodec_configure(nullptr, nullptr, nullptr, nullptr, 0) ==
              AMEDIA_ERROR_INVALID_OBJECT,
          "null configure returns INVALID_OBJECT");
  c.Check(AMediaCodecActionCode_isRecoverable(2) && !AMediaCodecActionCode_isRecoverable(1),
          "action code recoverable");
  c.Check(AMediaCodecActionCode_isTransient(1) && !AMediaCodecActionCode_isTransient(2),
          "action code transient");

  // 1. Encode a 64x64 I420 frame with AMediaCodec_createEncoderByType("video/avc").
  std::vector<Packet> encoded;
  {
    AMediaCodec* enc = AMediaCodec_createEncoderByType("video/avc");
    c.Check(enc != nullptr, "create avc encoder");
    if (enc != nullptr) {
      char* name = nullptr;
      c.Check(AMediaCodec_getName(enc, &name) == AMEDIA_OK && name != nullptr, "encoder getName");
      AMediaCodec_releaseName(enc, name);
      c.Check(AMediaCodec_queueSecureInputBuffer(enc, 0, 0, nullptr, 0, 0) ==
                  AMEDIA_ERROR_UNSUPPORTED,
              "queueSecureInputBuffer returns UNSUPPORTED");

      AMediaFormat* fmt = AMediaFormat_new();
      AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 64);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 64);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 250000);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, 30);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F420888);
      c.Check(AMediaCodec_configure(enc, fmt, nullptr, nullptr,
                                    AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK,
              "encoder configure");
      AMediaFormat_delete(fmt);
      c.Check(AMediaCodec_start(enc) == AMEDIA_OK, "encoder start");

      ssize_t idx = AMediaCodec_dequeueInputBuffer(enc, 5000000);
      c.Check(idx >= 0, "encoder dequeueInputBuffer");
      if (idx >= 0) {
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(enc, static_cast<size_t>(idx), &cap);
        const size_t yuv_size = 64 * 64 * 3 / 2;
        c.Check(buf != nullptr && cap >= yuv_size, "encoder getInputBuffer");
        if (buf != nullptr && cap >= yuv_size) {
          std::memset(buf, 128, yuv_size);
          c.Check(AMediaCodec_queueInputBuffer(enc, static_cast<size_t>(idx), 0, yuv_size, 0,
                                               AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == AMEDIA_OK,
                  "encoder queueInputBuffer EOS");
        }
      }

      bool saw_format = false;
      bool saw_eos = false;
      for (int iter = 0; iter < 32 && !saw_eos; ++iter) {
        AMediaCodecBufferInfo info{};
        ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(enc, &info, 2000000);
        if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
          saw_format = true;
          AMediaFormat* out_fmt = AMediaCodec_getOutputFormat(enc);
          c.Check(out_fmt != nullptr, "encoder getOutputFormat");
          if (out_fmt != nullptr) AMediaFormat_delete(out_fmt);
        } else if (out_idx >= 0) {
          size_t out_cap = 0;
          uint8_t* out_buf = AMediaCodec_getOutputBuffer(enc, static_cast<size_t>(out_idx), &out_cap);
          if (info.size > 0 && out_buf != nullptr) {
            Packet p;
            p.data.assign(out_buf + info.offset, out_buf + info.offset + info.size);
            p.time_us = info.presentationTimeUs;
            p.flags = info.flags;
            encoded.push_back(std::move(p));
          }
          if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) saw_eos = true;
          c.Check(AMediaCodec_releaseOutputBuffer(enc, static_cast<size_t>(out_idx), false) ==
                      AMEDIA_OK,
                  "encoder releaseOutputBuffer");
        }
      }
      c.Check(saw_format && saw_eos && !encoded.empty(), "encoder produced format, packets, eos");
      c.Check(AMediaCodec_stop(enc) == AMEDIA_OK, "encoder stop");
      c.Check(AMediaCodec_delete(enc) == AMEDIA_OK, "encoder delete");
    }
  }

  // 2. Decode those H.264 packets with AMediaCodec_createDecoderByType("video/avc").
  if (!encoded.empty()) {
    AMediaCodec* dec = AMediaCodec_createDecoderByType("video/avc");
    c.Check(dec != nullptr, "create avc decoder");
    if (dec != nullptr) {
      AMediaFormat* fmt = AMediaFormat_new();
      AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, 64);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, 64);
      c.Check(AMediaCodec_configure(dec, fmt, nullptr, nullptr, 0) == AMEDIA_OK,
              "decoder configure");
      AMediaFormat_delete(fmt);
      c.Check(AMediaCodec_start(dec) == AMEDIA_OK, "decoder start");

      for (size_t i = 0; i < encoded.size(); ++i) {
        ssize_t idx = AMediaCodec_dequeueInputBuffer(dec, 5000000);
        c.Check(idx >= 0, "decoder dequeueInputBuffer");
        if (idx < 0) break;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(dec, static_cast<size_t>(idx), &cap);
        c.Check(buf != nullptr && cap >= encoded[i].data.size(), "decoder getInputBuffer");
        std::memcpy(buf, encoded[i].data.data(), encoded[i].data.size());
        uint32_t flags = encoded[i].flags;
        if (i + 1 == encoded.size()) flags |= AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM;
        c.Check(AMediaCodec_queueInputBuffer(dec, static_cast<size_t>(idx), 0,
                                             encoded[i].data.size(),
                                             static_cast<uint64_t>(encoded[i].time_us),
                                             flags) == AMEDIA_OK,
                "decoder queueInputBuffer");
      }

      bool saw_format = false;
      bool saw_frame = false;
      bool saw_eos = false;
      for (int iter = 0; iter < 32 && !saw_eos; ++iter) {
        AMediaCodecBufferInfo info{};
        ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec, &info, 2000000);
        if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
          saw_format = true;
          AMediaFormat* out_fmt = AMediaCodec_getOutputFormat(dec);
          int32_t w = 0, h = 0;
          if (out_fmt != nullptr) {
            AMediaFormat_getInt32(out_fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(out_fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
            AMediaFormat_delete(out_fmt);
          }
          c.Check(w == 64 && h == 64, "decoder output format 64x64");
        } else if (out_idx >= 0) {
          if (info.size > 0) saw_frame = true;
          if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) saw_eos = true;
          c.Check(AMediaCodec_releaseOutputBuffer(dec, static_cast<size_t>(out_idx), false) ==
                      AMEDIA_OK,
                  "decoder releaseOutputBuffer");
        }
      }
      c.Check(saw_format && saw_frame && saw_eos, "decoder produced format, frame, eos");
      c.Check(AMediaCodec_flush(dec) == AMEDIA_OK, "decoder flush");
      c.Check(AMediaCodec_stop(dec) == AMEDIA_OK, "decoder stop");
      c.Check(AMediaCodec_delete(dec) == AMEDIA_OK, "decoder delete");
    }
  }

  // 3. Async notify callback check on Opus encoder.
  {
    struct AsyncState {
      std::mutex mu;
      std::condition_variable cv;
      bool got_input = false;
      bool got_output = false;
      bool got_eos = false;
      bool got_format = false;
    } state;

    AMediaCodec* opus = AMediaCodec_createEncoderByType("audio/opus");
    c.Check(opus != nullptr, "create opus encoder");
    if (opus != nullptr) {
      AMediaCodecOnAsyncNotifyCallback cb{};
      cb.onAsyncInputAvailable = [](AMediaCodec* codec, void* ud, int32_t idx) {
        auto* st = static_cast<AsyncState*>(ud);
        bool send = false;
        {
          std::lock_guard<std::mutex> lk(st->mu);
          if (!st->got_input) {
            st->got_input = true;
            send = true;
          }
        }
        if (send) {
          size_t cap = 0;
          uint8_t* buf = AMediaCodec_getInputBuffer(codec, static_cast<size_t>(idx), &cap);
          if (buf != nullptr && cap >= 1920) {
            std::memset(buf, 0, 1920);
            AMediaCodec_queueInputBuffer(codec, static_cast<size_t>(idx), 0, 1920, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
          }
        }
        st->cv.notify_all();
      };
      cb.onAsyncOutputAvailable = [](AMediaCodec* codec, void* ud, int32_t idx,
                                     AMediaCodecBufferInfo* info) {
        auto* st = static_cast<AsyncState*>(ud);
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(idx), false);
        {
          std::lock_guard<std::mutex> lk(st->mu);
          st->got_output = true;
          if (info != nullptr && (info->flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)) {
            st->got_eos = true;
          }
        }
        st->cv.notify_all();
      };
      cb.onAsyncFormatChanged = [](AMediaCodec*, void* ud, AMediaFormat* fmt) {
        auto* st = static_cast<AsyncState*>(ud);
        std::lock_guard<std::mutex> lk(st->mu);
        if (fmt != nullptr) st->got_format = true;
        st->cv.notify_all();
      };
      cb.onAsyncError = [](AMediaCodec*, void*, media_status_t, int32_t, const char*) {};

      c.Check(AMediaCodec_setAsyncNotifyCallback(opus, cb, &state) == AMEDIA_OK,
              "setAsyncNotifyCallback");
      AMediaFormat* fmt = AMediaFormat_new();
      AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/opus");
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 1);
      AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE, 64000);
      c.Check(AMediaCodec_configure(opus, fmt, nullptr, nullptr,
                                    AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK,
              "opus configure");
      AMediaFormat_delete(fmt);
      c.Check(AMediaCodec_start(opus) == AMEDIA_OK, "opus start");

      std::unique_lock<std::mutex> lk(state.mu);
      state.cv.wait_for(lk, std::chrono::seconds(5), [&] { return state.got_eos; });
      c.Check(state.got_input && state.got_format && state.got_output && state.got_eos,
              "async opus encode lifecycle");
      lk.unlock();
      c.Check(AMediaCodec_stop(opus) == AMEDIA_OK, "opus stop");
      c.Check(AMediaCodec_delete(opus) == AMEDIA_OK, "opus delete");
    }
  }

  return c.failures;
}

#ifdef MEDIA_CODEC_EXECUTABLE
int main() {
  const int failures = RunCodecTests();
  std::cout << "ndk-codec: " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures
            << " failures)\n";
  return failures == 0 ? 0 : 1;
}
#endif
