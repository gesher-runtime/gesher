# The WebView2 probe (docs/webview.md), a project of its own: it needs only
# llvm-mingw and the WebView2 SDK, not AOSP, so .github/workflows/webview.yml
# builds and runs it in minutes.
#
#   cmake -S probes/webview -B out-webview -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-llvm-mingw.cmake -DLLVM_MINGW=...
#   ninja -C out-webview -k 0
#
# Two executables: webview2_probe.exe links WebView2LoaderStatic.lib (with
# msvc_shims.cc), webview2_probe_dll.exe loads WebView2Loader.dll.
cmake_minimum_required(VERSION 3.24)
project(gesher_webview_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
add_compile_options(-fno-exceptions)
add_link_options(-static -municode)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/webview2_sdk.cmake)
set(_wv ${CMAKE_CURRENT_LIST_DIR}/../../compat/webview)
set(_libs ole32 oleaut32 uuid shlwapi user32 gdi32 advapi32 windowscodecs)

foreach(_variant static dll)
  if(_variant STREQUAL "static")
    set(_name webview2_probe)
  else()
    set(_name webview2_probe_dll)
  endif()
  add_executable(${_name} ${CMAKE_CURRENT_LIST_DIR}/webview2_probe.cc ${_wv}/webview_host.cc
    ${_wv}/webview2_loader.cc)
  target_include_directories(${_name} PRIVATE ${_wv})
  target_include_directories(${_name} SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIRS})
  target_compile_options(${_name} PRIVATE -Wall -Werror -Wno-unused-parameter)
  target_link_libraries(${_name} PRIVATE ${_libs})
  if(_variant STREQUAL "static")
    target_sources(${_name} PRIVATE ${_wv}/msvc_shims.cc)
    set_source_files_properties(${_wv}/msvc_shims.cc PROPERTIES COMPILE_OPTIONS -fno-emulated-tls)
    target_compile_definitions(${_name} PRIVATE GESHER_WEBVIEW2_STATIC)
    target_link_libraries(${_name} PRIVATE ${WEBVIEW2_STATIC_LIB})
  endif()
endforeach()

# What the probe job ships next to the dll variant.
configure_file(${WEBVIEW2_LOADER_DLL} ${CMAKE_BINARY_DIR}/WebView2Loader.dll COPYONLY)
