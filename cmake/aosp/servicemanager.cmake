# Binder across processes on Windows (docs/decisions/0015-services-binder-ipc.md,
# docs/services.md): binderd, the broker that plays the binder driver for
# gesher-base's /dev/binder (compat/posix/src/binder.cc); frameworks/native's
# servicemanager; and their probes, run in CI on Windows.

set(_uapi "${AOSP_DIR}/bionic/libc/kernel/uapi")

# binderd: Win32 only, with the driver's ABI from bionic's uapi header,
# searched after gesher-base's <linux/*.h>.
add_executable(binderd ${CMAKE_SOURCE_DIR}/compat/binder/binderd.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/brokered_network.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/loopback_relay.cc
  ${CMAKE_SOURCE_DIR}/compat/posix/src/rooted_storage.cc
  ${CMAKE_SOURCE_DIR}/compat/posix/src/internal.cc
  ${CMAKE_SOURCE_DIR}/compat/posix/src/binder_endpoint.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_bootstrap.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_peers.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_shared_memory.cc
  ${CMAKE_SOURCE_DIR}/compat/framework/app_container.cc)
target_compile_definitions(binderd PRIVATE GESHER_PROTECTED_BINDER)
set_source_files_properties(
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_bootstrap.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_peers.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_shared_memory.cc
  PROPERTIES COMPILE_OPTIONS "-fexceptions")
target_compile_definitions(binderd PRIVATE GESHER_RECORD_LOCK_BROKER=1)
target_compile_options(binderd PRIVATE -Wall -Wextra -Werror "SHELL:-idirafter ${_uapi}")
# The descriptor broker's tokens (ADR 0088) come from BCryptGenRandom; the
# sandboxed pipe's security descriptor (ADR 0090) from app_container.cc.
# Resolve ordinary imports before mincore's umbrella API-set library. Only
# precise interrupt-time APIs require mincore; keep the broker's explicit
# system-DLL contract identical to the rest of gesher_posix.
target_link_libraries(binderd PRIVATE bcrypt advapi32 ole32 shell32 uuid kernel32 powrprof ws2_32 mincore)

add_executable(binder_protected_admission_test EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/native/binder_protected_admission_test.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/brokered_network.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/loopback_relay.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_bootstrap.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_peers.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/protected_shared_memory.cc
  ${CMAKE_SOURCE_DIR}/compat/framework/app_container.cc)
target_compile_definitions(binder_protected_admission_test PRIVATE
  GESHER_PROTECTED_BINDER GESHER_BINDER_PEER_FIXTURE)
target_compile_options(binder_protected_admission_test PRIVATE -Wall -Wextra -Werror
  "SHELL:-idirafter ${_uapi}")
target_link_libraries(binder_protected_admission_test PRIVATE bcrypt advapi32 ole32 shell32 uuid ws2_32)

# The production relay retains its TCP endpoint and exchanges data over a real
# named pipe. This transport fixture is separate from actual ART integration.
add_executable(loopback_relay_test EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/native/loopback_relay_test.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/loopback_relay.cc)
set_target_properties(loopback_relay_test PROPERTIES LINK_LIBRARIES "ws2_32")
target_compile_options(loopback_relay_test PRIVATE -Wall -Wextra -Werror)

add_executable(loopback_relay_frames_test EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/native/loopback_relay_frames_test.cc)
set_target_properties(loopback_relay_frames_test PROPERTIES LINK_LIBRARIES "ws2_32")
target_compile_options(loopback_relay_frames_test PRIVATE -Wall -Wextra -Werror)

# Real Windows TLS/FLS exit and the actual native fork exit gate. The fixture
# declares only unused logging/cloned-child boundaries; it never clones ART.
add_executable(relay_worker_exit_test EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/native/relay_worker_exit_test.cc
  ${CMAKE_SOURCE_DIR}/compat/binder/loopback_relay.cc
  ${CMAKE_SOURCE_DIR}/compat/posix/src/fork_locks.cc)
target_include_directories(relay_worker_exit_test PRIVATE ${CMAKE_SOURCE_DIR}/compat/posix/include)
target_compile_definitions(relay_worker_exit_test PRIVATE GESHER_RELAY_EXIT_TEST_BOUNDARIES)
target_compile_options(relay_worker_exit_test PRIVATE -Wall -Wextra -Werror)
set_target_properties(relay_worker_exit_test PROPERTIES LINK_LIBRARIES "ws2_32")

add_executable(canonical_file_name_test EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/native/canonical_file_name_test.cc)
target_include_directories(canonical_file_name_test PRIVATE ${CMAKE_SOURCE_DIR}/compat/posix/include)
target_compile_options(canonical_file_name_test PRIVATE -Wall -Wextra -Werror)
target_link_options(canonical_file_name_test PRIVATE -municode)

# servicemanager, as vndservicemanager is built (no VINTF, no perfetto), with
# libcutils' multiuser.cpp (not in libcutils' Windows set) and a main of its
# own (compat/binder/servicemanager_main.cc).
set(_sm "${AOSP_DIR}/frameworks/native/cmds/servicemanager")
add_executable(servicemanager
  ${_sm}/Access.cpp ${_sm}/ServiceManager.cpp
  ${AOSP_DIR}/system/core/libcutils/multiuser.cpp
  ${CMAKE_SOURCE_DIR}/compat/binder/servicemanager_main.cc)
target_include_directories(servicemanager PRIVATE ${_sm} ${CMAKE_SOURCE_DIR}/compat/binder/include)
target_compile_definitions(servicemanager PRIVATE VENDORSERVICEMANAGER
  ANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION)
target_link_libraries(servicemanager PRIVATE binder)
# libbinder's headers reach cutils/trace.h's <stdatomic.h> (C++23, as libbinder).
set_target_properties(servicemanager PROPERTIES CXX_STANDARD 23)
target_compile_options(servicemanager PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)

# The driver's ABI across processes (raw ioctls), and libbinder with
# servicemanager across processes.
add_executable(binder_probe ${CMAKE_SOURCE_DIR}/probes/binder/binder_probe.cc)
target_compile_options(binder_probe PRIVATE -Wall -Wextra -Werror "SHELL:-idirafter ${_uapi}")

# ADR0068/test88: independent real driver actors, also compiled for Android.
add_executable(binder_sg_actor EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/binder/sg-actor.cc)
target_compile_options(binder_sg_actor PRIVATE -Wall -Wextra -Werror "SHELL:-idirafter ${_uapi}")
add_executable(binder_sg_wire_peer EXCLUDE_FROM_ALL ${CMAKE_SOURCE_DIR}/tests/binder/sg-wire-peer.cc)
target_compile_options(binder_sg_wire_peer PRIVATE -Wall -Wextra -Werror "SHELL:-idirafter ${_uapi}")

add_executable(libbinder_probe ${CMAKE_SOURCE_DIR}/probes/binder/libbinder_probe.cc)
target_link_libraries(libbinder_probe PRIVATE binder)
set_target_properties(libbinder_probe PROPERTIES CXX_STANDARD 23)
target_compile_options(libbinder_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
