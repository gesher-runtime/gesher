# CI fix-ups for tools/host, injected with
#   -DCMAKE_PROJECT_gesher_host_tools_INCLUDE=tools/host-tools-ci.cmake
# Each one stands in for a change tools/host/CMakeLists.txt should make, and
# turns itself off once it does.

# sysprop_java's libpropertyinfoserializer calls PropertyInfoArea's lookups,
# defined in libpropertyinfoparser (Soong links libpropertyinfoparser).
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _gesher_host_tools_ci_fixups)

function(_gesher_host_tools_ci_fixups)
  set(parser "${AOSP_DIR}/system/core/property_service/libpropertyinfoparser/property_info_parser.cpp")
  if(TARGET sysprop_java)
    get_target_property(srcs sysprop_java SOURCES)
    if(NOT srcs MATCHES "property_info_parser\\.cpp")
      message(STATUS "CI: adding property_info_parser.cpp to sysprop_java")
      target_sources(sysprop_java PRIVATE "${parser}")
    endif()
  endif()
endfunction()
