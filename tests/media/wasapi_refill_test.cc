// mmreg.h needs the GDI types disabled by the host build's default flags.
#undef NOGDI
#include "compat/media/audio/wasapi_refill.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {

using gesher::media::Mixer;
using gesher::media::RefillWasapiGroup;

struct Client {
  UINT32 queued = 0;

  HRESULT GetCurrentPadding(UINT32* padding) const {
    *padding = queued;
    return S_OK;
  }
};

struct Render {
  HRESULT get_result = S_OK;
  UINT32 frames = 0;
  DWORD flags = 0;
  std::array<float, 960> samples{};

  HRESULT GetBuffer(UINT32 count, BYTE** data) {
    if (FAILED(get_result)) return get_result;
    if (count * 2 > samples.size()) return E_INVALIDARG;
    *data = reinterpret_cast<BYTE*>(samples.data());
    return S_OK;
  }

  HRESULT ReleaseBuffer(UINT32 count, DWORD buffer_flags) {
    frames = count;
    flags = buffer_flags;
    return S_OK;
  }
};

class TestMixer final : public Mixer {
 public:
  int mixed_group = -1;
  bool has_audio = true;

  bool GroupWanted(int) override { return true; }

  bool MixGroup(int group, float* out, size_t frames) override {
    mixed_group = group;
    std::fill_n(out, frames * 2, has_audio ? 0.125f * (group + 1) : 0.0f);
    return has_audio;
  }
};

int failures = 0;

void Check(bool ok, const char* message) {
  if (ok) return;
  std::fprintf(stderr, "%s\n", message);
  ++failures;
}

}  // namespace

int main() {
  TestMixer mixer;
  Client media{480};
  Client calls{96};
  Render media_render;
  Render call_render;
  UINT32 padding = 0;
  Check(RefillWasapiGroup(media, media_render, mixer, 0, 480, &padding) == S_FALSE &&
            mixer.mixed_group == -1,
        "A full endpoint must leave the media tracks alone");
  Check(RefillWasapiGroup(calls, call_render, mixer, 4, 240, &padding) == S_OK &&
            padding == 96 && mixer.mixed_group == 4 && call_render.frames == 144 &&
            call_render.flags == 0 && call_render.samples[0] == 0.625f &&
            call_render.samples[287] == 0.625f && call_render.samples[288] == 0.0f,
        "The voice session receives exactly its missing frames and samples");
  media.queued = 352;
  Check(RefillWasapiGroup(media, media_render, mixer, 0, 480, &padding) == S_OK &&
            media_render.frames == 128 && media_render.samples[255] == 0.125f &&
            call_render.samples[0] == 0.625f,
        "A separate media refill must leave the voice session samples intact");
  calls.queued = 0;
  mixer.has_audio = false;
  Check(RefillWasapiGroup(calls, call_render, mixer, 4, 240, &padding) == S_OK &&
            call_render.frames == 240 && call_render.flags == AUDCLNT_BUFFERFLAGS_SILENT,
        "An empty voice mix supplies silence to the endpoint");
  call_render.get_result = AUDCLNT_E_DEVICE_INVALIDATED;
  mixer.mixed_group = -1;
  Check(RefillWasapiGroup(calls, call_render, mixer, 4, 240, &padding) == AUDCLNT_E_DEVICE_INVALIDATED &&
            mixer.mixed_group == -1,
        "Endpoint loss propagates without draining the track");
  if (failures) return 1;
  std::puts("WASAPI refill contracts passed");
  return 0;
}
