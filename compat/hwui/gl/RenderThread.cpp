/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Gesher: hwui's RenderThread for Windows (docs/angle.md, ADR 0030).
//
// frameworks/base/libs/hwui/platform/host/renderthread/RenderThread.cpp (the
// host's: no vsync source, no GPU) with the OpenGL half of the device's
// renderthread/RenderThread.cpp: an EglManager, and a GrDirectContext over
// ANGLE (GrGLMakeNativeInterface, compat/hwui/gl/GrGLMakeNativeInterface_angle.cc)
// for the SkiaGL pipeline, made on first use by requireGlContext. The device's
// CacheManager GPU budget (CacheManager::configureContext/reset) is not
// there on the host: Ganesh keeps its default resource cache limit.
// Vulkan is not built.

#include "renderthread/RenderThread.h"

#include <android-base/properties.h>
#include <include/gpu/ganesh/GrContextOptions.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>

#include "DeviceInfo.h"
#include "Properties.h"
#include "Readback.h"
#include "renderstate/RenderState.h"
#include "renderthread/EglManager.h"
#include "renderthread/VulkanManager.h"

namespace android {
namespace uirenderer {
namespace renderthread {

static bool gHasRenderThreadInstance = false;
static JVMAttachHook gOnStartHook = nullptr;

bool RenderThread::hasInstance() {
    return gHasRenderThreadInstance;
}

void RenderThread::setOnStartHook(JVMAttachHook onStartHook) {
    LOG_ALWAYS_FATAL_IF(hasInstance(), "can't set an onStartHook after we've started...");
    gOnStartHook = onStartHook;
}

JVMAttachHook RenderThread::getOnStartHook() {
    return gOnStartHook;
}

RenderThread& RenderThread::getInstance() {
    [[clang::no_destroy]] static sp<RenderThread> sInstance = []() {
        sp<RenderThread> thread = sp<RenderThread>::make();
        thread->start("RenderThread");
        return thread;
    }();
    gHasRenderThreadInstance = true;
    return *sInstance;
}

RenderThread::RenderThread()
        : ThreadBase()
        , mVsyncSource(nullptr)
        , mVsyncRequested(false)
        , mFrameCallbackTaskPending(false)
        , mRenderState(nullptr)
        , mEglManager(nullptr)
        , mFunctorManager(WebViewFunctorManager::instance())
        , mGlobalProfileData(mJankDataMutex) {
    Properties::load();
}

RenderThread::~RenderThread() {}

void RenderThread::initThreadLocals() {
    mEglManager = new EglManager();
    mRenderState = new RenderState(*this);
    mCacheManager = new CacheManager(*this);
}

// As on a device (renderthread/RenderThread.cpp), less the CacheManager's
// context configuration (its persistent shader cache is device-only).
void RenderThread::requireGlContext() {
    if (mEglManager->hasEglContext()) {
        return;
    }
    mEglManager->initialize();

    sk_sp<const GrGLInterface> glInterface = GrGLMakeNativeInterface();
    LOG_ALWAYS_FATAL_IF(!glInterface.get(), "No OpenGL ES interface over ANGLE");

    GrContextOptions options;
    initGrContextOptions(options);
    sk_sp<GrDirectContext> grContext(GrDirectContexts::MakeGL(std::move(glInterface), options));
    LOG_ALWAYS_FATAL_IF(!grContext.get(), "GrDirectContexts::MakeGL failed over ANGLE");
    setGrContext(grContext);
}

void RenderThread::requireVkContext() {}

void RenderThread::initGrContextOptions(GrContextOptions& options) {
    options.fPreferExternalImagesOverES3 = true;
    options.fDisableDistanceFieldPaths = true;
    if (android::base::GetBoolProperty(PROPERTY_REDUCE_OPS_TASK_SPLITTING, true)) {
        options.fReduceOpsTaskSplitting = GrContextOptions::Enable::kYes;
    } else {
        options.fReduceOpsTaskSplitting = GrContextOptions::Enable::kNo;
    }
}

void RenderThread::destroyRenderingContext() {
    mFunctorManager.onContextDestroyed();
    if (Properties::getRenderPipelineType() == RenderPipelineType::SkiaGL &&
        mEglManager->hasEglContext()) {
        setGrContext(nullptr);
        mEglManager->destroy();
    }
}

VulkanManager& RenderThread::vulkanManager() {
    return *mVkManager;
}

void RenderThread::dumpGraphicsMemory(int fd, bool includeProfileData) {}

void RenderThread::getMemoryUsage(size_t* cpuUsage, size_t* gpuUsage) {}

Readback& RenderThread::readback() {
    if (!mReadback) {
        mReadback = new Readback(*this);
    }

    return *mReadback;
}

void RenderThread::setGrContext(sk_sp<GrDirectContext> context) {
    if (mGrContext) {
        mRenderState->onContextDestroyed();
        mGrContext->releaseResourcesAndAbandonContext();
    }
    mGrContext = std::move(context);
    if (mGrContext) {
        DeviceInfo::setMaxTextureSize(mGrContext->maxRenderTargetSize());
    }
}

sk_sp<GrDirectContext> RenderThread::requireGrContext() {
    if (Properties::getRenderPipelineType() == RenderPipelineType::SkiaGL) {
        requireGlContext();
    }
    return mGrContext;
}

bool RenderThread::threadLoop() {
    if (gOnStartHook) {
        gOnStartHook("RenderThread");
    }
    initThreadLocals();

    while (true) {
        waitForWork();
        processQueue();
        mCacheManager->onThreadIdle();
    }

    return false;
}

void RenderThread::postFrameCallback(IFrameCallback* callback) {}

bool RenderThread::removeFrameCallback(IFrameCallback* callback) {
    return false;
}

void RenderThread::pushBackFrameCallback(IFrameCallback* callback) {}

sk_sp<Bitmap> RenderThread::allocateHardwareBitmap(SkBitmap& skBitmap) {
    return nullptr;
}

bool RenderThread::isCurrent() {
    return true;
}

void RenderThread::preload() {}

void RenderThread::trimMemory(TrimLevel level) {}

void RenderThread::trimCaches(CacheTrimLevel level) {}

} /* namespace renderthread */
} /* namespace uirenderer */
} /* namespace android */
