// Gesher: hardware/libhardware's <hardware/hardware.h>, as far as the host
// graphics code uses it. ui/PixelFormat.h includes it for the HAL pixel
// formats, which are <system/graphics.h>'s; the HAL module structures are
// device-only and left out.
#pragma once

#include <cutils/native_handle.h>
#include <system/graphics.h>
