// gesher: ASurfaceControl for an app's ELF libraries (ADR 0089); see
// surface_control.cc. What libandroid.cc's ASurfaceControl_fromJava uses.
#pragma once

#include <stdint.h>

struct ASurfaceControl;

namespace gesher::bionic::sc {

// The ASurfaceControl standing for a Java android.view.SurfaceControl whose
// native object is `native_object` (one per object: a second call gives the
// same one), with a reference the caller owns. Gesher's Java SurfaceControls
// are not composited (docs/graphics.md, the device stand-ins), so the layer is
// not on a display: what is put under it is not shown.
ASurfaceControl* ForJavaSurfaceControl(int64_t native_object, const char* name);

}  // namespace gesher::bionic::sc
