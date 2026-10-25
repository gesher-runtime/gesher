# The Microsoft Edge WebView2 SDK (docs/webview.md): the NuGet package
# Microsoft.Web.WebView2, pinned by version and SHA-256, downloaded at
# configure time into the build directory. Gives
#   WEBVIEW2_INCLUDE_DIRS  WebView2.h and Gesher's compat headers for it
#   WEBVIEW2_STATIC_LIB    build/native/x64/WebView2LoaderStatic.lib
#   WEBVIEW2_LOADER_DLL    build/native/x64/WebView2Loader.dll
# Used by cmake/webview.cmake (dalvikvm) and probes/webview (the probe).
set(WEBVIEW2_VERSION 1.0.4258.31)
set(WEBVIEW2_SHA256 56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9)

set(_wv2_dir "${CMAKE_BINARY_DIR}/webview2-sdk-${WEBVIEW2_VERSION}")
set(_wv2_pkg "${CMAKE_BINARY_DIR}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg")
if(NOT EXISTS "${_wv2_dir}/build/native/include/WebView2.h")
  if(NOT EXISTS "${_wv2_pkg}")
    message(STATUS "Downloading Microsoft.Web.WebView2 ${WEBVIEW2_VERSION}")
    file(DOWNLOAD
      "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${WEBVIEW2_VERSION}/microsoft.web.webview2.${WEBVIEW2_VERSION}.nupkg"
      "${_wv2_pkg}" EXPECTED_HASH SHA256=${WEBVIEW2_SHA256} TLS_VERIFY ON STATUS _wv2_status)
    list(GET _wv2_status 0 _wv2_code)
    if(NOT _wv2_code EQUAL 0)
      file(REMOVE "${_wv2_pkg}")
      message(FATAL_ERROR "Downloading the WebView2 SDK failed: ${_wv2_status}")
    endif()
  endif()
  file(ARCHIVE_EXTRACT INPUT "${_wv2_pkg}" DESTINATION "${_wv2_dir}"
    PATTERNS "build/native/include/*" "build/native/x64/*")
endif()

set(WEBVIEW2_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../compat/webview/include"
  "${_wv2_dir}/build/native/include")
set(WEBVIEW2_STATIC_LIB "${_wv2_dir}/build/native/x64/WebView2LoaderStatic.lib")
set(WEBVIEW2_LOADER_DLL "${_wv2_dir}/build/native/x64/WebView2Loader.dll")
