# hwui (frameworks/base/libs/hwui) for Windows, with what it builds on:
# HarfBuzz, minikin, libui-types and libhostgraphics. hwui's host variant is
# the one layoutlib uses (SkiaCpu pipeline, HWUI_NULL_GPU); its Windows code
# paths are the non-Linux ones Android.bp's Windows layoutlib once took.
# docs/graphics.md has the details.

set(_fw "${AOSP_DIR}/frameworks/base")
set(_hwui "${_fw}/libs/hwui")
set(_native "${AOSP_DIR}/frameworks/native")

# What llvm-mingw's newer libc++ is stricter about than Soong's host libc++:
# libutils' StrongPointer.h uses std::nothrow without <new>, ui/Rect.h
# std::round without <cmath>; utils/Trace.h mixes C's <stdatomic.h> with
# <atomic>, which libc++ allows from C++23 on (framework.cmake does the same);
# math/half.h specializes std::is_floating_point and uses std::float_denorm_style
# (deprecated in C++23).
function(gesher_graphics_cxx target)
  set_target_properties(${target} PROPERTIES CXX_STANDARD 23)
  target_compile_options(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include new -include cmath>"
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-invalid-specialization>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>)
endfunction()

# --- HarfBuzz (external/harfbuzz_ng: libharfbuzz_ng) ----------------------------
gesher_bp_cc(HB external/harfbuzz_ng libharfbuzz_ng)
gesher_bp_library(harfbuzz HB QUIET)
target_link_libraries(harfbuzz PUBLIC icu_headers PRIVATE icuuc cutils utils log)
gesher_graphics_lib(harfbuzz)

# --- minikin (frameworks/minikin: libminikin) -------------------------------------
# frameworks/minikin patch 0001: the Rust hyphenator is left out (no Rust
# toolchain); the C++ one reads the same patterns.
gesher_bp_cc(MINIKIN frameworks/minikin/libs/minikin libminikin EXCLUDE "^Debug\\.cpp$")
gesher_bp_library(minikin MINIKIN)
# libminikin_defaults (frameworks/minikin/Android.bp)
target_compile_options(minikin PRIVATE -Wall -Wextra -Wthread-safety "-DLOG_TAG=\"Minikin\""
  -Wno-unused-parameter)
target_include_directories(minikin
  PRIVATE ${AOSP_DIR}/frameworks/minikin/libs/minikin
  PUBLIC ${AOSP_DIR}/frameworks/minikin/include
         ${AOSP_DIR}/external/googletest/googletest/include)  # gtest_prod.h
target_link_libraries(minikin PUBLIC harfbuzz icu_headers icuuc icui18n utils cutils log base)
gesher_graphics_lib(minikin)

# --- libmath, libarect, the native window headers (header-only) ------------------
add_library(graphics_native_headers INTERFACE)
target_include_directories(graphics_native_headers INTERFACE
  ${_native}/libs/math/include
  ${_native}/libs/arect/include
  ${_native}/libs/nativebase/include
  ${_native}/libs/nativewindow/include
  ${_native}/libs/nativedisplay/include
  ${_native}/include
  ${_native}/include/private              # surface_control_private.h (RenderThread.h)
  ${_native}/opengl/include
  # hardware/hardware.h (ui/PixelFormat.h): what of libhardware's header it uses.
  ${CMAKE_SOURCE_DIR}/compat/hwui/include-native
  ${AOSP_DIR}/external/vulkan-headers/include
  ${AOSP_DIR}/frameworks/av/media/ndk/include)

# --- libui-types (frameworks/native/libs/ui) -------------------------------------
gesher_bp_cc(UI frameworks/native/libs/ui libui-types)
gesher_bp_library(ui_types UI)
target_link_libraries(ui_types PUBLIC graphics_native_headers utils base log)
gesher_graphics_cxx(ui_types)
gesher_graphics_lib(ui_types)

# --- libhostgraphics (frameworks/base/libs/hostgraphics) -------------------------
# The host's ANativeWindow, Surface and BufferQueue, which hwui's host variant
# and libandroid_runtime's Surface natives are written against. Its include
# directory goes ahead of frameworks/native's, whose gui/ and ui/ headers are
# the device's.
gesher_bp_cc(HOSTGRAPHICS frameworks/base/libs/hostgraphics libhostgraphics)
gesher_bp_library(hostgraphics HOSTGRAPHICS)
target_include_directories(hostgraphics BEFORE PUBLIC ${_fw}/libs/hostgraphics/include)
target_link_libraries(hostgraphics PUBLIC ui_types graphics_native_headers utils base log)
gesher_graphics_cxx(hostgraphics)
gesher_graphics_lib(hostgraphics)

# --- hwui (frameworks/base/libs/hwui: libhwui, host) -----------------------------
# Left out: YuvToJpegEncoder (libultrahdr; android.graphics.YuvImage), and
# LayoutlibLoader, whose registration table compat/hwui/graphics_runtime.cc
# replaces. compat/hwui stands in for what Soong generates (the sysprop
# accessors) or Gesher does not build (perfetto's C API, dlopen of
# libandroid_runtime.so in SharedLib).
gesher_bp_cc(HWUI frameworks/base/libs/hwui libhwui
  EXCLUDE "^jni/YuvToJpegEncoder\\.cpp$" "^apex/LayoutlibLoader\\.cpp$")
# gui/TraceUtils.h, alone of libgui's headers (the rest are hostgraphics').
configure_file(${_native}/libs/gui/include/gui/TraceUtils.h
  ${CMAKE_BINARY_DIR}/gen/hwui-gui/gui/TraceUtils.h COPYONLY)
add_library(hwui STATIC ${HWUI_SRCS}
  ${CMAKE_SOURCE_DIR}/compat/hwui/SharedLib.cc
  ${CMAKE_SOURCE_DIR}/compat/hwui/graphics_runtime.cc
  # libandroid_runtime's Surface natives and ANativeWindow_fromSurface, in
  # their host form (core/jni/Android.bp), over hostgraphics' Surface.
  ${_fw}/core/jni/android_view_Surface.cpp
  ${_fw}/core/jni/platform/host/native_window_jni.cpp)
gesher_bp_flags(hwui HWUI)
gesher_graphics_cxx(hwui)
target_compile_options(hwui PRIVATE
  # What clang for Windows reports that Soong's host build does not see.
  -Wno-error -Wno-unused-function -Wno-unknown-pragmas)
target_include_directories(hwui PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/hwui/include
  ${CMAKE_SOURCE_DIR}/compat/framework/include
  ${_hwui} ${_hwui}/platform/host ${_hwui}/jni ${_hwui}/apex/include
  ${CMAKE_BINARY_DIR}/gen/hwui-gui
  ${HWUI_INCLUDES}
  ${_fw}/core/jni ${_fw}/core/jni/include)
target_link_libraries(hwui PUBLIC skia minikin harfbuzz hostgraphics ui_types graphics_native_headers
  androidfw nativehelper ziparchive utils cutils log base icu_headers icuuc icui18n jpeg z)
gesher_graphics_lib(hwui)
# hwui's registrations run from libandroid_runtime's table
# (compat/framework/android_runtime.cc: register_gesher_graphics).
add_library(gesher_graphics INTERFACE)
target_link_libraries(gesher_graphics INTERFACE hwui)
