# Gesher's Win32 window (compat/hwui/window, docs/graphics.md "Window"): the
# top-level window an Android window is shown in, its back buffer and its
# input queue (gesher_window); Win32Surface, the ANativeWindow over it, with
# the NDK's ANativeWindow_* functions (gesher_window_surface, over
# hostgraphics' Surface); gesher-app's NativeWindow natives and
# ANativeWindow_toSurface, which build with hwui (core/jni's Surface natives).
set(_win ${CMAKE_SOURCE_DIR}/compat/hwui/window)

add_library(gesher_window STATIC ${_win}/gesher_window.cc)
target_include_directories(gesher_window PUBLIC ${_win})
target_compile_options(gesher_window PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gesher_window PUBLIC user32 gdi32 ole32)
gesher_graphics_lib(gesher_window)

add_library(gesher_window_surface STATIC ${_win}/win32_surface.cc ${_win}/native_window.cc)
target_link_libraries(gesher_window_surface PUBLIC gesher_window hostgraphics)
target_compile_options(gesher_window_surface PRIVATE -Wall -Werror)
gesher_graphics_cxx(gesher_window_surface)
gesher_graphics_lib(gesher_window_surface)

target_sources(hwui PRIVATE ${_win}/window_jni.cc)
target_link_libraries(hwui PUBLIC gesher_window_surface)

# The window probe (probes/graphics/window_probe.cc), run in CI on Windows.
add_executable(window_probe ${CMAKE_SOURCE_DIR}/probes/graphics/window_probe.cc)
target_link_libraries(window_probe PRIVATE gesher_window_surface skia)
target_compile_options(window_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
gesher_graphics_cxx(window_probe)
set_target_properties(window_probe PROPERTIES EXCLUDE_FROM_ALL ON)
