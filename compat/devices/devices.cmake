# Gesher (devices track, docs/devices.md): hardware-backed device APIs on
# Windows. The natives join libandroid_runtime's builtin library
# (android_runtime.cc calls register_gesher_devices after the device
# stand-ins, so the real tables replace them). Windows' APIs are reached at
# run time (combase's WinRT functions, Media Foundation), so dalvikvm starts
# on Windows editions without them.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/devices/bluetooth.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/bluetooth_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/bluetooth_test.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/bluetooth_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_frames.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/caption_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/caption_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_metadata_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/camera_mf.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/devices_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/hello_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/hello_owner.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/hello_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/location_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/webauthn_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/webauthn_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/speech_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/speech_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/speech_recognition_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/speech_recognition_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/winrt.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/devices
  ${CMAKE_SOURCE_DIR}/compat/bionic/include)  # gesher/rgba_buffers.h
target_link_libraries(framework_runtime PUBLIC ole32)
# Location: the GNSS receiver's device interface (CM_Get_Device_Interface_List).
target_link_libraries(framework_runtime PUBLIC cfgmgr32)
# Bluetooth (docs/bluetooth.md): Winsock's AF_BTH sockets and WSASetService for
# RFCOMM; IID_IUnknown. BluetoothApis.dll and the WinRT classes are found at run time.
# Speech synthesis (docs/tts.md): IID_IStream; shcore.dll's
# CreateStreamOverRandomAccessStream is found at run time.
# Speech recognition (docs/speech-recognition.md): IID_IStream; SAPI's classes are
# created through COM (sapi.dll), so nothing more is linked.
target_link_libraries(framework_runtime PUBLIC ws2_32 uuid)

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
