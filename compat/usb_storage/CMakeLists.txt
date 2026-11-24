add_executable(gesher-usb-authority
  usb_authority_service.cc
  protected_runtime.rc
  authority_launch.cc
  authority_peer.cc
  binder_authority_control.cc
  binder_listener_factory.cc
  protected_identity.cc
  protected_runtime_session.cc
  protected_signal.cc
  block_session.cc
  block_channel.cc
  block_broker_server.cc
  host_disk.cc
  ${CMAKE_CURRENT_LIST_DIR}/../framework/app_container.cc)
set_target_properties(gesher-usb-authority PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_include_directories(gesher-usb-authority PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(gesher-usb-authority PRIVATE -Wall -Wextra -Werror -fexceptions)
target_link_options(gesher-usb-authority PRIVATE -municode)
target_link_libraries(gesher-usb-authority PRIVATE advapi32 bcrypt ole32 shell32 uuid wtsapi32
  userenv user32 comctl32 setupapi cfgmgr32)

# Full launch validation needs a disposable LocalSystem test host with an
# ordinary desktop user logged on. The default invocation tests refusal.
add_executable(usb_authority_launch_test EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_LIST_DIR}/../../tests/native/usb_authority_launch_test.cc
  ${CMAKE_CURRENT_LIST_DIR}/../framework/app_container.cc
  authority_launch.cc
  authority_peer.cc
  binder_listener_factory.cc
  binder_authority_control.cc)
target_compile_options(usb_authority_launch_test PRIVATE -Wall -Wextra -Werror -fexceptions)
target_link_options(usb_authority_launch_test PRIVATE -municode)
target_link_libraries(usb_authority_launch_test PRIVATE advapi32 bcrypt ole32 wtsapi32)
