# Not included yet (docs/graphics.md, "Next concrete steps"): written, never
# compiled. cmake/aosp/graphics.cmake includes it once the window step starts.
# Gesher's Win32 window (compat/hwui/window, docs/graphics.md "Window"): the
# top-level window an Android window is shown in, its back buffer and its
# input queue.
add_library(gesher_window STATIC ${CMAKE_SOURCE_DIR}/compat/hwui/window/gesher_window.cc)
target_include_directories(gesher_window PUBLIC ${CMAKE_SOURCE_DIR}/compat/hwui/window)
target_compile_options(gesher_window PRIVATE -Wall -Wextra -Werror)
target_link_libraries(gesher_window PUBLIC user32 gdi32)
gesher_graphics_lib(gesher_window)

# The window probe (probes/graphics/window_probe.cc), run in CI on Windows.
add_executable(window_probe ${CMAKE_SOURCE_DIR}/probes/graphics/window_probe.cc)
target_link_libraries(window_probe PRIVATE gesher_window skia)
target_compile_options(window_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
set_target_properties(window_probe PROPERTIES EXCLUDE_FROM_ALL ON)
