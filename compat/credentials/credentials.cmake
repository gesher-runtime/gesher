# OpenJDK headers describe ART's host JNI tables. The custom platform header
# marks only managed native entry points as SysV; JNI_OnLoad stays host ABI.
find_program(_credential_javac javac NO_CMAKE_FIND_ROOT_PATH)
if(_credential_javac)
  get_filename_component(_credential_javac_real "${_credential_javac}" REALPATH)
  get_filename_component(_credential_java_bin "${_credential_javac_real}" DIRECTORY)
  get_filename_component(_credential_java_home "${_credential_java_bin}" DIRECTORY)
endif()
find_path(GESHER_CREDENTIAL_JNI jni.h
  HINTS "$ENV{JAVA_HOME}/include" "${_credential_java_home}/include"
  NO_CMAKE_FIND_ROOT_PATH REQUIRED)
add_library(gesher_account_crypto SHARED "${CMAKE_CURRENT_LIST_DIR}/account_crypto.c")
target_include_directories(gesher_account_crypto PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/include" "${GESHER_CREDENTIAL_JNI}")
target_link_libraries(gesher_account_crypto PRIVATE crypt32 kernel32 shell32 user32)
set_target_properties(gesher_account_crypto PROPERTIES
  PREFIX "lib" SUFFIX ".so" OUTPUT_NAME "gesher_account_crypto")
target_compile_options(gesher_account_crypto PRIVATE -Wall -Wextra -Werror)
add_dependencies(dalvikvm gesher_account_crypto)

add_executable(account_chooser_test EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/tests/native/account_chooser_test.c")
target_link_libraries(account_chooser_test PRIVATE kernel32 user32)
target_compile_options(account_chooser_test PRIVATE -UNOGDI -Wall -Wextra -Werror)
target_link_options(account_chooser_test PRIVATE -municode)

add_executable(window_promotion_test EXCLUDE_FROM_ALL
  "${CMAKE_SOURCE_DIR}/tests/native/window_promotion_test.c")
target_link_libraries(window_promotion_test PRIVATE kernel32 user32)
target_compile_options(window_promotion_test PRIVATE -UNOGDI -Wall -Wextra -Werror)
target_link_options(window_promotion_test PRIVATE -municode)
