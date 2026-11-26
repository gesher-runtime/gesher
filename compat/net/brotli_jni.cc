// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// The pinned AOSP Brotli decoder. No encoder, private dictionary or large-window
// extension is enabled for HTTP content decoding.
#include <brotli/decode.h>
#include <gesher/builtin_library.h>
#include <jni.h>
#include <nativehelper/jni_macros.h>

#include <algorithm>
#include <cstdint>
#include <new>

namespace {
constexpr size_t kChunk = 32768;
struct Decoder {
  BrotliDecoderState* state;
  uint8_t input[kChunk];
  uint8_t output[kChunk];
  ~Decoder() { BrotliDecoderDestroyInstance(state); }
};

void Throw(JNIEnv* env, const char* type, const char* message) {
  jclass cls = env->FindClass(type);
  if (cls != nullptr) {
    env->ThrowNew(cls, message);
    env->DeleteLocalRef(cls);
  }
}

jlong Create(JNIEnv* env, jclass) {
  auto* decoder = new (std::nothrow) Decoder;
  if (decoder != nullptr) {
    decoder->state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (decoder->state != nullptr) return reinterpret_cast<uintptr_t>(decoder);
    delete decoder;
  }
  Throw(env, "java/lang/OutOfMemoryError", "Cannot allocate Brotli decoder");
  return 0;
}

// Only the Java stream's cleanup-state monitor calls these methods. Region
// copies are bounded even if the caller supplied a very large destination:
// never pin/copy the whole Java array or hold a critical region across JNI.
jlong Process(JNIEnv* env, jclass, jlong handle, jbyteArray input, jint position,
              jint remaining, jbyteArray output, jint offset, jint length) {
  auto* decoder = reinterpret_cast<Decoder*>(static_cast<uintptr_t>(handle));
  if (decoder == nullptr) {
    Throw(env, "java/io/IOException", "Brotli decoder is closed");
    return 0;
  }
  if (input == nullptr || output == nullptr || position < 0 || remaining < 0 ||
      remaining > static_cast<jint>(kChunk) || offset < 0 || length <= 0 ||
      length > static_cast<jint>(kChunk) ||
      static_cast<int64_t>(position) + remaining > env->GetArrayLength(input) ||
      static_cast<int64_t>(offset) + length > env->GetArrayLength(output)) {
    Throw(env, "java/lang/IndexOutOfBoundsException", "Invalid Brotli buffer range");
    return 0;
  }
  if (remaining != 0) {
    env->GetByteArrayRegion(input, position, remaining,
                           reinterpret_cast<jbyte*>(decoder->input));
    if (env->ExceptionCheck()) return 0;
  }
  const uint8_t* next_input = decoder->input;
  uint8_t* next_output = decoder->output;
  size_t available_input = static_cast<size_t>(remaining);
  size_t available_output = static_cast<size_t>(length);
  BrotliDecoderResult status = BrotliDecoderDecompressStream(
      decoder->state, &available_input, &next_input, &available_output, &next_output, nullptr);
  if (status == BROTLI_DECODER_RESULT_ERROR) {
    Throw(env, "java/io/IOException", "Invalid Brotli response body");
    return 0;
  }
  size_t produced = static_cast<size_t>(length) - available_output;
  if (produced != 0) {
    env->SetByteArrayRegion(output, offset, static_cast<jsize>(produced),
                           reinterpret_cast<const jbyte*>(decoder->output));
    if (env->ExceptionCheck()) return 0;
  }
  // In/out counts are each at most 32768; the low two bits carry the provider
  // status. The protocol avoids a per-read result object or mutable global state.
  return static_cast<jlong>((static_cast<uint64_t>(remaining - available_input) << 32) |
                            (static_cast<uint64_t>(produced) << 2) | status);
}

void Destroy(JNIEnv*, jclass, jlong handle) {
  delete reinterpret_cast<Decoder*>(static_cast<uintptr_t>(handle));
}

const JNINativeMethod kMethods[] = {
    {"nativeCreate", "()J", NATIVEHELPER_SYSV_JNI_ENTRY(Create)},
    {"nativeProcess", "(J[BII[BII)J", NATIVEHELPER_SYSV_JNI_ENTRY(Process)},
    {"nativeDestroy", "(J)V", NATIVEHELPER_SYSV_JNI_ENTRY(Destroy)},
};

jint OnLoad(JavaVM* vm, void*) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  jclass cls = env->FindClass("android/net/http/BrotliInputStream");
  if (cls == nullptr) return JNI_ERR;
  jint result = env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(cls);
  return result == JNI_OK ? JNI_VERSION_1_6 : JNI_ERR;
}
const gesher_builtin_symbol kSymbols[] = {{"JNI_OnLoad", reinterpret_cast<void*>(&OnLoad)}};
[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libgesher_brotli.so", kSymbols,
                                  sizeof(kSymbols) / sizeof(kSymbols[0]));
  return true;
}();
}  // namespace
