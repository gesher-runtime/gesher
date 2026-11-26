// OpenSL C contracts through the actual app NDK ABI (media4, ADR 0064).
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <dlfcn.h>
#include <jni.h>

#include <cstdint>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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

struct CallbackState {
  std::atomic<int> calls{0};
  std::atomic<bool> valid{true};
};
constexpr uint64_t kCookie = UINT64_C(0x1122334455667788);
thread_local uint64_t g_callback_cookie = kCookie;

void BufferDone(SLAndroidSimpleBufferQueueItf, void* user) {
  auto* state = static_cast<CallbackState*>(user);
  if (g_callback_cookie != kCookie) state->valid = false;
  ++g_callback_cookie;
  state->calls.fetch_add(1, std::memory_order_release);
}

void CheckVolumeEffect(Report& r, SLObjectItf player, SLAndroidEffectItf effect) {
  constexpr SLInterfaceID_ volume = {
      0x119341a0, 0x8469, 0x11df, 0x81f9, {0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b}};
  constexpr SLInterfaceID_ unavailable = {};
  const auto& api = **effect;
  SLboolean enabled = SL_BOOLEAN_TRUE;
  r.Check(api.CreateEffect(effect, &unavailable) == SL_RESULT_RESOURCE_ERROR &&
              api.SetEnabled(effect, &volume, SL_BOOLEAN_TRUE) == SL_RESULT_PARAMETER_INVALID,
          "unknown and uncreated Android effects fail");
  r.Check(api.CreateEffect(effect, &volume) == SL_RESULT_SUCCESS &&
              api.IsEnabled(effect, &volume, &enabled) == SL_RESULT_SUCCESS && !enabled,
          "create disabled Android volume effect");
  r.Check(api.SetEnabled(effect, &volume, SL_BOOLEAN_TRUE) == SL_RESULT_SUCCESS &&
              api.CreateEffect(effect, &volume) == SL_RESULT_SUCCESS &&
              api.IsEnabled(effect, &volume, &enabled) == SL_RESULT_SUCCESS && enabled,
          "enable effect and preserve state on duplicate creation");
  struct LevelParam {
    int32_t status;
    uint32_t parameter_size;
    uint32_t value_size;
    uint32_t parameter;
    int16_t value;
  } request{0, 4, 2, 0, -637}, reply{};
  constexpr SLuint32 kSetParam = 5, kGetParam = 8;
  constexpr SLuint32 kParamHeader = offsetof(LevelParam, value);
  SLuint32 size = sizeof(int32_t);
  int32_t status = -1;
  r.Check(api.SendCommand(effect, &volume, kSetParam, kParamHeader + 2, &request, &size, &status) ==
              SL_RESULT_SUCCESS && size == sizeof(status) && status == 0,
          "set Android volume effect level");
  size = sizeof(reply);
  r.Check(api.SendCommand(effect, &volume, kGetParam, kParamHeader, &request, &size, &reply) ==
              SL_RESULT_SUCCESS && size == kParamHeader + 2 && reply.status == 0 && reply.value == -600,
          "get Android volume effect level at bundle precision");
  request.value_size = UINT32_MAX;
  r.Check(api.SendCommand(effect, &volume, kSetParam, kParamHeader + 2, &request, &size, &reply) ==
              SL_RESULT_PARAMETER_INVALID, "reject malformed Android effect command");
  struct BooleanParam {
    int32_t status;
    uint32_t parameter_size;
    uint32_t value_size;
    uint32_t parameter;
    uint32_t value;
    uint32_t guard;
  };
  static_assert(offsetof(BooleanParam, value) == kParamHeader);
  constexpr uint32_t kGuard = 0x12345678;
  for (uint32_t parameter : {2u, 3u}) {
    BooleanParam boolean_request{0, 4, 4, parameter, 1, 0};
    status = -1;
    size = sizeof(status);
    r.Check(api.SendCommand(effect, &volume, kSetParam, kParamHeader + 4, &boolean_request, &size, &status) ==
                SL_RESULT_SUCCESS && size == sizeof(status) && status == 0,
            "set Android volume effect boolean parameter");
    BooleanParam boolean_reply{0, 0, 0, 0, 0, kGuard};
    boolean_request.value_size = 8;
    size = sizeof(boolean_reply);
    r.Check(api.SendCommand(effect, &volume, kGetParam, kParamHeader, &boolean_request, &size,
                            &boolean_reply) == SL_RESULT_SUCCESS &&
                size == kParamHeader + 4 && boolean_reply.status == 0 && boolean_reply.value_size == 4 &&
                boolean_reply.value == 1 && boolean_reply.guard == kGuard,
            "get Android volume effect boolean with larger reply capacity");
  }
  SLVolumeItf ordinary = nullptr;
  SLmillibel level = 1;
  r.Check((*player)->GetInterface(player, SL_IID_VOLUME, &ordinary) == SL_RESULT_SUCCESS &&
              ordinary != nullptr && (*ordinary)->GetVolumeLevel(ordinary, &level) == SL_RESULT_SUCCESS &&
              level == 0, "effect gain is separate from ordinary player volume");
  r.Check(api.ReleaseEffect(effect, &volume) == SL_RESULT_SUCCESS &&
              api.IsEnabled(effect, &volume, &enabled) == SL_RESULT_PARAMETER_INVALID,
          "release Android volume effect");
}

