// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Windows' closed-caption settings (Settings > Accessibility > Captions > Caption style):
// Windows.Media.ClosedCaptioning.ClosedCaptionProperties, for
// com.gesher.server.WindowsCaptions (CaptioningSettings maps them to the
// Settings.Secure keys android.view.accessibility.CaptioningManager reads).
#pragma once

#include <windows.h>
#include <stdint.h>

namespace gesher::devices::captions {

// Windows' own enum values (ClosedCaptionColor, ClosedCaptionOpacity, ClosedCaptionSize,
// ClosedCaptionStyle, ClosedCaptionEdgeEffect; 0 is Default in each) and the computed
// colors as 0xAARRGGBB (Windows.UI.Color). A computed color is unused and zero
// when its color and opacity are both Default (the player chooses both).
struct Properties {
  int32_t font_color;
  uint32_t computed_font_color;
  int32_t font_opacity;
  int32_t font_size;
  int32_t font_style;
  int32_t font_effect;
  int32_t background_color;
  uint32_t computed_background_color;
  int32_t background_opacity;
  int32_t region_color;
  uint32_t computed_region_color;
  int32_t region_opacity;
};

constexpr int kValues = 12;  // Properties as an int array, in declaration order

// Reads every enum and each computed color whose color or opacity is specified.
// E_NOTIMPL without combase's WinRT functions; another failure is the first
// required getter's HRESULT (nothing is reported then).
HRESULT Read(Properties* out);

// PropertiesChanged (IClosedCaptionPropertiesStatics2): after StartWatch, each change
// Windows reports increments a counter and wakes AwaitChange. E_NOINTERFACE on Windows
// versions without the event. Not reference counted: one watch per process.
HRESULT StartWatch();
// Waits up to timeout_ms for a change after `seen`; returns the current counter (equal to
// `seen` on time-out), or -1 when no watch is running.
int64_t AwaitChange(int64_t seen, uint32_t timeout_ms);
void StopWatch();

}  // namespace gesher::devices::captions
