# Gesher: Windows desktop integration (desktop polish track: drag & drop, rich clipboard, notifications)
set(_win_compat ${CMAKE_SOURCE_DIR}/compat/windows)

add_library(gesher_desktop_compat STATIC
  ${_win_compat}/drop_target.cc
  ${_win_compat}/clipboard_bridge.cc
  ${_win_compat}/clipboard_publication.cc
  ${_win_compat}/notifications.cc
  ${_win_compat}/window.cc)

target_include_directories(gesher_desktop_compat PUBLIC ${_win_compat}
  ${CMAKE_SOURCE_DIR}/compat/hwui/window
  ${CMAKE_SOURCE_DIR}/compat/devsvc
  ${AOSP_DIR}/libnativehelper/include_jni
  ${AOSP_DIR}/libnativehelper/include)
target_compile_options(gesher_desktop_compat PRIVATE -Wall -Wextra -Werror)
# These clipboard boundaries own C++ buffers and convert allocation failure to
# an unavailable receipt. Override the global AOSP -fno-exceptions only here.
set_property(SOURCE ${_win_compat}/clipboard_publication.cc
  ${_win_compat}/clipboard_bridge.cc APPEND PROPERTY COMPILE_OPTIONS -fexceptions)
target_link_libraries(gesher_desktop_compat PUBLIC ole32 uuid shell32 user32)

if(TARGET framework_runtime)
  target_link_libraries(framework_runtime PUBLIC gesher_desktop_compat)
endif()

if(TARGET gesher_window)
  target_link_libraries(gesher_window PUBLIC gesher_desktop_compat)
endif()
