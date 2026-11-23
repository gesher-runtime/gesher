// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
#include "brokered_audio.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <gesher/binder_bootstrap.h>
#include <gesher/binder_endpoint.h>

#include "engine.h"
#include "shared_state.h"

namespace gesher::media {
namespace {

constexpr uint32_t kMaxRenderChannels = 8;  // WasapiSink caps its device format
constexpr uint32_t kMaxRenderFrames = 8192;
constexpr uint32_t kAllowedGroups = (1u << kNumGroups) - 1;
constexpr ULONGLONG kResponseMs = 100;

struct EndpointInfo {
  uint32_t available = 0;
  uint32_t rate = 0;
  uint32_t channels = 0;
  uint32_t latency_frames = 0;
  int32_t device_type = 0;
  char device_name[256]{};
  char description[512]{};
};

enum class Action : uint32_t { kGroups = 1, kMix };
struct Command {
  Action action;
  uint32_t group;
  uint32_t frames;
};

template <typename T>
bool Transfer(HANDLE pipe, T& value, bool write, ULONGLONG deadline) {
  return binder_bootstrap::Transfer(pipe, &value, sizeof(value), write, deadline);
}

template <size_t Size>
void CopyText(char (&destination)[Size], const std::string& source) {
  std::memcpy(destination, source.data(), std::min(Size - 1, source.size()));
}

bool AwaitInfo(HANDLE pipe, EndpointInfo* info) {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  uint32_t read = 1;
  return Transfer(pipe, *info, false, deadline) && Transfer(pipe, read, true, deadline);
}

bool SendInfo(HANDLE pipe, EndpointInfo& info) {
  const ULONGLONG deadline = GetTickCount64() + 5000;
  uint32_t read = 0;
  return Transfer(pipe, info, true, deadline) && Transfer(pipe, read, false, deadline) && read == 1;
}

class GuestSink final : public Sink {
 public:
  explicit GuestSink(HANDLE pipe) : pipe_(pipe) {}
  ~GuestSink() override { CloseHandle(pipe_); }

  bool ReadInfo(std::string* error) {
    if (!AwaitInfo(pipe_, &info_)) {
      *error = "WASAPI broker: the authenticated endpoint is unavailable";
      return false;
    }
    info_.device_name[sizeof(info_.device_name) - 1] = '\0';
    info_.description[sizeof(info_.description) - 1] = '\0';
    if (!info_.available || !info_.rate || !info_.channels || info_.channels > kMaxRenderChannels) {
      *error = info_.description;
      return false;
    }
    samples_.resize(static_cast<size_t>(kMaxRenderFrames) * info_.channels);
    return true;
  }

  uint32_t rate() const override { return info_.rate; }
  uint32_t channels() const override { return info_.channels; }
  uint32_t latency_frames() const override { return info_.latency_frames; }
  std::string DeviceName() const override { return info_.device_name; }
  int DeviceType() const override { return info_.device_type; }
  std::string Describe() const override { return info_.description; }
  void WaitForRefill(void*) override {}  // the actual endpoint requests each refill

  bool Tick(Mixer* mixer) override {
    Command command{};
    if (!Transfer(pipe_, command, false, GetTickCount64() + 5000)) return false;
    const ULONGLONG deadline = GetTickCount64() + kResponseMs;
    if (command.action == Action::kGroups) {
      uint32_t groups = 0;
      for (int group = 0; group < kNumGroups; ++group) {
        if (mixer->GroupWanted(group)) groups |= 1u << group;
      }
      return Transfer(pipe_, groups, true, deadline);
    }
    if (command.action != Action::kMix || command.group >= kNumGroups ||
        !command.frames || command.frames > kMaxRenderFrames) return false;
    uint32_t played = mixer->MixGroup(command.group, samples_.data(), command.frames);
    return Transfer(pipe_, played, true, deadline) &&
           (!played || binder_bootstrap::Transfer(pipe_, samples_.data(),
               command.frames * info_.channels * sizeof(float), true, deadline));
  }

 private:
  HANDLE pipe_;
  EndpointInfo info_{};
  std::vector<float> samples_;
};

class HostMixer final : public Mixer {
 public:
  HostMixer(HANDLE pipe, uint32_t channels) : pipe_(pipe), channels_(channels) {}

  bool RequestGroups() {
    Command command{Action::kGroups, 0, 0};
    const ULONGLONG deadline = GetTickCount64() + kResponseMs;
    return Transfer(pipe_, command, true, deadline) &&
           Transfer(pipe_, groups_, false, deadline) && (groups_ & ~kAllowedGroups) == 0;
  }

