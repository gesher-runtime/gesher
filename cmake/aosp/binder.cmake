# frameworks/native/libs/binder: libbinder, mirroring its Android.bp host
# configuration with kernel IPC (libbinder_kernel_defaults), as the framework's
# Binder and Parcel natives use it (docs/framework-natives.md, step 5).
#
# Built without __ANDROID__: ProcessState's open of /dev/binder fails
# (gesher-base: ENOENT) and only logs, so local Binder objects, Parcel and
# in-process transactions work and nothing reaches a driver. The binder
# observer (RELEASE_LIBBINDER_BINDER_OBSERVER) is off, as in the release, and
# its sources, though IPCThreadState and ProcessState still include its
# headers and so the statsbootstrap AIDL's. The AIDL interfaces libbinder implements
# (IServiceManager and friends) are generated at configure time by the aidl of
# Google's build-tools, the release Android 16 ships.

set(_binder "${AOSP_DIR}/frameworks/native/libs/binder")

# --- aidl ---------------------------------------------------------------------
set(GESHER_AIDL "" CACHE FILEPATH "aidl compiler (default: from build-tools, downloaded)")
if(NOT GESHER_AIDL)
  set(_bt_version 36.1)
  if(CMAKE_HOST_WIN32)
    set(_bt_host windows)
    set(_bt_sha256 23189d2d52b40a070a05e9cf7e497c9563f67fee76902e8fd3135ef29ef4dbeb)
    set(_aidl_name aidl.exe)
  else()
    set(_bt_host linux)
    set(_bt_sha256 a7b5889e4a79fcf3b0976bef40d401f4240fb1eed891d9d91169da1111e11d78)
    set(_aidl_name aidl)
  endif()
  set(_bt "${CMAKE_BINARY_DIR}/build-tools-${_bt_version}")
  if(NOT EXISTS "${_bt}/${_aidl_name}")
    set(_zip "${CMAKE_BINARY_DIR}/build-tools-${_bt_version}.zip")
    message(STATUS "Fetching build-tools ${_bt_version} for aidl")
    file(DOWNLOAD
      "https://dl.google.com/android/repository/build-tools_r${_bt_version}_${_bt_host}.zip"
      "${_zip}" EXPECTED_HASH SHA256=${_bt_sha256} STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "build-tools download failed: ${_status}")
    endif()
    file(REMOVE_RECURSE "${_bt}.tmp")
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_bt}.tmp")
    file(GLOB _bt_root "${_bt}.tmp/*")
    file(RENAME "${_bt_root}" "${_bt}")
    file(REMOVE_RECURSE "${_bt}.tmp")
    file(REMOVE "${_zip}")
  endif()
  set(GESHER_AIDL "${_bt}/${_aidl_name}")
endif()

# Generates C++ for AIDL files at configure time: aidl_cpp(<out var> <import
# root> <files...>) appends the generated sources to <out var>; headers go to
# ${CMAKE_BINARY_DIR}/aidl/include.
set(_aidl_out "${CMAKE_BINARY_DIR}/aidl")
function(aidl_cpp var root)
  set(srcs ${${var}})
  foreach(f ${ARGN})
    file(RELATIVE_PATH rel "${root}" "${f}")
    string(REGEX REPLACE "\\.aidl$" ".cpp" cpp "${rel}")
    set(cpp "${_aidl_out}/src/${cpp}")
    if(NOT EXISTS "${cpp}" OR "${f}" IS_NEWER_THAN "${cpp}")
      # The Windows aidl matches files to import paths by their native form.
      file(TO_NATIVE_PATH "${root}" nroot)
      file(TO_NATIVE_PATH "${f}" nf)
      execute_process(
        COMMAND "${GESHER_AIDL}" --lang=cpp "-I${nroot}" -o "${_aidl_out}/src"
                --header_out "${_aidl_out}/include" "${nf}"
        RESULT_VARIABLE rc ERROR_VARIABLE err)
      if(NOT rc EQUAL 0)
        message(FATAL_ERROR "aidl ${rel}: ${err}")
      endif()
    endif()
    list(APPEND srcs "${cpp}")
  endforeach()
  set(${var} ${srcs} PARENT_SCOPE)
endfunction()