std::string Run(bool allowed, const char* caller, bool has_manifest) {
  Report r;
#if defined(__aarch64__)
  r.text = "media4 opensl ABI: arm64-v8a\n";
#else
  r.text = "media4 opensl ABI: x86_64\n";
#endif
  r.text += "media4 opensl caller: " + std::string(caller) + "\n";
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
  const SLuint32 counts[] = {2, 1, 9, 5};
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
      SLresult set = (*volume)->SetVolumeLevel(volume, -600);
      SLresult get = (*volume)->GetVolumeLevel(volume, &level);
      r.text += "media4 opensl volume trace: requested -600, set " + std::to_string(set) +
          ", get " + std::to_string(get) + ", value " + std::to_string(level) + "\n";
      r.Check(set == SL_RESULT_SUCCESS && get == SL_RESULT_SUCCESS && level == -600,
              "actual PCM volume round trip");
      const SLmillibel edges[] = {SL_MILLIBEL_MIN, -1, 0};
      for (SLmillibel edge : edges) {
        set = (*volume)->SetVolumeLevel(volume, edge);
        get = (*volume)->GetVolumeLevel(volume, &level);
        r.text += "media4 opensl volume trace: requested " + std::to_string(edge) +
            ", set " + std::to_string(set) + ", get " + std::to_string(get) +
            ", value " + std::to_string(level) + "\n";
        r.Check(set == SL_RESULT_SUCCESS && get == SL_RESULT_SUCCESS && level == edge,
                "signed16 volume boundary " + std::to_string(edge));
      }
      int16_t silence[882] = {};
      SLAndroidSimpleBufferQueueState state{};
      r.Check((*buffers)->Enqueue(buffers, silence, sizeof(silence)) == SL_RESULT_SUCCESS &&
                  (*buffers)->GetState(buffers, &state) == SL_RESULT_SUCCESS && state.count == 1 &&
                  (*buffers)->Clear(buffers) == SL_RESULT_SUCCESS &&
                  (*buffers)->GetState(buffers, &state) == SL_RESULT_SUCCESS && state.count == 0,
              "real PCM queue accepts and clears caller buffer");
      CallbackState callback;
      r.Check((*buffers)->RegisterCallback(buffers, BufferDone, &callback) == SL_RESULT_SUCCESS &&
                  (*buffers)->Enqueue(buffers, silence, sizeof(silence)) == SL_RESULT_SUCCESS &&
                  (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS,
              "start real native PCM callback");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (callback.calls.load(std::memory_order_acquire) == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      r.Check((*play)->SetPlayState(play, SL_PLAYSTATE_STOPPED) == SL_RESULT_SUCCESS &&
                  callback.calls.load(std::memory_order_acquire) == 1 && callback.valid &&
                  g_callback_cookie == kCookie,
              "actual callback thread has independent initialized ELF TLS");
      r.text += "media4 opensl callback trace: calls " + std::to_string(callback.calls.load()) +
          ", TLS valid " + (callback.valid ? "true" : "false") + ", caller TLS unchanged " +
          (g_callback_cookie == kCookie ? "true\n" : "false\n");
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
  const SLInterfaceID effect_ids[] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_ANDROIDEFFECT,
                                     SL_IID_ANDROIDCONFIGURATION};
  const SLboolean effect_required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
  Object effect_player;
  SLAndroidEffectItf effect = nullptr;
  r.Check((*api)->CreateAudioPlayer(api, &effect_player.value, &source, &sink, 4, effect_ids,
                                  effect_required) == SL_RESULT_SUCCESS && effect_player.value != nullptr,
          "create PCM player with required Android effect interface");
  if (effect_player.value != nullptr) {
    const auto p = effect_player.value;
    r.Check((*p)->GetInterface(p, SL_IID_ANDROIDEFFECT, &effect) == SL_RESULT_PRECONDITIONS_VIOLATED &&
                effect == nullptr, "effect unavailable before realization");
    r.Check((*p)->Realize(p, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS &&
                (*p)->GetInterface(p, SL_IID_ANDROIDEFFECT, &effect) == SL_RESULT_SUCCESS &&
                effect != nullptr, "retrieve Android effect on realized player");
    if (effect != nullptr) CheckVolumeEffect(r, p, effect);
  }
  r.Part("effect", before, "required Android effect interface and per-player volume lifecycle");

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
  r.text += "media4 opensl permission trace: expected " + std::string(allowed ? "allowed" : "denied") +
      ", result " + std::to_string(result) + ", object " + (recorder.value != nullptr ? "true\n" : "false\n");
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
  r.Part("permission", before, has_manifest ?
      (allowed ? "manifest allowed native recorder lifecycle" : "manifest denied native recorder returns 0x0B without object") :
      std::strcmp(caller, "executable") == 0 ? "native process without recording identity returns 0x0B without object" :
      "bare core without recording identity returns 0x0B without object");
  if (library != nullptr) dlclose(library);
  if (system != nullptr) dlclose(system);
  r.text += r.failed == 0 ? "media4 opensl: all passed\n" : "media4 opensl: FAILED\n";
  return r.text;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_gesher_OpenSlTest_run(JNIEnv* env, jclass, jboolean) {
  return env->NewStringUTF(Run(false, "JNI", false).c_str());
}
extern "C" JNIEXPORT jstring JNICALL Java_com_gesher_opensl_OpenSlActivity_run(JNIEnv* env, jclass, jboolean allowed) {
  return env->NewStringUTF(Run(allowed == JNI_TRUE, "JNI", true).c_str());
}

#ifdef MEDIA_OPENSL_EXECUTABLE
int main() {
  // No Java/manifest permission provider is installed in a native process.
  const std::string report = Run(false, "executable", false);
  std::fputs(report.c_str(), stdout);
  return report.find("media4 opensl: all passed\n") == std::string::npos ? 1 : 0;
}
#endif
