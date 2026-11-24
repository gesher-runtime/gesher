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

add_executable(binder_protected_shared_memory_test
  ../../compat/binder/protected_shared_memory.cc
  ../../compat/binder/protected_peers.cc
  ../../tests/native/binder_protected_shared_memory_test.cc)
target_compile_features(binder_protected_shared_memory_test PRIVATE cxx_std_20)
target_compile_definitions(binder_protected_shared_memory_test PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX GESHER_BINDER_PEER_FIXTURE)
target_link_libraries(binder_protected_shared_memory_test PRIVATE advapi32)
if(MINGW)
  target_compile_options(binder_protected_shared_memory_test PRIVATE -Wall -Wextra -Werror)
  target_link_options(binder_protected_shared_memory_test PRIVATE -static -municode)
endif()
add_test(NAME binder_protected_shared_memory COMMAND binder_protected_shared_memory_test)
set_tests_properties(binder_protected_shared_memory PROPERTIES TIMEOUT 20)

# Both modules compile the same production transport source. Only the executable
# may own sealed startup state; the DLL must observe that exact owner.
foreach(target binder_transport_module_test binder_transport_owner_test)
  if(target STREQUAL "binder_transport_module_test")
    add_library(${target} SHARED ../../tests/native/binder_transport_module_test.cc)
  else()
    add_executable(${target} ../../tests/native/binder_transport_owner_test.cc)
  endif()
  target_sources(${target} PRIVATE
    ../../compat/posix/src/binder_transport.cc
    ../../compat/binder/protected_data_client.cc)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(${target} PRIVATE advapi32)
  if(MINGW)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_link_options(${target} PRIVATE -static)
  endif()
endforeach()
if(MINGW)
  target_link_options(binder_transport_owner_test PRIVATE -municode)
endif()
foreach(mode ordinary protected invalid)
  add_test(NAME binder_transport_owner_${mode}
    COMMAND binder_transport_owner_test $<TARGET_FILE:binder_transport_module_test> ${mode})
  set_tests_properties(binder_transport_owner_${mode} PROPERTIES TIMEOUT 15)
endforeach()
