# Gesher's Win32 window (compat/hwui/window, docs/graphics.md "Window"): the
# top-level window an Android window is shown in, its back buffer and its
# input queue (gesher_window); Win32Surface, the ANativeWindow over it, with
# the NDK's ANativeWindow_* functions (gesher_window_surface, over
# hostgraphics' Surface); gesher-app's NativeWindow natives and
# ANativeWindow_toSurface, which build with hwui (core/jni's Surface natives).
set(_win ${CMAKE_SOURCE_DIR}/compat/hwui/window)

add_library(gesher_window STATIC ${_win}/gesher_window.cc ${_win}/gesher_popup.cc
  ${_win}/input_pane.cc)
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

add_executable(windows4_input_pane_probe ${CMAKE_SOURCE_DIR}/probes/windows4/input_pane_probe.cc)
target_link_libraries(windows4_input_pane_probe PRIVATE gesher_window)
target_compile_options(windows4_input_pane_probe PRIVATE -Wall -Wextra -Werror)
set_target_properties(windows4_input_pane_probe PROPERTIES EXCLUDE_FROM_ALL ON)

add_executable(windows4_text_store_probe
  ${CMAKE_SOURCE_DIR}/probes/windows4/text_store_probe.cc
  ${CMAKE_SOURCE_DIR}/compat/input/text_store.cc
  ${CMAKE_SOURCE_DIR}/compat/input/text_service.cc)
target_include_directories(windows4_text_store_probe PRIVATE ${CMAKE_SOURCE_DIR}/compat/input)
target_compile_options(windows4_text_store_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(windows4_text_store_probe PRIVATE user32 ole32 uuid)
set_target_properties(windows4_text_store_probe PROPERTIES EXCLUDE_FROM_ALL ON)

add_library(windows4_tsf_service SHARED ${CMAKE_SOURCE_DIR}/probes/windows4/tsf_service_dll.cc)
target_include_directories(windows4_tsf_service PRIVATE ${CMAKE_SOURCE_DIR}/compat/input)
target_compile_options(windows4_tsf_service PRIVATE -Wall -Wextra -Werror)
target_link_libraries(windows4_tsf_service PRIVATE user32 ole32 uuid)
set_target_properties(windows4_tsf_service PROPERTIES PREFIX "" OUTPUT_NAME windows4_tsf_service EXCLUDE_FROM_ALL ON)
add_dependencies(windows4_text_store_probe windows4_tsf_service)
