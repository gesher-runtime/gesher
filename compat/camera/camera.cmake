# Gesher (camera subsystem): Media Foundation webcam capture and Camera2 delivery
target_sources(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/camera/mf_camera.cc)
target_include_directories(framework_runtime PRIVATE
  ${CMAKE_SOURCE_DIR}/compat/camera)
