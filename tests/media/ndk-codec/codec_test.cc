// NDK AMediaCodec contract test (tests/media/ndk-codec/codec_test.cc).
// Compiled with Android NDK API 29 for x86_64 and arm64-v8a as both a
// standalone ELF executable and a JNI shared library.

#include <dlfcn.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaError.h>
#include <media/NdkMediaFormat.h>

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
    "AMediaCodec_setOnFrameRenderedCallback",
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

// Accessing this guest ELF TLS from a host-created notification thread also
// exercises EnterLoadedCode and the translated ARM64 callback return path.
thread_local uint32_t callback_entries = 0;

struct LifetimeState {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, delete_initial = false, flushed = false, sent = false;
  int errors = 0, formats = 0, outputs = 0;
};

void Complete(LifetimeState* state) {
  std::lock_guard<std::mutex> lock(state->mu);
  state->done = true;
  state->cv.notify_all();
}

void CheckNativeLifetime(Checker& check, bool delete_initial) {
  LifetimeState state;
  state.delete_initial = delete_initial;
  AMediaCodec* codec = AMediaCodec_createDecoderByType("audio/raw");
  check.Check(codec != nullptr, "raw lifetime decoder");
  if (codec == nullptr) return;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "audio/raw");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, 48000);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 2);
  check.Check(AMediaCodec_configure(codec, format, nullptr, nullptr, 0) == AMEDIA_OK,
              "raw lifetime configure");
  AMediaFormat_delete(format);
  AMediaCodecOnAsyncNotifyCallback callbacks{};
  callbacks.onAsyncInputAvailable = [](AMediaCodec* owner, void* opaque, int32_t index) {
    auto* s = static_cast<LifetimeState*>(opaque);
    ++callback_entries;
    if (s->delete_initial) {
      if (AMediaCodec_delete(owner) != AMEDIA_OK) ++s->errors;
      Complete(s);
      return;
    }
    if (!s->flushed) {
      s->flushed = true;
      if (AMediaCodec_flush(owner) != AMEDIA_OK) ++s->errors;
      if (AMediaCodec_start(owner) != AMEDIA_OK) ++s->errors;
      return;
    }
    if (s->sent) return;
    s->sent = true;
    size_t size = 0;
    auto* data = AMediaCodec_getInputBuffer(owner, static_cast<size_t>(index), &size);
    if (data == nullptr || size < 4) { ++s->errors; return; }
    std::memcpy(data, "abcd", 4);
    if (AMediaCodec_queueInputBuffer(owner, static_cast<size_t>(index), 0, 4, 1234,
                                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK) ++s->errors;
  };
  callbacks.onAsyncFormatChanged = [](AMediaCodec*, void* opaque, AMediaFormat* output) {
    auto* s = static_cast<LifetimeState*>(opaque);
    ++s->formats;
    int32_t rate = 0;
    if (output == nullptr || !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate) ||
        rate != 48000 || callback_entries == 0) ++s->errors;
    // The public NDK callback transfers this format to its recipient.
    if (output != nullptr) AMediaFormat_delete(output);
  };
  callbacks.onAsyncOutputAvailable = [](AMediaCodec* owner, void* opaque, int32_t index,
                                        AMediaCodecBufferInfo* info) {
    auto* s = static_cast<LifetimeState*>(opaque);
    ++s->outputs;
    if (callback_entries == 0 || info == nullptr) ++s->errors;
    if (info != nullptr && info->size > 0) {
      size_t size = 0;
      auto* data = AMediaCodec_getOutputBuffer(owner, static_cast<size_t>(index), &size);
      if (info->size != 4 || size < 4 || data == nullptr || std::memcmp(data, "abcd", 4) != 0 ||
          info->presentationTimeUs != 1234) ++s->errors;
    }
    const bool eos = info != nullptr && (info->flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    if (AMediaCodec_releaseOutputBuffer(owner, static_cast<size_t>(index), false) != AMEDIA_OK) ++s->errors;
    if (eos) {
      if (AMediaCodec_stop(owner) != AMEDIA_OK) ++s->errors;
      if (AMediaCodec_delete(owner) != AMEDIA_OK) ++s->errors;
      Complete(s);
    }
  };
  callbacks.onAsyncError = [](AMediaCodec* owner, void* opaque, media_status_t, int32_t, const char*) {
    auto* s = static_cast<LifetimeState*>(opaque);
    ++s->errors;
    AMediaCodec_delete(owner);
    Complete(s);
  };
  check.Check(AMediaCodec_setAsyncNotifyCallback(codec, callbacks, &state) == AMEDIA_OK,
              "lifetime callback aggregate and userdata");
  check.Check(AMediaCodec_start(codec) == AMEDIA_OK, "lifetime start survives callback deletion");
  std::unique_lock<std::mutex> lock(state.mu);
  if (!state.cv.wait_for(lock, std::chrono::seconds(10), [&] { return state.done; })) {
    std::cerr << "FAIL: callback lifetime operation timed out\n";
    std::_Exit(1);  // Do not let a deadlocked callback outlive its stack userdata.
  }
  check.Check(state.errors == 0, "native callbacks permit flush/restart/stop/delete with guest TLS");
  if (!delete_initial) check.Check(state.flushed && state.formats == 1 && state.outputs == 2,
                                    "restarted callback epoch delivers PCM and EOS exactly once");
}

}  // namespace

