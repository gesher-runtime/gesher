cmake_minimum_required(VERSION 3.24)
project(gesher_binder_protected_peers LANGUAGES CXX)

include(CTest)
add_executable(binder_protected_peers_test
  ../../compat/binder/protected_peers.cc
  ../../tests/native/binder_protected_peers_test.cc)
target_compile_features(binder_protected_peers_test PRIVATE cxx_std_20)
target_compile_definitions(binder_protected_peers_test PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX GESHER_BINDER_PEER_FIXTURE)
target_link_libraries(binder_protected_peers_test PRIVATE advapi32)
if(MINGW)
  target_compile_options(binder_protected_peers_test PRIVATE -Wall -Wextra -Werror)
  target_link_options(binder_protected_peers_test PRIVATE -static -municode)
endif()
add_test(NAME binder_protected_peers COMMAND binder_protected_peers_test)
set_tests_properties(binder_protected_peers PROPERTIES TIMEOUT 15)

add_executable(binder_protected_bootstrap_test
  ../../compat/binder/protected_bootstrap.cc
  ../../compat/binder/protected_peers.cc
  ../../tests/native/binder_protected_bootstrap_test.cc)
target_compile_features(binder_protected_bootstrap_test PRIVATE cxx_std_20)
target_compile_definitions(binder_protected_bootstrap_test PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX GESHER_BINDER_BOOTSTRAP_FIXTURE)
target_link_libraries(binder_protected_bootstrap_test PRIVATE advapi32 ole32 shell32 uuid)
if(MINGW)
  target_compile_options(binder_protected_bootstrap_test PRIVATE -Wall -Wextra -Werror)
  target_link_options(binder_protected_bootstrap_test PRIVATE -static)
endif()
add_test(NAME binder_protected_bootstrap COMMAND binder_protected_bootstrap_test)
set_tests_properties(binder_protected_bootstrap PROPERTIES TIMEOUT 15)

add_executable(binder_protected_data_test
  ../../compat/binder/protected_data_client.cc
  ../../compat/framework/app_container.cc
  ../../compat/posix/src/binder_transport.cc
  ../../compat/usb_storage/binder_listener_factory.cc
  ../../tests/native/binder_listener_factory_test.cc)
target_compile_features(binder_protected_data_test PRIVATE cxx_std_20)
target_compile_definitions(binder_protected_data_test PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX GESHER_BINDER_DATA_CLIENT_FIXTURE
  GESHER_BINDER_LISTENER_FACTORY_FIXTURE)
target_link_libraries(binder_protected_data_test PRIVATE advapi32 bcrypt ole32)
if(MINGW)
  target_compile_options(binder_protected_data_test PRIVATE -Wall -Wextra -Werror)
  target_link_options(binder_protected_data_test PRIVATE -static -municode)
endif()
add_test(NAME binder_protected_data COMMAND binder_protected_data_test)
set_tests_properties(binder_protected_data PROPERTIES TIMEOUT 20)