set(_binder_aidl_srcs)
aidl_cpp(_binder_aidl_srcs "${_binder}/aidl"
  # libbinder_aidl, libbinder_accessor_aidl
  ${_binder}/aidl/android/os/ConnectionInfo.aidl
  ${_binder}/aidl/android/os/IClientCallback.aidl
  ${_binder}/aidl/android/os/IServiceCallback.aidl
  ${_binder}/aidl/android/os/IServiceManager.aidl
  ${_binder}/aidl/android/os/Service.aidl
  ${_binder}/aidl/android/os/ServiceWithMetadata.aidl
  ${_binder}/aidl/android/os/ServiceDebugInfo.aidl
  ${_binder}/aidl/android/os/IAccessor.aidl)
# android-os-statsbootstrap-aidl (frameworks/base), for BinderObserver.h.
set(_fwjava "${AOSP_DIR}/frameworks/base/core/java")
aidl_cpp(_binder_aidl_srcs "${_fwjava}"
  ${_fwjava}/android/os/IStatsBootstrapAtomService.aidl
  ${_fwjava}/android/os/StatsBootstrapAtom.aidl
  ${_fwjava}/android/os/StatsBootstrapAtomValue.aidl)

# --- libbinder ------------------------------------------------------------------
add_library(binder STATIC
  # libbinder_common_defaults
  ${_binder}/Binder.cpp ${_binder}/BpBinder.cpp ${_binder}/Debug.cpp ${_binder}/FdTrigger.cpp
  ${_binder}/IInterface.cpp ${_binder}/IResultReceiver.cpp ${_binder}/Parcel.cpp
  ${_binder}/ParcelFileDescriptor.cpp ${_binder}/RecordedTransaction.cpp
  ${_binder}/RpcSession.cpp ${_binder}/RpcServer.cpp ${_binder}/RpcState.cpp
  ${_binder}/RpcTransportRaw.cpp ${_binder}/Stability.cpp ${_binder}/Status.cpp
  ${_binder}/TextOutput.cpp ${_binder}/Utils.cpp ${_binder}/file.cpp
  # libbinder_android_defaults
  ${_binder}/OS_android.cpp ${_binder}/OS_unix_base.cpp
  # libbinder_kernel_defaults, with its host source
  ${_binder}/BufferedTextOutput.cpp ${_binder}/BackendUnifiedServiceManager.cpp
  ${_binder}/IPCThreadState.cpp ${_binder}/IServiceManager.cpp ${_binder}/IServiceManagerFFI.cpp
  ${_binder}/ProcessState.cpp ${_binder}/Static.cpp ${_binder}/ServiceManagerHost.cpp
  ${_binder}/IPermissionController.cpp ${_binder}/PermissionCache.cpp
  ${_binder}/PermissionController.cpp
  ${_binder_aidl_srcs}
  # libbinder
  ${_binder}/IMemory.cpp ${_binder}/IShellCallback.cpp ${_binder}/LazyServiceRegistrar.cpp
  ${_binder}/MemoryBase.cpp ${_binder}/MemoryDealer.cpp ${_binder}/MemoryHeapBase.cpp
  ${_binder}/ParcelableHolder.cpp ${_binder}/PersistableBundle.cpp)
target_include_directories(binder PUBLIC ${_binder}/include ${_binder}/ndk/include_cpp
  ${_aidl_out}/include
  PRIVATE ${_binder} ${AOSP_DIR}/bionic/libc/kernel/uapi)
target_link_libraries(binder PUBLIC utils cutils log base nativehelper)
target_compile_definitions(binder PRIVATE BINDER_WITH_KERNEL_IPC NO_LIBBINDER_BINDER_OBSERVER
  ANDROID_BASE_UNIQUE_FD_DISABLE_IMPLICIT_CONVERSION
  ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION BUILDING_LIBBINDER)
# libbinder_common_defaults' warnings are Soong's plus its own; these fire on
# designated initializers that leave fields out on purpose, on variables only
# the kernel-IPC or debug paths use, and on one override without `override`.
set_target_properties(binder PROPERTIES CXX_STANDARD 23)
target_compile_options(binder PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter
  -Wno-missing-field-initializers -Wno-missing-designated-field-initializers
  -Wno-unused-variable -Wno-unused-but-set-variable -Wno-inconsistent-missing-override)
