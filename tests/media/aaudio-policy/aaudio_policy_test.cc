// Android 16 policy extensions and real legacy streams through the guest NDK ABI.
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <jni.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>

namespace {

// API 36 extensions are resolved dynamically; the CI NDK r27 headers are older.
using GetPolicy = int32_t (*)();
using SetPolicy = aaudio_result_t (*)(int32_t);
using IsMMap = bool (*)(AAudioStream*);
constexpr int32_t kUnspecified = 0, kNever = 1, kAuto = 2, kAlways = 3;
constexpr aaudio_result_t kNoService = -881;
static_assert(sizeof(aaudio_result_t) == 4 && sizeof(bool) == 1 && sizeof(void*) == 8);

struct Report {
  std::string text;
  int failed = 0;
  void Check(bool ok, const std::string& detail) {
    if (!ok) { text += "media4 aaudio failure: " + detail + "\n"; ++failed; }
  }
  void Part(const char* name, int before, const char* detail) {
    text += "media4 aaudio " + std::string(name) + ": " +
            (failed == before ? "ok; " : "FAILED; ") + detail + "\n";
  }
};

struct Builder {
  AAudioStreamBuilder* value = nullptr;
  ~Builder() { if (value != nullptr) AAudioStreamBuilder_delete(value); }
};

struct Stream {
  AAudioStream* value = nullptr;
  ~Stream() { if (value != nullptr) AAudioStream_close(value); }
};

struct PolicyGuard {
  SetPolicy set;
  int32_t old;
  ~PolicyGuard() { set(old); }
};

void Configure(AAudioStreamBuilder* builder, aaudio_direction_t direction) {
  AAudioStreamBuilder_setDirection(builder, direction);
  AAudioStreamBuilder_setSampleRate(builder, 44100);
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
  AAudioStreamBuilder_setBufferCapacityInFrames(builder, 44100);
}

std::string Run(bool allowed, bool has_manifest) {
  Report r;
#if defined(__aarch64__)
  r.text = "media4 aaudio ABI: arm64-v8a\n";
#else
  r.text = "media4 aaudio ABI: x86_64\n";
#endif
  r.text += "media4 aaudio caller: JNI\n";
  int before = r.failed;
  void* library = dlopen("libaaudio.so", RTLD_NOW);
  void* system = dlopen("/system/lib64/libaaudio.so", RTLD_NOW);
  auto get = reinterpret_cast<GetPolicy>(library != nullptr ? dlsym(library, "AAudio_getMMapPolicy") : nullptr);
  auto set = reinterpret_cast<SetPolicy>(library != nullptr ? dlsym(library, "AAudio_setMMapPolicy") : nullptr);
  auto used = reinterpret_cast<IsMMap>(library != nullptr ? dlsym(library, "AAudioStream_isMMapUsed") : nullptr);
  r.Check(library != nullptr && system != nullptr && get != nullptr && set != nullptr && used != nullptr,
          "actual policy/identity symbols through both library paths");
  if (get == nullptr || set == nullptr || used == nullptr || system == nullptr) return r.text;
  r.Check(dlsym(system, "AAudio_getMMapPolicy") == reinterpret_cast<void*>(get) &&
          dlsym(system, "AAudio_setMMapPolicy") == reinterpret_cast<void*>(set) &&
          dlsym(system, "AAudioStream_isMMapUsed") == reinterpret_cast<void*>(used),
          "bare and system paths share the exact implementation");
  r.Part("exports", before, "typed API36 policy and stream identity through both ELF library paths");
  {
    const int32_t original = get();
    PolicyGuard guard{set, original};
    before = r.failed;
    r.Check(original == kUnspecified, "process policy starts UNSPECIFIED, separately from backend NEVER");
    for (int32_t policy : {kUnspecified, kNever, kAuto, kAlways}) {
      r.Check(set(policy) == AAUDIO_OK && get() == policy, "valid policy round trip " + std::to_string(policy));
      for (int32_t invalid : {std::numeric_limits<int32_t>::min(), -1, 4, std::numeric_limits<int32_t>::max()}) {
        const auto result = set(invalid);
        r.Check(result == AAUDIO_ERROR_ILLEGAL_ARGUMENT && get() == policy,
                "invalid policy preserves selection " + std::to_string(invalid));
        r.text += "media4 aaudio policy trace: selected " + std::to_string(policy) +
                  ", invalid " + std::to_string(invalid) + ", result " +
                  std::to_string(result) + ", current " + std::to_string(get()) + "\n";
      }
    }
    r.Part("policy", before, "initial0, valid0/1/2/3 and invalid signed32 sentinels preserve the selection");

    before = r.failed;
    for (int32_t policy : {kUnspecified, kNever, kAuto}) {
      r.Check(set(policy) == AAUDIO_OK, "select legacy policy");
      Builder builder;
      r.Check(AAudio_createStreamBuilder(&builder.value) == AAUDIO_OK && builder.value != nullptr,
              "create output builder");
      if (builder.value == nullptr) continue;
      Configure(builder.value, AAUDIO_DIRECTION_OUTPUT);
      Stream stream;
      const auto result = AAudioStreamBuilder_openStream(builder.value, &stream.value);
      r.Check(result == AAUDIO_OK && stream.value != nullptr, "open real shared output under policy " + std::to_string(policy));
      if (stream.value == nullptr) continue;
      r.Check(!used(stream.value), "actual legacy output identity");
      r.Check(AAudioStream_requestStart(stream.value) == AAUDIO_OK, "start real shared output");
      std::array<int16_t, 64> samples{};
      for (size_t i = 0; i < samples.size(); ++i) samples[i] = i % 2 == 0 ? 1000 : -1000;
      const auto written = AAudioStream_write(stream.value, samples.data(), 32, 1000000000LL);
      r.Check(written == 32 && AAudioStream_getFramesWritten(stream.value) >= 32, "real PCM output accepts frames");
      r.Check(set(kAlways) == AAUDIO_OK && !used(stream.value), "changing global policy leaves live output legacy");
      r.Check(AAudioStream_write(stream.value, samples.data(), 32, 1000000000LL) == 32,
              "live output remains functional after global policy change");
      r.Check(AAudioStream_requestStop(stream.value) == AAUDIO_OK, "stop output");
      r.Check(AAudioStream_close(stream.value) == AAUDIO_OK, "close output");
      stream.value = nullptr;
      r.text += "media4 aaudio backend trace: requested " + std::to_string(policy) +
                ", result " + std::to_string(result) + ", mmap false, written " + std::to_string(written) +
                ", live policy " + std::to_string(get()) + "\n";
    }
    r.Part("backend", before, "UNSPECIFIED/NEVER/AUTO use real PCM legacy output, live identity survives policy changes");

    before = r.failed;
    r.Check(set(kAlways) == AAUDIO_OK, "select unavailable required MMAP");
    for (auto direction : {AAUDIO_DIRECTION_OUTPUT, AAUDIO_DIRECTION_INPUT}) {
      Builder builder;
      r.Check(AAudio_createStreamBuilder(&builder.value) == AAUDIO_OK && builder.value != nullptr,
              "create required-MMAP builder");
      if (builder.value == nullptr) continue;
      Configure(builder.value, direction);
      AAudioStream* rejected = reinterpret_cast<AAudioStream*>(uintptr_t{1});
      const auto result = AAudioStreamBuilder_openStream(builder.value, &rejected);
      r.Check(result == kNoService && rejected == nullptr, "ALWAYS cannot silently fall back or retain a stale stream");
      if (result == AAUDIO_OK && rejected != nullptr &&
          rejected != reinterpret_cast<AAudioStream*>(uintptr_t{1})) AAudioStream_close(rejected);
      r.text += "media4 aaudio always trace: direction " + std::to_string(direction) +
                ", result " + std::to_string(result) + ", null " + (rejected == nullptr ? "true\n" : "false\n");
      AAudioStreamBuilder_setFormat(builder.value, static_cast<aaudio_format_t>(12345));
      rejected = reinterpret_cast<AAudioStream*>(uintptr_t{1});
      const auto invalid = AAudioStreamBuilder_openStream(builder.value, &rejected);
      r.Check(invalid == AAUDIO_ERROR_INVALID_FORMAT && rejected == nullptr,
              "format validation precedes policy selection and nulls output");
      if (invalid == AAUDIO_OK && rejected != nullptr &&
          rejected != reinterpret_cast<AAudioStream*>(uintptr_t{1})) AAudioStream_close(rejected);
    }
    r.Check(std::strcmp(AAudio_convertResultToText(kNoService), "AAUDIO_ERROR_NO_SERVICE") == 0,
            "exact NO_SERVICE error text");
    r.Part("always", before, "no MMAP service returns -881/null for both directions, format validation takes priority");

    before = r.failed;
    r.Check(set(kAuto) == AAUDIO_OK, "restore fallback before permission check");
    Builder input;
    r.Check(AAudio_createStreamBuilder(&input.value) == AAUDIO_OK && input.value != nullptr,
            "create permission-aware input builder");
    if (input.value != nullptr) {
      Configure(input.value, AAUDIO_DIRECTION_INPUT);
      Stream stream;
      const auto result = AAudioStreamBuilder_openStream(input.value, &stream.value);
      r.text += "media4 aaudio permission trace: expected " + std::string(allowed ? "allowed" : "denied") +
                ", result " + std::to_string(result) + ", object " + (stream.value != nullptr ? "true\n" : "false\n");
      if (allowed) {
        r.Check(result == AAUDIO_OK && stream.value != nullptr, "manifest grants the real input stream");
        if (stream.value != nullptr) {
          r.Check(!used(stream.value), "actual input is legacy");
          r.Check(AAudioStream_requestStart(stream.value) == AAUDIO_OK, "start permitted capture");
          std::array<int16_t, 256> samples{};
          const auto read = AAudioStream_read(stream.value, samples.data(), 128, 2000000000LL);
          bool nonzero = false;
          for (int16_t sample : samples) nonzero |= sample != 0;
          r.Check(read == 128 && nonzero && AAudioStream_getFramesRead(stream.value) == 128,
                  "actual file microphone supplies PCM frames");
          r.Check(set(kAlways) == AAUDIO_OK && !used(stream.value), "live input identity does not change");
          r.Check(AAudioStream_requestStop(stream.value) == AAUDIO_OK, "stop permitted capture");
          r.Check(AAudioStream_close(stream.value) == AAUDIO_OK, "close permitted capture");
          stream.value = nullptr;
          r.text += "media4 aaudio input trace: read " + std::to_string(read) + ", nonzero " +
                    (nonzero ? "true" : "false") + ", mmap false, live policy " + std::to_string(get()) + "\n";
        }
      } else {
        r.Check(result == AAUDIO_ERROR_UNAVAILABLE && stream.value == nullptr,
                "denied/no-identity capture remains unavailable without a stale object");
      }
    }
    r.Part("permission", before, allowed ? "manifest allowed actual legacy capture/read/stop" :
           has_manifest ? "manifest denied input returns -889 without object" :
                          "bare core without recording identity returns -889 without object");
  }
  r.Check(get() == kUnspecified, "restore the original process policy");
  dlclose(system);
  dlclose(library);
  r.text += r.failed == 0 ? "media4 aaudio: all passed\n" : "media4 aaudio: FAILED\n";
  return r.text;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_gesher_AAudioPolicyTest_run(JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(Run(allowed, false).c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_com_gesher_aaudiopolicy_AAudioPolicyActivity_run(
    JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(Run(allowed, true).c_str());
}
