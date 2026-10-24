// Gesher: which of hwui's pipelines renders (pipeline_select.cc; ADR 0030).
#pragma once

#include "Properties.h"

// The pipeline for this process, chosen once (register_gesher_graphics):
// SkiaCpu unless GESHER_HWUI_RENDERER or the system property
// debug.hwui.renderer is "skiagl" and OpenGL ES over ANGLE works.
android::uirenderer::RenderPipelineType gesher_hwui_choose_pipeline();

// "skiagl" or "skiacpu", after gesher_hwui_choose_pipeline.
const char* gesher_hwui_renderer_name();

// What was found: GL_RENDERER and versions for skiagl, why not for a
// fallback to skiacpu.
const char* gesher_hwui_renderer_detail();
