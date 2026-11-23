cmake_minimum_required(VERSION 3.24)
project(gesher_usb_storage LANGUAGES CXX)

add_executable(usb_storage_probe usb_storage_probe.cc
  ../../compat/usb_storage/host_disk.cc)
target_compile_features(usb_storage_probe PRIVATE cxx_std_20)
target_compile_definitions(usb_storage_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(usb_storage_probe PRIVATE cfgmgr32)
if(MINGW)
  target_link_options(usb_storage_probe PRIVATE -municode -static)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(usb_block_channel_test ../../tests/native/usb_block_channel_test.cc
    ../../compat/usb_storage/block_broker_client.cc
    ../../compat/usb_storage/block_channel.cc)
  target_include_directories(usb_block_channel_test PRIVATE ../../compat/usb_storage)
  target_compile_features(usb_block_channel_test PRIVATE cxx_std_20)
  target_compile_definitions(usb_block_channel_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(usb_block_channel_test PRIVATE bcrypt advapi32)
  if(MINGW)
    target_link_options(usb_block_channel_test PRIVATE -static)
  endif()
  add_test(NAME usb_block_channel COMMAND usb_block_channel_test)
  set_tests_properties(usb_block_channel PROPERTIES TIMEOUT 15)
endif()
