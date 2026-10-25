# Gesher (devices track, docs/devices.md): hardware-backed device APIs on
# Windows. The natives join libandroid_runtime's builtin library
# (android_runtime.cc calls register_gesher_devices after the device
# stand-ins, so the real tables replace them). Windows' APIs are reached at
# run time (combase's WinRT functions, Media Foundation), so dalvikvm starts
# on Windows editions without them.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_frames.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_metadata_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_mf.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/devices_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/location_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/winrt.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/devices)
target_link_libraries(framework_runtime PUBLIC ole32)

# libcamera_metadata (system/media/camera/Android.bp): the metadata buffers
# behind CameraMetadataNative (camera_metadata_jni.cc).
set(_camera_metadata "${AOSP_DIR}/system/media/camera")
add_library(camera_metadata STATIC ${_camera_metadata}/src/camera_metadata.c
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_metadata_compat.c)
target_include_directories(camera_metadata PUBLIC ${_camera_metadata}/include
  ${AOSP_DIR}/system/media/private/camera/include)
target_link_libraries(camera_metadata PUBLIC cutils log)
target_compile_definitions(camera_metadata PRIVATE LOG_NDEBUG=1)
# The Windows C runtime has no dprintf (camera_metadata_compat.h).
target_compile_options(camera_metadata PRIVATE
  -include ${CMAKE_SOURCE_DIR}/compat/devices/camera_metadata_compat.h)
target_link_libraries(framework_runtime PUBLIC camera_metadata)
