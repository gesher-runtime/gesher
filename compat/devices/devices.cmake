# Gesher (devices track, docs/devices.md): hardware-backed device APIs on
# Windows. The natives join libandroid_runtime's builtin library
# (android_runtime.cc calls register_gesher_devices after the device
# stand-ins, so the real tables replace them). Windows' APIs are reached at
# run time (combase's WinRT functions, Media Foundation), so dalvikvm starts
# on Windows editions without them.
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/devices/devices_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/location_win.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/sensors_jni.cc
  ${CMAKE_SOURCE_DIR}/compat/devices/winrt.cc)
target_include_directories(framework_runtime PRIVATE ${CMAKE_SOURCE_DIR}/compat/devices)
target_link_libraries(framework_runtime PUBLIC ole32)