int RunCodecTests() {
#if defined(__aarch64__)
  std::cout << "ndk-codec ABI: arm64-v8a\n";
#elif defined(__x86_64__)
  std::cout << "ndk-codec ABI: x86_64\n";
#else
#error Unsupported NDK test ABI
#endif
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
        const size_t huge = (size_t{1} << 32) + static_cast<size_t>(idx);
        size_t rejected_capacity = 99;
        c.Check(AMediaCodec_getInputBuffer(enc, huge, &rejected_capacity) == nullptr &&
                    rejected_capacity == 99,
                "LP64 input index does not alias an owned slot");
        c.Check(AMediaCodec_queueInputBuffer(enc, huge, 0, 0, 0, 0) == AMEDIA_ERROR_INVALID_PARAMETER,
                "LP64 queue index is rejected before narrowing");
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
        if (buf == nullptr || cap < encoded[i].data.size()) break;
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
          if (info.size > 0) {
            saw_frame = true;
            AMediaFormat* buffer_format = AMediaCodec_getBufferFormat(dec, static_cast<size_t>(out_idx));
            int32_t width = 0;
            c.Check(buffer_format != nullptr &&
                        AMediaFormat_getInt32(buffer_format, AMEDIAFORMAT_KEY_WIDTH, &width) && width == 64,
                    "owned output has its publication format");
            if (buffer_format != nullptr) AMediaFormat_delete(buffer_format);
            // NdkMediaCodec wraps an unavailable format in an owned, empty one.
            AMediaFormat* alias = AMediaCodec_getBufferFormat(dec, (size_t{1} << 32) + out_idx);
            int32_t alias_width = 0;
            c.Check(alias != nullptr &&
                        !AMediaFormat_getInt32(alias, AMEDIAFORMAT_KEY_WIDTH, &alias_width),
                    "buffer format rejects LP64 alias");
            if (alias != nullptr) AMediaFormat_delete(alias);
          }
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
        if (fmt != nullptr) AMediaFormat_delete(fmt);
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

  CheckNativeLifetime(c, false);
  for (int repeat = 0; repeat < 8; ++repeat) CheckNativeLifetime(c, true);
  if (library != nullptr) dlclose(library);

  return c.failures;
}

extern "C" JNIEXPORT jint JNICALL Java_gesher_MediaCodecNdkTest_run(JNIEnv*, jclass) {
  std::cout << "ndk-codec caller: JNI\n";
  const int failures = RunCodecTests();
  std::cout << "ndk-codec: " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " failures)\n";
  return failures;
}

#ifdef MEDIA_CODEC_EXECUTABLE
int main() {
  std::cout << "ndk-codec caller: executable\n";
  const int failures = RunCodecTests();
  std::cout << "ndk-codec: " << (failures == 0 ? "PASS" : "FAIL") << " (" << failures
            << " failures)\n";
  return failures == 0 ? 0 : 1;
}
#endif
