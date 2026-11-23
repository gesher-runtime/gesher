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

struct RenderInfo {
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

class GuestSink final : public Sink {
 public:
  explicit GuestSink(HANDLE pipe) : pipe_(pipe) {}
  ~GuestSink() override { CloseHandle(pipe_); }

  bool ReadInfo(std::string* error) {
    if (!Transfer(pipe_, info_, false, GetTickCount64() + 5000)) {
      *error = "WASAPI broker: the authenticated endpoint is unavailable";
      return false;
    }
    info_.device_name[sizeof(info_.device_name) - 1] = '\0';
    info_.description[sizeof(info_.description) - 1] = '\0';
    if (!info_.rate || !info_.channels || info_.channels > kMaxRenderChannels) {
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
  RenderInfo info_{};
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
  RenderInfo info{};
  if (sink) {
    info.rate = sink->rate();
    info.channels = sink->channels();
    info.latency_frames = sink->latency_frames();
    info.device_type = sink->DeviceType();
    CopyText(info.device_name, sink->DeviceName());
    CopyText(info.description, sink->Describe());
  } else {
    CopyText(info.description, error);
  }
  if (!Transfer(pipe, info, true, GetTickCount64() + 5000) || !sink) return;
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

}  // namespace gesher::media
