// Gesher: which of hwui's pipelines renders (pipeline_select.cc; ADR 0030).
#pragma once

#include "Properties.h"

// The pipeline for this process, chosen once (register_gesher_graphics):
// SkiaCpu by default; explicit auto qualifies ANGLE D3D11 hardware, while
// software/failed probes keep SkiaCpu. Physical default-path acceptance is pending.
// GESHER_HWUI_RENDERER or debug.hwui.renderer: auto, skiacpu, skiagl.
android::uirenderer::RenderPipelineType gesher_hwui_choose_pipeline();

// "skiagl" or "skiacpu", after gesher_hwui_choose_pipeline.
const char* gesher_hwui_renderer_name();

// What asked for the pipeline and what was found ("debug.hwui.renderer=skiagl:
// <GL_RENDERER>, <GL_VERSION>, EGL 1.5"; for a fallback to skiacpu, why).
const char* gesher_hwui_renderer_detail();
