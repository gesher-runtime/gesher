# android.webkit.WebView on Microsoft Edge WebView2 (docs/webview.md, ADR
# 0041): compat/webview's host and com.gesher.app.webview.WebView2Host's
# natives, in dalvikvm as the builtin library libgesher_webview.so.
#
# GESHER_WEBVIEW2_LOADER: "static" links the SDK's WebView2LoaderStatic.lib
# (with msvc_shims.cc); "dll" loads WebView2Loader.dll from dalvikvm's
# directory at run time (shipped next to it).
include(${CMAKE_CURRENT_LIST_DIR}/webview2_sdk.cmake)

set(GESHER_WEBVIEW2_LOADER "static" CACHE STRING "How dalvikvm gets the WebView2 loader: static or dll")
set_property(CACHE GESHER_WEBVIEW2_LOADER PROPERTY STRINGS static dll)

set(_wv ${CMAKE_SOURCE_DIR}/compat/webview)
# An OBJECT library: the builtin library registers itself from a static
# initializer, which an archive member nothing refers to would lose.
add_library(gesher_webview OBJECT ${_wv}/webview_host.cc ${_wv}/webview2_loader.cc
  ${_wv}/webview_jni.cc)
target_include_directories(gesher_webview PRIVATE ${_wv})
target_include_directories(gesher_webview SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIRS})
target_link_libraries(gesher_webview PRIVATE nativehelper gesher_posix)
target_compile_options(gesher_webview PRIVATE -Wall -Werror -Wno-unused-parameter)
set(_wv_libs ole32 oleaut32 uuid shlwapi user32 gdi32 advapi32 bcrypt)
if(GESHER_WEBVIEW2_LOADER STREQUAL "static")
  target_sources(gesher_webview PRIVATE ${_wv}/msvc_shims.cc)
  set_source_files_properties(${_wv}/msvc_shims.cc PROPERTIES COMPILE_OPTIONS -fno-emulated-tls)
  target_compile_definitions(gesher_webview PRIVATE GESHER_WEBVIEW2_STATIC)
  list(APPEND _wv_libs ${WEBVIEW2_STATIC_LIB})
endif()

target_sources(dalvikvm PRIVATE $<TARGET_OBJECTS:gesher_webview>)
target_link_libraries(dalvikvm PRIVATE ${_wv_libs})

# The Java provider loads a distinct PE library name. A builtin of the same
# basename would shadow a file-based update in Gesher's dlopen implementation.
# Keep the bridge's Windows COM/CRT dependencies separate from gesher_posix:
# directory-wide link_libraries(gesher_posix) is for the runtime's AOSP targets.
add_library(gesher_webview_bridge SHARED ${_wv}/webview_host.cc
  ${_wv}/webview2_loader.cc ${_wv}/webview_jni.cc ${_wv}/webview_bridge.def)
set_property(TARGET gesher_webview_bridge PROPERTY LINK_LIBRARIES "")
target_include_directories(gesher_webview_bridge PRIVATE ${_wv}
  "${AOSP_DIR}/libnativehelper/include_jni")
target_include_directories(gesher_webview_bridge SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIRS})
target_compile_definitions(gesher_webview_bridge PRIVATE GESHER_WEBVIEW_SHARED)
target_compile_options(gesher_webview_bridge PRIVATE -Wall -Werror -Wno-unused-parameter)
target_link_libraries(gesher_webview_bridge PRIVATE ${_wv_libs} version)
target_link_options(gesher_webview_bridge PRIVATE -Wl,--exclude-all-symbols)
if(GESHER_WEBVIEW2_LOADER STREQUAL "static")
  target_sources(gesher_webview_bridge PRIVATE ${_wv}/msvc_shims.cc)
  target_compile_definitions(gesher_webview_bridge PRIVATE GESHER_WEBVIEW2_STATIC)
endif()
set_target_properties(gesher_webview_bridge PROPERTIES
  PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "gesher_webview_bridge")
add_dependencies(dalvikvm gesher_webview_bridge)

# Private helper regression only; no additional runtime hooks or default targets.
option(GESHER_WEBVIEW_BRIDGE_TESTS "Build native WebView bridge boundary tests" OFF)
if(GESHER_WEBVIEW_BRIDGE_TESTS)
  add_executable(webview_bridge_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/native/webview_bridge_test.cc ${_wv}/webview2_loader.cc)
  target_include_directories(webview_bridge_test PRIVATE ${_wv})
  target_include_directories(webview_bridge_test SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIRS})
  target_compile_options(webview_bridge_test PRIVATE -Wall -Werror -Wno-unused-parameter -UNDEBUG)
  target_link_libraries(webview_bridge_test PRIVATE ${_wv_libs})
  if(GESHER_WEBVIEW2_LOADER STREQUAL "static")
    target_sources(webview_bridge_test PRIVATE ${_wv}/msvc_shims.cc)
    target_compile_definitions(webview_bridge_test PRIVATE GESHER_WEBVIEW2_STATIC)
  endif()
endif()

# web3 track (ADR 0058): accessibility over UI Automation (libgesher_a11y.so:
# Android's views for UIA clients, WebView2's UIA tree for a WebView's
# AccessibilityNodeProvider) and Windows' proxy settings
# (libgesher_proxy.so), builtin libraries of dalvikvm.
add_library(gesher_web3 OBJECT ${CMAKE_SOURCE_DIR}/compat/accessibility/uia_bridge.cc
  ${CMAKE_SOURCE_DIR}/compat/accessibility/query_pool.cc
  ${CMAKE_SOURCE_DIR}/compat/accessibility/uia_client.cc ${CMAKE_SOURCE_DIR}/compat/net/windows_proxy.cc)
target_link_libraries(gesher_web3 PRIVATE nativehelper gesher_posix)
target_compile_options(gesher_web3 PRIVATE -Wall -Werror -Wno-unused-parameter)
target_sources(dalvikvm PRIVATE $<TARGET_OBJECTS:gesher_web3>)
target_link_libraries(dalvikvm PRIVATE uiautomationcore oleaut32 ole32 winhttp user32 advapi32)

# Test-only HWND dispatch hook, absent from normal runtime/package builds.
option(GESHER_WEB3_QUERY_TESTS "Compile the real-window query contention fixture hook" OFF)
if(GESHER_WEB3_QUERY_TESTS)
  target_compile_definitions(gesher_web3 PRIVATE GESHER_WEB3_QUERY_TESTS=1)
endif()
