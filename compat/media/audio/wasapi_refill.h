// Device-paced refill shared by the WASAPI sink and its endpoint-free test.
#pragma once

#include <windows.h>
#include <audioclient.h>

#include "sink.h"

namespace gesher::media {

template <typename Client, typename Render>
HRESULT RefillWasapiGroup(Client& client, Render& render, Mixer& mixer, int group,
                          UINT32 target, UINT32* padding) {
  HRESULT hr = client.GetCurrentPadding(padding);
  if (FAILED(hr)) return hr;
  if (*padding >= target) return S_FALSE;

  const UINT32 frames = target - *padding;
  BYTE* data = nullptr;
  hr = render.GetBuffer(frames, &data);
  if (FAILED(hr)) return hr;
  const bool has_audio = mixer.MixGroup(group, reinterpret_cast<float*>(data), frames);
  hr = render.ReleaseBuffer(frames, has_audio ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
  return FAILED(hr) ? hr : S_OK;
}

}  // namespace gesher::media
