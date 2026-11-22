# Explicit production publisher overlay; independent of selected-minimum.
include_guard(GLOBAL)
if(NOT "$ENV{GESHER_HOST_PTHREAD_SELECTION}" STREQUAL "publisher-cancel")
  message(FATAL_ERROR "Cancellation link capture requires the explicit publisher-cancel role")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
function(_gesher_cancel_maps)
  foreach(target libbinder_probe stdio_ofd_test servicemanager dalvikvm)
    if(TARGET "${target}")
      if(NOT target STREQUAL "dalvikvm")
        target_link_options("${target}" PRIVATE "-Wl,-Map,$<TARGET_FILE:${target}>.pthread-cancel.map")
      endif()
      target_link_options("${target}" PRIVATE "-Wl,--verbose")
      set_property(TARGET "${target}" PROPERTY C_LINKER_LAUNCHER
        "${Python3_EXECUTABLE};${CMAKE_CURRENT_FUNCTION_LIST_DIR}/prepare_pthread_cancel.py;link")
      set_property(TARGET "${target}" PROPERTY CXX_LINKER_LAUNCHER
        "${Python3_EXECUTABLE};${CMAKE_CURRENT_FUNCTION_LIST_DIR}/prepare_pthread_cancel.py;link")
    endif()
  endforeach()
  # dalvikvm already supplies its production map, consumed without replacement.
endfunction()
cmake_language(DEFER CALL _gesher_cancel_maps)
