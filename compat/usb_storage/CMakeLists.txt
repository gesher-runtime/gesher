add_executable(gesher-usb-authority usb_authority_service.cc)
set_target_properties(gesher-usb-authority PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_options(gesher-usb-authority PRIVATE -Wall -Wextra -Werror)
target_link_options(gesher-usb-authority PRIVATE -municode)
target_link_libraries(gesher-usb-authority PRIVATE advapi32 ole32 shell32 uuid)
