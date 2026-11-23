// Copyright (C) 2026 The Gesher Authors
 // SPDX-License-Identifier: Apache-2.0
#pragma once

#include "sink.h"

#include <functional>
#include <windows.h>

namespace gesher::media {

constexpr uint32_t kAudioRenderLabelChars = 120;
std::unique_ptr<Sink> OpenBrokeredAudio(const std::u16string& label, std::string* error);
void ServeBrokeredAudio(HANDLE pipe, const std::u16string& label, uint32_t owner,
                       const std::function<bool()>& is_live);

}  // namespace gesher::media
