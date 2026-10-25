// Gesher media: where the engine's mixes go (engine.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace gesher::media {

// The engine, as a sink sees it.
class Mixer {
 public:
  virtual ~Mixer() = default;
  // Mixes the group's tracks into `out` (frames x the sink's channels, at its
  // rate), zeroed first, with the master volume applied (not the group's
  // stream volume). Returns false, and silence, when no track of the group
  // played.
  virtual bool MixGroup(int group, float* out, size_t frames) = 0;
  // A track of the group is playing (or was within the last second).
  virtual bool GroupWanted(int group) = 0;
};

class Sink {
 public:
  virtual ~Sink() = default;
  virtual uint32_t rate() const = 0;
  virtual uint32_t channels() const = 0;
  // Frames the sink holds before they are heard.
  virtual uint32_t latency_frames() const = 0;
  virtual std::string Describe() const = 0;
  // The output device, as AudioManager.getDevices shows it: its name and its
  // Android type (AUDIO_DEVICE_OUT_SPEAKER, ..._WIRED_HEADPHONE, ...).
  virtual std::string DeviceName() const { return "Gesher audio output"; }
  virtual int DeviceType() const { return 0x2; }
  // One period: pull what the output needs now. Returns false when the
  // output is gone (the device was removed); the engine then opens another.
  virtual bool Tick(Mixer* mixer) = 0;
  // The shared stream volumes changed (shared_state.h).
  virtual void OnVolumesChanged() {}
};

// WASAPI shared mode on the default render endpoint; null with `error` set
// when there is none or it fails.
std::unique_ptr<Sink> OpenWasapiSink(const std::u16string& app_name, std::string* error);
// The mixed output, paced by the clock, into a 16-bit stereo 48 kHz WAV file.
std::unique_ptr<Sink> OpenCaptureSink(const std::wstring& path, std::string* error);
// Nothing heard, paced by the clock.
std::unique_ptr<Sink> OpenNullSink(const std::string& reason);

}  // namespace gesher::media
