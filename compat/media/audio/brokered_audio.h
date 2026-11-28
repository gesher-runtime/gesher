// Copyright (C) 2026 The Gesher Authors
 // SPDX-License-Identifier: Apache-2.0
#pragma once

#include "capture.h"
#include "sink.h"

#include <functional>
#include <windows.h>

namespace gesher::media {

constexpr uint32_t kAudioRenderLabelChars = 120;
std::unique_ptr<Sink> OpenBrokeredAudio(const std::u16string& label, std::string* error);
void ServeBrokeredAudio(HANDLE pipe, const std::u16string& label, uint32_t owner,
                       const std::function<bool()>& is_live);

std::unique_ptr<CaptureSource> OpenBrokeredCapture(std::string* error);
bool HasBrokeredCaptureEndpoint(std::string* name);
void ServeBrokeredCapture(HANDLE pipe, bool info_only, const std::function<bool()>& is_live,
                          const std::function<bool()>& allowed);

}  // namespace gesher::media
