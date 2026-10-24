// Gesher: hwui's Vulkan pipeline is not built (there is no Vulkan on
// Gesher's Windows path; ANGLE gives OpenGL ES). CanvasContext names the
// class for RenderPipelineType::SkiaVulkan, which register_gesher_graphics
// never selects (compat/hwui/gl/pipeline_select.cc), so the name stands for
// the GL pipeline and CanvasContext compiles unchanged.
#pragma once

#include "pipeline/skia/SkiaOpenGLPipeline.h"

namespace android {
namespace uirenderer {
namespace skiapipeline {

class SkiaVulkanPipeline : public SkiaOpenGLPipeline {
public:
    explicit SkiaVulkanPipeline(renderthread::RenderThread& thread) : SkiaOpenGLPipeline(thread) {}

    static void invokeFunctor(const renderthread::RenderThread& thread, Functor* functor) {}
};

}  // namespace skiapipeline
}  // namespace uirenderer
}  // namespace android
