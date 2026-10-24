// Gesher: Skia's "native" GL interface is ANGLE's OpenGL ES (docs/angle.md,
// ADR 0030). Built into Skia in place of its platform factories
// (GrGLMakeNativeInterface_egl/_win/_none.cpp): the entry points come from
// libGLESv2.dll, loaded from the executable's directory (where the installer
// puts it, next to dalvikvm.exe), else through ANGLE's eglGetProcAddress
// (extensions libGLESv2.dll does not export by name). ANGLE's functions use
// the Windows x64 calling convention, which is GR_GL_FUNCTION_TYPE's here.
// Without ANGLE the interface is null and Ganesh GL is not available.
//
// The caller makes an ANGLE context current first (GrGLMakeAssembledInterface
// reads GL_VERSION to pick the OpenGL ES assembly).

#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include <string>

namespace {

HMODULE LoadNextToExe(const wchar_t* name) {
  if (HMODULE loaded = GetModuleHandleW(name)) return loaded;
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      p.resize(slash + 1);
      p += name;
      if (HMODULE m = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return m;
    }
  }
  return LoadLibraryW(name);
}

using EglGetProcAddress = void*(__stdcall*)(const char*);

struct Angle {
  HMODULE gles = nullptr;
  EglGetProcAddress get_proc = nullptr;
};

const Angle& GetAngle() {
  static const Angle angle = [] {
    Angle a;
    a.gles = LoadNextToExe(L"libGLESv2.dll");
    if (HMODULE egl = LoadNextToExe(L"libEGL.dll")) {
      a.get_proc = reinterpret_cast<EglGetProcAddress>(
          reinterpret_cast<void*>(GetProcAddress(egl, "eglGetProcAddress")));
    }
    return a;
  }();
  return angle;
}

GrGLFuncPtr GetProc(void*, const char name[]) {
  const Angle& a = GetAngle();
  if (a.gles != nullptr) {
    if (FARPROC p = GetProcAddress(a.gles, name)) {
      return reinterpret_cast<GrGLFuncPtr>(reinterpret_cast<void*>(p));
    }
  }
  return a.get_proc != nullptr ? reinterpret_cast<GrGLFuncPtr>(a.get_proc(name)) : nullptr;
}

}  // namespace

sk_sp<const GrGLInterface> GrGLMakeNativeInterface() {
  if (GetAngle().gles == nullptr) return nullptr;
  return GrGLMakeAssembledInterface(nullptr, GetProc);
}
