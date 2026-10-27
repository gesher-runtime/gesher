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
set(_wv_libs ole32 oleaut32 uuid shlwapi user32 gdi32 advapi32)
if(GESHER_WEBVIEW2_LOADER STREQUAL "static")
  target_sources(gesher_webview PRIVATE ${_wv}/msvc_shims.cc)
  set_source_files_properties(${_wv}/msvc_shims.cc PROPERTIES COMPILE_OPTIONS -fno-emulated-tls)
  target_compile_definitions(gesher_webview PRIVATE GESHER_WEBVIEW2_STATIC)
  list(APPEND _wv_libs ${WEBVIEW2_STATIC_LIB})
endif()

target_sources(dalvikvm PRIVATE $<TARGET_OBJECTS:gesher_webview>)
target_link_libraries(dalvikvm PRIVATE ${_wv_libs})

# web3 track (ADR 0058): accessibility over UI Automation (libgesher_a11y.so:
# Android's views for UIA clients, WebView2's UIA tree for a WebView's
# AccessibilityNodeProvider) and Windows' proxy settings
# (libgesher_proxy.so), builtin libraries of dalvikvm.
add_library(gesher_web3 OBJECT ${CMAKE_SOURCE_DIR}/compat/accessibility/uia_bridge.cc
  ${CMAKE_SOURCE_DIR}/compat/accessibility/uia_client.cc ${CMAKE_SOURCE_DIR}/compat/net/windows_proxy.cc)
target_link_libraries(gesher_web3 PRIVATE nativehelper gesher_posix)
target_compile_options(gesher_web3 PRIVATE -Wall -Werror -Wno-unused-parameter)
target_sources(dalvikvm PRIVATE $<TARGET_OBJECTS:gesher_web3>)
target_link_libraries(dalvikvm PRIVATE uiautomationcore oleaut32 ole32 winhttp user32 advapi32)
