# A small host PE JNI library, independent of the monolithic Android native runtime.
# credentials.cmake resolves the host JDK headers and its managed-entry ABI header.
add_library(gesher_install_pipe SHARED "${CMAKE_CURRENT_LIST_DIR}/install_pipe.c")
target_include_directories(gesher_install_pipe PRIVATE
  "${CMAKE_SOURCE_DIR}/compat/credentials/include" "${GESHER_CREDENTIAL_JNI}")
target_link_libraries(gesher_install_pipe PRIVATE advapi32 bcrypt shell32 kernel32)
set_target_properties(gesher_install_pipe PROPERTIES
  PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "gesher_install_pipe")
target_compile_options(gesher_install_pipe PRIVATE -Wall -Wextra -Werror)
add_dependencies(dalvikvm gesher_install_pipe)

add_executable(install_pipe_test EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/tests/native/install_pipe_test.c")
target_include_directories(install_pipe_test PRIVATE
  "${CMAKE_SOURCE_DIR}/compat/credentials/include" "${GESHER_CREDENTIAL_JNI}")
target_link_libraries(install_pipe_test PRIVATE advapi32 bcrypt shell32 kernel32)
target_compile_options(install_pipe_test PRIVATE -Wall -Wextra -Werror)
