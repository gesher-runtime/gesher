# gesher: android.app.NativeActivity, android.view.InputQueue and libandroid's
# AInputQueue/AInputEvent/AChoreographer for the app's .so files
# (docs/native-libs.md, "NativeActivity"; ADR 0032): native_activity.cc
# (after core/jni's android_app_NativeActivity.cpp and android_view_InputQueue.cpp
# and native/android's input.cpp) and choreographer.cc. An OBJECT library in
# dalvikvm: register_gesher_native_activity is in libandroid_runtime's table
# (compat/framework/android_runtime.cc), and static initializers add the
# System V entries to the bionic shim's table. Over libinput (android_input)
# and the framework's KeyEvent/MotionEvent natives (input_jni), which
# cmake/aosp/input.cmake defines later (targets resolve at generate time).
set(_na_fwbase ${AOSP_DIR}/frameworks/base)
add_library(gesher_native_activity OBJECT
  ${CMAKE_CURRENT_LIST_DIR}/native_activity.cc
  ${CMAKE_CURRENT_LIST_DIR}/choreographer.cc)
target_include_directories(gesher_native_activity PRIVATE ${CMAKE_CURRENT_LIST_DIR}
  ${_na_fwbase}/core/jni ${_na_fwbase}/core/jni/include ${CMAKE_SOURCE_DIR}/compat/framework/include)
target_link_libraries(gesher_native_activity PRIVATE gesher_bionic gesher_elf android_input input_jni
  graphics_native_headers androidfw nativehelper utils cutils log base)
# As cmake/aosp/hwui.cmake's gesher_graphics_cxx (defined after this file):
# C++23 (utils/Trace.h and <stdatomic.h>), and what AOSP's headers trip.
set_target_properties(gesher_native_activity PROPERTIES CXX_STANDARD 23)
target_compile_options(gesher_native_activity PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-deprecated-copy -Wno-deprecated-anon-enum-enum-conversion -Wno-non-virtual-dtor
  -Wno-inconsistent-missing-override -Wno-invalid-specialization -Wno-deprecated-declarations
  "SHELL:-include new -include cmath"
  "SHELL:-include ${CMAKE_SOURCE_DIR}/compat/input/libinput/chrono_literals.h")
target_link_libraries(dalvikvm PRIVATE gesher_native_activity)