  bool GroupWanted(int group) override { return (groups_ & (1u << group)) != 0; }

  bool MixGroup(int group, float* out, size_t frames) override {
    const size_t samples = frames * channels_;
    std::fill_n(out, samples, 0.0f);
    if (failed_ || frames == 0 || frames > kMaxRenderFrames) return false;
    Command command{Action::kMix, static_cast<uint32_t>(group), static_cast<uint32_t>(frames)};
    uint32_t played = 0;
    const ULONGLONG deadline = GetTickCount64() + kResponseMs;
    if (!Transfer(pipe_, command, true, deadline) ||
        !Transfer(pipe_, played, false, deadline) || played > 1 ||
        (played && !binder_bootstrap::Transfer(pipe_, out, samples * sizeof(float), false, deadline))) {
      failed_ = true;
      std::fill_n(out, samples, 0.0f);
      return false;
    }
    // PCM comes from the application. Keep a bad sample from leaving the
    // WASAPI buffer with invalid floating point or out-of-range values.
    for (size_t i = 0; i < samples; ++i) {
      out[i] = std::isfinite(out[i]) ? std::clamp(out[i], -1.0f, 1.0f) : 0.0f;
    }
    return played != 0;
  }

  bool Failed() const { return failed_; }

 private:
  HANDLE pipe_;
  uint32_t channels_;
  uint32_t groups_ = 0;
  bool failed_ = false;
};

}  // namespace

std::unique_ptr<Sink> OpenBrokeredAudio(const std::u16string& label, std::string* error) {
  HANDLE pipe = binder_endpoint::OpenAudioRender(label);
  if (pipe == INVALID_HANDLE_VALUE) {
    *error = "WASAPI broker: cannot open the authenticated render lane";
    return nullptr;
  }
  auto sink = std::make_unique<GuestSink>(pipe);
  if (!sink->ReadInfo(error)) return nullptr;
  return sink;
}

void ServeBrokeredAudio(HANDLE pipe, const std::u16string& label, uint32_t owner,
                       const std::function<bool()>& is_live) {
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return;
  struct ComScope {
    ~ComScope() { CoUninitialize(); }
  } com;
  std::string error;
  auto sink = OpenWasapiSink(label, &error, owner);
  EndpointInfo info{};
  if (sink) {
    info.available = 1;
    info.rate = sink->rate();
    info.channels = sink->channels();
    info.latency_frames = sink->latency_frames();
    info.device_type = sink->DeviceType();
    CopyText(info.device_name, sink->DeviceName());
    CopyText(info.description, sink->Describe());
  } else {
    CopyText(info.description, error);
  }
  if (!SendInfo(pipe, info) || !sink) return;
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  if (!timer) return;
  LARGE_INTEGER due{};
  due.QuadPart = -100000;  // 10 ms, matching the existing audio-engine timer
  if (SetWaitableTimer(timer, &due, 10, nullptr, nullptr, FALSE)) {
    HostMixer mixer(pipe, info.channels);
    uint32_t generation = Generation() - 1;
    while (is_live()) {
      sink->WaitForRefill(timer);
      const uint32_t next = Generation();
      if (generation != next) {
        generation = next;
        sink->OnVolumesChanged();
      }
      if (!mixer.RequestGroups() || !sink->Tick(&mixer) || mixer.Failed()) break;
    }
  }
  CloseHandle(timer);
}

namespace {

constexpr size_t kCaptureSamples = 1u << 20;
enum class CaptureAction : uint32_t { kStart = 1, kStop, kPull };
struct CaptureReply {
  uint32_t okay = 0;
  uint32_t samples = 0;
};

class GuestCapture final : public CaptureSource {
 public:
  GuestCapture(HANDLE pipe, EndpointInfo info) : pipe_(pipe), info_(info) {}
  ~GuestCapture() override { CloseHandle(pipe_); }
  uint32_t rate() const override { return info_.rate; }
  uint32_t channels() const override { return info_.channels; }
  std::string Describe() const override { return info_.description; }
  std::string DeviceName() const override { return info_.device_name; }

  bool Start() override { return Exchange(CaptureAction::kStart); }
  void Stop() override { Exchange(CaptureAction::kStop); }

