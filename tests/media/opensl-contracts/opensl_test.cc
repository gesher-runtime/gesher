// OpenSL C contracts through the actual app NDK ABI (media4, ADR 0064).
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
#include <jni.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace {

struct Report {
  std::string text;
  int failed = 0;
  void Check(bool ok, const std::string& detail) {
    if (!ok) { text += "media4 opensl failure: " + detail + "\n"; ++failed; }
  }
  void Part(const char* name, int before, const char* detail) {
    text += "media4 opensl " + std::string(name) + ": " + (failed == before ? "ok; " : "FAILED; ") + detail + "\n";
  }
};

struct Object {
  SLObjectItf value = nullptr;
  ~Object() { if (value != nullptr) (*value)->Destroy(value); }
};

bool Same(SLInterfaceID a, SLInterfaceID b) {
  return a != nullptr && b != nullptr && std::memcmp(a, b, sizeof(*a)) == 0;
}

struct Id {
  const char* name;
  const SLInterfaceID* object;
  SLInterfaceID_ expected;
};
const Id kIds[] = {
#define PINNED_OPENSL_IID(name, ...) {#name, &name, {__VA_ARGS__}},
#include "pinned_iids.inc"
#undef PINNED_OPENSL_IID
};
static_assert(sizeof(SLInterfaceID_) == 16 && alignof(SLInterfaceID_) == 4);
static_assert(sizeof(kIds) / sizeof(kIds[0]) == 53);

