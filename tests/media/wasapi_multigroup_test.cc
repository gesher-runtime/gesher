// compat/media/audio/wasapi_sink.cc: verify multi-group mixing default (-1),
// STREAM_VOICE_CALL (group 4 "Calls") active and muted draining, and StreamGain
// software volume application with fake Mixer and real/isolated SharedAudioState.
//
//   wasapi_multigroup_test.exe -> "wasapi_multigroup_test all passed" (exit 0)

#undef NOGDI
#undef WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "compat/media/audio/engine.h"
#include "compat/media/audio/shared_state.h"
#include "compat/media/audio/sink.h"
#include "compat/media/audio/wasapi_sink.h"

namespace gesher::media {
namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

class RecordingMixer final : public Mixer {
 public:
  bool wanted[kNumGroups] = {};
  bool has_audio[kNumGroups] = {};
  float fill_value[kNumGroups] = {};
  int wanted_calls[kNumGroups] = {};
  int mix_calls[kNumGroups] = {};

  void ResetCounts() {
    for (int g = 0; g < kNumGroups; ++g) {
      wanted_calls[g] = 0;
      mix_calls[g] = 0;
    }
  }

  bool GroupWanted(int group) override {
    if (group >= 0 && group < kNumGroups) {
      ++wanted_calls[group];
      return wanted[group];
    }
    return false;
  }

  bool MixGroup(int group, float* out, size_t frames) override {
    if (group < 0 || group >= kNumGroups) return false;
    ++mix_calls[group];
    const size_t n = frames * 2;
    const float v = has_audio[group] ? fill_value[group] : 0.0f;
    for (size_t i = 0; i < n; ++i) out[i] = v;
    return has_audio[group];
  }
};

}  // namespace
}  // namespace gesher::media

int main() {
  using namespace gesher::media;

  _putenv("ANDROID_DATA=gesher-wasapi-multigroup-test");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  InitStreamVolume(3, 0, 15);  // STREAM_MUSIC -> group 0 ("Media")
  InitStreamVolume(0, 1, 5);   // STREAM_VOICE_CALL -> group 4 ("Calls")
  SetStreamVolume(3, 15, false);
  SetStreamVolume(0, 3, false);
  SetMasterVolume(1.0f);
  SetMasterMute(false);

  WasapiSinkParams defaults;
  Check(defaults.stream_group == -1, "WasapiSinkParams defaults stream_group to -1 (all groups)");
  Check(GroupOfStream(3) == 0 && StreamOfGroup(0) == 3, "Group 0 maps STREAM_MUSIC");
  Check(GroupOfStream(0) == 4 && StreamOfGroup(4) == 0, "Group 4 maps STREAM_VOICE_CALL");

  std::string err;
  std::unique_ptr<Sink> sink = OpenLowLatencyWasapiSink(u"WasapiMultigroupTest", &err);
  if (sink == nullptr) {
    std::printf("INFO  no WASAPI endpoint on host (%s); skipping live endpoint checks\n",
                err.c_str());
  } else {
    RecordingMixer mixer;
    mixer.wanted[4] = true;
    mixer.has_audio[4] = true;
    mixer.fill_value[4] = 0.25f;

    Check(sink->Tick(&mixer), "Tick succeeds with only group 4 (Calls) wanted");
    for (int g = 0; g < kNumGroups; ++g) {
      Check(mixer.wanted_calls[g] > 0, "Multi-group Tick checks GroupWanted across all groups");
    }
    Check(mixer.mix_calls[4] == 1, "Group 4 (Calls) is drained by MixGroup when wanted");
    Check(mixer.mix_calls[0] == 0, "Unwanted Group 0 (Media) is not mixed");

    mixer.ResetCounts();
    SetStreamVolume(0, 3, true);
    sink->OnVolumesChanged();
    Check(StreamGain(0) == 0.0f, "Muted STREAM_VOICE_CALL has zero StreamGain");
    Check(sink->Tick(&mixer), "Tick succeeds while group 4 is muted");
    Check(mixer.mix_calls[4] == 1,
          "Muted group 4 is still drained so AudioTrack writers do not block");

    mixer.ResetCounts();
    SetStreamVolume(0, 5, false);
    sink->OnVolumesChanged();
    mixer.wanted[0] = true;
    mixer.has_audio[0] = true;
    mixer.fill_value[0] = 0.5f;
    Check(sink->Tick(&mixer), "Tick mixes simultaneous group 0 and group 4");
    Check(mixer.mix_calls[0] == 1 && mixer.mix_calls[4] == 1,
          "Both group 0 and group 4 mixed on simultaneous activity");

    sink.reset();
  }

  CoUninitialize();
  if (g_failures == 0) {
    std::printf("wasapi_multigroup_test all passed\n");
    return 0;
  }
  return 1;
}
