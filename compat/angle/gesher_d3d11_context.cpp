// Copyright 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// Private host ABI, deliberately not an EGL/GL extension exposed to APKs.
// D3D11 has one immediate context behind all of this display's GL contexts.
// Fence Signal/Wait/Flush must use the SAME mutex as GL submission; acquiring
// an unrelated shim mutex or using the device's immediate context directly
// would race ANGLE. The callback may enqueue synchronization only: no render
// state changes, no GL/EGL reentry, and no CPU wait for the GPU.
#include "libANGLE/Context.h"
#include "libANGLE/renderer/d3d/d3d11/Context11.h"
#include "libANGLE/renderer/d3d/d3d11/Renderer11.h"
#include "libGLESv2/global_state.h"

#if !defined(ANGLE_FORCE_CONTEXT_CHECK_EVERY_CALL)
#    error "Gesher's D3D11 dispatch requires the shared global GL mutex"
#endif

extern "C" __declspec(dllexport) int __stdcall gesherAngleD3D11ContextV1(
    void(__stdcall *operation)(ID3D11Device *, ID3D11DeviceContext *, void *), void *argument)
{
    ANGLE_SCOPED_GLOBAL_LOCK();
    gl::Context *context = gl::GetValidGlobalContext();
    if (context == nullptr || operation == nullptr)
    {
        return 0;
    }
    auto *renderer = rx::GetImplAs<rx::Context11>(context)->getRenderer();
    operation(renderer->getDevice(), renderer->getDeviceContext(), argument);
    return 1;
}

extern "C" __declspec(dllexport) void __stdcall gesherAngleEglErrorV1(unsigned error)
{
    ANGLE_SCOPED_GLOBAL_LOCK();
    egl::GetCurrentThread()->setError(static_cast<EGLint>(error), "Gesher EGL", nullptr,
                                     "Android EGL operation failed");
}
