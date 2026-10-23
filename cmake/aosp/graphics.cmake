# Graphics (docs/graphics.md, roadmap phase 3): Skia and its libraries,
# minikin, hwui, and the window and input code of gesher-base.
include(${CMAKE_CURRENT_LIST_DIR}/skia.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/hwui.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/window.cmake)

# hwui in dalvikvm: libandroid_runtime's table (compat/framework/android_runtime.cc)
# registers its natives through register_gesher_graphics.
target_link_libraries(dalvikvm PRIVATE gesher_graphics)

# What .github/workflows/graphics.yml builds: every graphics library, and the
# probes it runs on Windows.
add_custom_target(graphics_all DEPENDS ${GESHER_GRAPHICS_TARGETS} skia_probe window_probe)