  bool Pull(std::vector<float>* out) override {
    const ULONGLONG deadline = GetTickCount64() + 1000;
    CaptureAction action = CaptureAction::kPull;
    CaptureReply reply;
    if (!Transfer(pipe_, action, true, deadline) || !Transfer(pipe_, reply, false, deadline) ||
        !reply.okay || reply.samples > kCaptureSamples || reply.samples % info_.channels != 0) return false;
    const size_t start = out->size();
    out->resize(start + reply.samples);
    if (reply.samples == 0 || binder_bootstrap::Transfer(
            pipe_, out->data() + start, reply.samples * sizeof(float), false, deadline)) return true;
    out->resize(start);
    return false;
  }

 private:
  bool Exchange(CaptureAction action) {
    const ULONGLONG deadline = GetTickCount64() + 1000;
    CaptureReply reply;
    return Transfer(pipe_, action, true, deadline) && Transfer(pipe_, reply, false, deadline) &&
           reply.okay == 1 && reply.samples == 0;
  }

  HANDLE pipe_;
  EndpointInfo info_;
};

}  // namespace

std::unique_ptr<CaptureSource> OpenBrokeredCapture(std::string* error) {
  HANDLE pipe = binder_endpoint::OpenAudioCapture(false);
  if (pipe == INVALID_HANDLE_VALUE) {
    *error = "WASAPI capture broker: the authenticated endpoint is unavailable";
    return nullptr;
  }
  EndpointInfo info;
  const bool received = AwaitInfo(pipe, &info);
  info.description[sizeof(info.description) - 1] = '\0';
  info.device_name[sizeof(info.device_name) - 1] = '\0';
  if (!received || !info.available || !info.rate || !info.channels) {
    *error = received ? info.description : "WASAPI capture broker disconnected";
    CloseHandle(pipe);
    return nullptr;
  }
  return std::make_unique<GuestCapture>(pipe, info);
}

bool HasBrokeredCaptureEndpoint(std::string* name) {
  HANDLE pipe = binder_endpoint::OpenAudioCapture(true);
  if (pipe == INVALID_HANDLE_VALUE) return false;
  EndpointInfo info;
  const bool received = AwaitInfo(pipe, &info);
  CloseHandle(pipe);
  info.device_name[sizeof(info.device_name) - 1] = '\0';
  if (!received || !info.available) return false;
  *name = info.device_name;
  return true;
}

void ServeBrokeredCapture(HANDLE pipe, bool info_only, const std::function<bool()>& is_live,
                          const std::function<bool()>& allowed) {
  DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
  if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) return;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
  struct ComScope {
    ~ComScope() { CoUninitialize(); }
  } com;
  EndpointInfo info;
  std::string name;
  if (info_only) {
    if (is_live() && HasWasapiCaptureEndpoint(&name)) {
      info.available = 1;
      CopyText(info.device_name, name);
    }
    SendInfo(pipe, info);
    return;
  }
  std::string error;
  auto source = allowed() ? OpenWasapiCapture(false, &error) : nullptr;
  if (source) {
    info.available = 1;
    info.rate = source->rate();
    info.channels = source->channels();
    CopyText(info.device_name, source->DeviceName());
    CopyText(info.description, source->Describe());
  } else {
    CopyText(info.description, error.empty() ? "RECORD_AUDIO permission denied" : error);
  }
  if (!SendInfo(pipe, info) || !source) return;

  bool capturing = false;
  ULONGLONG last_request = GetTickCount64();
  std::vector<float> samples;
  while (is_live()) {
    DWORD bytes = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes, nullptr)) break;
    if (bytes < sizeof(CaptureAction)) {
      // A client that stops polling cannot leave the microphone running.
      if (capturing && GetTickCount64() - last_request > 1000) break;
      Sleep(2);
      continue;
    }
    last_request = GetTickCount64();
    const ULONGLONG deadline = last_request + 1000;
    CaptureAction action{};
    if (!Transfer(pipe, action, false, deadline)) break;
    CaptureReply reply;
    samples.clear();
    switch (action) {
      case CaptureAction::kStart:
        capturing = allowed() && (capturing || source->Start());
        reply.okay = capturing;
        break;
      case CaptureAction::kStop:
        if (capturing) source->Stop();
        capturing = false;
        reply.okay = 1;
        break;
      case CaptureAction::kPull:
        if (capturing && allowed() && source->Pull(&samples) &&
            samples.size() <= kCaptureSamples && allowed()) {
          reply.okay = 1;
          reply.samples = static_cast<uint32_t>(samples.size());
        }
        break;
      default:
        break;
    }
    if (!Transfer(pipe, reply, true, deadline) ||
        (reply.samples && !binder_bootstrap::Transfer(pipe, samples.data(),
             reply.samples * sizeof(float), true, deadline))) break;
    if (!reply.okay) break;
  }
  source->Stop();
}

}  // namespace gesher::media