std::string Run(bool allowed) {
  Report r;
#if defined(__aarch64__)
  r.text = "media4 opensl ABI: arm64-v8a\n";
#else
  r.text = "media4 opensl ABI: x86_64\n";
#endif
  r.text += "media4 opensl caller: JNI\n";
  int before = r.failed;
  void* library = dlopen("libOpenSLES.so", RTLD_NOW);
  void* system = dlopen("/system/lib64/libOpenSLES.so", RTLD_NOW);
  r.Check(library != nullptr && system != nullptr, "bare and system handles");
  for (const auto& id : kIds) {
    auto* dynamic = static_cast<const SLInterfaceID*>(library != nullptr ? dlsym(library, id.name) : nullptr);
    auto* absolute = static_cast<const SLInterfaceID*>(system != nullptr ? dlsym(system, id.name) : nullptr);
    auto* global = static_cast<const SLInterfaceID*>(dlsym(RTLD_DEFAULT, id.name));
    r.Check(Same(*id.object, &id.expected), std::string(id.name) + " pinned relocated UUID");
    r.Check(dynamic != nullptr && absolute != nullptr && global != nullptr &&
                Same(*dynamic, &id.expected) && Same(*absolute, &id.expected) && Same(*global, &id.expected),
            std::string(id.name) + " actual pointer data objects");
  }
  r.Part("data", before, "53 canonical UUID pointer objects, exact pinned fields and ELF indirection");

  before = r.failed;
  Object engine;
  r.Check(slCreateEngine(&engine.value, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS &&
              engine.value != nullptr, "create engine");
  if (engine.value == nullptr) return r.text + "media4 opensl: FAILED\n";
  void* output = reinterpret_cast<void*>(uintptr_t{0x1234});
  r.Check((*engine.value)->GetInterface(engine.value, SL_IID_ENGINE, &output) ==
              SL_RESULT_PRECONDITIONS_VIOLATED && output == nullptr, "unrealized engine output reset");
  r.Check((*engine.value)->GetInterface(engine.value, SL_IID_OBJECT, &output) == SL_RESULT_SUCCESS &&
              output != nullptr, "implicit prerealize object");
  r.Check((*engine.value)->Realize(engine.value, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize engine");
  SLEngineItf api = nullptr;
  r.Check((*engine.value)->GetInterface(engine.value, SL_IID_ENGINE, &api) == SL_RESULT_SUCCESS &&
              api != nullptr, "engine interface");
  if (api == nullptr) return r.text + "media4 opensl: FAILED\n";
  SLuint32 count = 777;
  r.Check(slQueryNumSupportedEngineInterfaces(&count) == SL_RESULT_SUCCESS && count == 2,
          "typed global engine count");
  r.Check(slQueryNumSupportedEngineInterfaces(nullptr) == SL_RESULT_PARAMETER_INVALID, "null global count");
  const SLuint32 kinds[] = {SL_OBJECTID_ENGINE, SL_OBJECTID_OUTPUTMIX, SL_OBJECTID_AUDIOPLAYER,
                            SL_OBJECTID_AUDIORECORDER};
  const SLuint32 counts[] = {2, 1, 8, 5};
  for (size_t k = 0; k < 4; k++) {
    count = 777;
    r.Check((*api)->QueryNumSupportedInterfaces(api, kinds[k], &count) == SL_RESULT_SUCCESS &&
                count == counts[k], "three-argument query count " + std::to_string(k));
    for (SLuint32 n = 0; n < count && n < 16; n++) {
      SLInterfaceID id = nullptr;
      r.Check((*api)->QuerySupportedInterfaces(api, kinds[k], n, &id) == SL_RESULT_SUCCESS && id != nullptr,
              "four-argument interface query");
      bool known = false;
      for (const auto& candidate : kIds) if (Same(id, &candidate.expected)) known = true;
      r.Check(known, "queried canonical identifier");
    }
    SLInterfaceID id = SL_IID_NULL;
    r.Check((*api)->QuerySupportedInterfaces(api, kinds[k], count, &id) == SL_RESULT_PARAMETER_INVALID &&
                id == nullptr, "past-end query resets output");
  }
  count = 777;
  r.Check((*api)->QueryNumSupportedInterfaces(api, 0xf001, &count) == SL_RESULT_FEATURE_UNSUPPORTED &&
              count == 777, "unknown object count preserves output");
  SLInterfaceID id = SL_IID_NULL;
  r.Check((*api)->QuerySupportedInterfaces(api, 0xf001, 0, &id) == SL_RESULT_FEATURE_UNSUPPORTED &&
              id == nullptr, "unknown object interface resets output");
  r.Check((*api)->QueryNumSupportedInterfaces(api, SL_OBJECTID_ENGINE, nullptr) == SL_RESULT_PARAMETER_INVALID &&
              (*api)->QuerySupportedInterfaces(api, SL_OBJECTID_ENGINE, 0, nullptr) == SL_RESULT_PARAMETER_INVALID,
          "typed null query outputs");
  for (SLuint32 n = 0; n < 2; n++) {
    id = nullptr;
    r.Check(slQuerySupportedEngineInterfaces(n, &id) == SL_RESULT_SUCCESS &&
                Same(id, n == 0 ? SL_IID_OBJECT : SL_IID_ENGINE), "global query canonical value");
  }
  id = SL_IID_ENGINE;
  r.Check(slQuerySupportedEngineInterfaces(2, &id) == SL_RESULT_PARAMETER_INVALID && id == nullptr,
          "global past-end output reset");
  r.Part("query", before, "typed global and engine queries, supported subsets, null/index/object controls");

  before = r.failed;
  const SLboolean required = SL_BOOLEAN_TRUE, optional = SL_BOOLEAN_FALSE;
  for (const auto& candidate : kIds) {
    SLInterfaceID_ copy = candidate.expected;
    SLInterfaceID requested = &copy;
    bool supported = Same(requested, SL_IID_OBJECT) || Same(requested, SL_IID_ENGINE);
    output = reinterpret_cast<void*>(uintptr_t{0x1234});
    SLresult result = (*engine.value)->GetInterface(engine.value, requested, &output);
    r.Check(result == (supported ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED) &&
                (supported ? output != nullptr : output == nullptr), std::string(candidate.name) + " copied-ID lookup");
    Object needed;
    result = slCreateEngine(&needed.value, 0, nullptr, 1, &requested, &required);
    r.Check(result == (supported ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED) &&
                (supported ? needed.value != nullptr : needed.value == nullptr),
            std::string(candidate.name) + " required interface result");
    Object maybe;
    r.Check(slCreateEngine(&maybe.value, 0, nullptr, 1, &requested, &optional) == SL_RESULT_SUCCESS &&
                maybe.value != nullptr, std::string(candidate.name) + " optional interface accepted");
  }
  SLInterfaceID_ copied = *SL_IID_ENGINE;
  const SLInterfaceID duplicates[] = {SL_IID_ENGINE, &copied};
  const SLboolean flags[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  Object duplicate;
  r.Check(slCreateEngine(&duplicate.value, 0, nullptr, 2, duplicates, flags) == SL_RESULT_SUCCESS,
          "duplicate aliased and copied requests");
  SLObjectItf bad = reinterpret_cast<SLObjectItf>(uintptr_t{0x1234});
  r.Check(slCreateEngine(&bad, 0, nullptr, 1, nullptr, &required) == SL_RESULT_PARAMETER_INVALID &&
              bad == nullptr, "missing IDs resets object");
  bad = reinterpret_cast<SLObjectItf>(uintptr_t{0x1234});
  r.Check(slCreateEngine(&bad, 0, nullptr, 1, &id, nullptr) == SL_RESULT_PARAMETER_INVALID &&
              bad == nullptr, "missing required flags resets object");
  const SLInterfaceID null_id = nullptr;
  r.Check(slCreateEngine(&bad, 0, nullptr, 1, &null_id, &required) == SL_RESULT_PARAMETER_INVALID &&
              bad == nullptr, "null individual identifier");
  SLInterfaceID_ unknown{};
  const SLInterfaceID unknown_id = &unknown;
  r.Check(slCreateEngine(&bad, 0, nullptr, 1, &unknown_id, &required) == SL_RESULT_FEATURE_UNSUPPORTED &&
              bad == nullptr, "unknown UUID required fails without object");
  Object optional_unknown;
  r.Check(slCreateEngine(&optional_unknown.value, 0, nullptr, 1, &unknown_id, &optional) == SL_RESULT_SUCCESS &&
              optional_unknown.value != nullptr, "unknown UUID optional accepted");
  r.Part("negotiation", before, "copied UUIDs, all 53 required/optional engine outcomes, duplicate and invalid arrays");

  before = r.failed;
  Object mix;
  r.Check((*api)->CreateOutputMix(api, &mix.value, 0, nullptr, nullptr) == SL_RESULT_SUCCESS &&
              mix.value != nullptr, "create output mix");
  if (mix.value == nullptr) return r.text + "media4 opensl: FAILED\n";
  r.Check((*mix.value)->Realize(mix.value, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize mix");
  const SLInterfaceID player_ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
  SLDataLocator_AndroidSimpleBufferQueue queue{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
  SLDataFormat_PCM pcm{SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                      SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                      SL_BYTEORDER_LITTLEENDIAN};
  SLDataSource source{&queue, &pcm};
  SLDataLocator_OutputMix destination{SL_DATALOCATOR_OUTPUTMIX, mix.value};
  SLDataSink sink{&destination, nullptr};
  Object player;
  r.Check((*api)->CreateAudioPlayer(api, &player.value, &source, &sink, 2, player_ids, flags) ==
              SL_RESULT_SUCCESS && player.value != nullptr, "required PCM queue and volume");
  if (player.value != nullptr) {
    r.Check((*player.value)->Realize(player.value, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize PCM player");
    SLVolumeItf volume = nullptr;
    SLAndroidSimpleBufferQueueItf buffers = nullptr;
    SLPlayItf play = nullptr;
    r.Check((*player.value)->GetInterface(player.value, SL_IID_VOLUME, &volume) == SL_RESULT_SUCCESS &&
                (*player.value)->GetInterface(player.value, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &buffers) == SL_RESULT_SUCCESS &&
                (*player.value)->GetInterface(player.value, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS &&
                volume != nullptr && buffers != nullptr && play != nullptr, "actual requested and implicit interfaces");
    if (volume != nullptr && buffers != nullptr && play != nullptr) {
      SLmillibel level = 0;
      r.Check((*volume)->SetVolumeLevel(volume, -600) == SL_RESULT_SUCCESS &&
                  (*volume)->GetVolumeLevel(volume, &level) == SL_RESULT_SUCCESS && level == -600,
              "actual PCM volume round trip");
      int16_t silence[882] = {};
      SLAndroidSimpleBufferQueueState state{};
      r.Check((*buffers)->Enqueue(buffers, silence, sizeof(silence)) == SL_RESULT_SUCCESS &&
                  (*buffers)->GetState(buffers, &state) == SL_RESULT_SUCCESS && state.count == 1 &&
                  (*buffers)->Clear(buffers) == SL_RESULT_SUCCESS &&
                  (*buffers)->GetState(buffers, &state) == SL_RESULT_SUCCESS && state.count == 0,
              "real PCM queue accepts and clears caller buffer");
    }
    output = reinterpret_cast<void*>(uintptr_t{0x1234});
    r.Check((*player.value)->GetInterface(player.value, SL_IID_ANDROIDCONFIGURATION, &output) ==
                SL_RESULT_FEATURE_UNSUPPORTED && output == nullptr, "unrequested explicit interface unavailable");
  }
  const SLInterfaceID unavailable = SL_IID_EQUALIZER;
  r.Check((*api)->CreateAudioPlayer(api, &bad, &source, &sink, 1, &unavailable, &required) ==
              SL_RESULT_FEATURE_UNSUPPORTED && bad == nullptr, "unsupported required PCM effect rejected");
  r.Check((*api)->CreateOutputMix(api, &bad, 1, &unavailable, &required) ==
              SL_RESULT_FEATURE_UNSUPPORTED && bad == nullptr, "unsupported required mix effect rejected");
  r.Part("player", before, "real PCM queue and volume, explicit exposure and unsupported effect rejection");

  before = r.failed;
  SLDataLocator_IODevice input{SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
                              SL_DEFAULTDEVICEID_AUDIOINPUT, nullptr};
  SLDataSource capture{&input, nullptr};
  SLDataSink recorded{&queue, &pcm};
  const SLInterfaceID record_ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
  r.Check((*api)->CreateAudioRecorder(api, &bad, &capture, &recorded, 1, &unavailable, &required) ==
              SL_RESULT_FEATURE_UNSUPPORTED && bad == nullptr, "unsupported required recorder effect rejected");
  Object recorder;
  SLresult result = (*api)->CreateAudioRecorder(api, &recorder.value, &capture, &recorded, 2, record_ids, flags);
  r.Check(result == (allowed ? SL_RESULT_SUCCESS : SL_RESULT_PERMISSION_DENIED) &&
              (allowed ? recorder.value != nullptr : recorder.value == nullptr),
          "actual native RECORD_AUDIO allow/deny result (permission constant 0x0B)");
  if (allowed && recorder.value != nullptr) {
    SLAndroidConfigurationItf config = nullptr;
    r.Check((*recorder.value)->GetInterface(recorder.value, SL_IID_ANDROIDCONFIGURATION, &config) ==
                SL_RESULT_SUCCESS && config != nullptr, "requested configuration available prerealize");
    r.Check((*recorder.value)->Realize(recorder.value, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS, "realize recorder");
    SLRecordItf record = nullptr;
    r.Check((*recorder.value)->GetInterface(recorder.value, SL_IID_RECORD, &record) == SL_RESULT_SUCCESS &&
                record != nullptr, "implicit native recorder interface");
    if (record != nullptr) {
      SLuint32 state = 0;
      r.Check((*record)->GetRecordState(record, &state) == SL_RESULT_SUCCESS && state == SL_RECORDSTATE_STOPPED,
              "actual initial native recorder state");
    }
  }
  r.Part("permission", before, allowed ? "manifest allowed native recorder lifecycle" : "manifest denied native recorder returns 0x0B without object");
  if (library != nullptr) dlclose(library);
  if (system != nullptr) dlclose(system);
  r.text += r.failed == 0 ? "media4 opensl: all passed\n" : "media4 opensl: FAILED\n";
  return r.text;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_gesher_OpenSlTest_run(JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(Run(allowed == JNI_TRUE).c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_gesher_opensl_OpenSlActivity_run(JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(Run(allowed == JNI_TRUE).c_str());
}
