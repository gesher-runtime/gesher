# Binder across processes on Windows (docs/decisions/0015-services-binder-ipc.md,
# docs/services.md): binderd, the broker that plays the binder driver for
# gesher-base's /dev/binder (compat/posix/src/binder.cc); frameworks/native's
# servicemanager; and their probes, run in CI on Windows.

set(_uapi "${AOSP_DIR}/bionic/libc/kernel/uapi")

# binderd: Win32 only, with the driver's ABI from bionic's uapi header,
# searched after gesher-base's <linux/*.h>.
add_executable(binderd ${CMAKE_SOURCE_DIR}/compat/binder/binderd.cc)
target_compile_options(binderd PRIVATE -Wall -Wextra -Werror "SHELL:-idirafter ${_uapi}")

# Source-only authority fixture. Never part of the ordinary production payload;
# requires an explicit exact-source future CI build and an outer run watchdog.
add_executable(binder_private_admission_fixture EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/windows/binder-private-admission-fixture.cc
  ${CMAKE_SOURCE_DIR}/compat/framework/services_zygote.cc)
target_compile_options(binder_private_admission_fixture PRIVATE -Wall -Wextra -Werror -fexceptions)
target_link_options(binder_private_admission_fixture PRIVATE -municode)

add_executable(installed_guest_policy_fixture EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/windows/installed-guest-policy-fixture.cc)
target_compile_options(installed_guest_policy_fixture PRIVATE -Wall -Wextra -Werror -fexceptions)
target_link_options(installed_guest_policy_fixture PRIVATE -municode)
target_link_libraries(installed_guest_policy_fixture PRIVATE advapi32)

add_executable(binder_launch_registry_fixture EXCLUDE_FROM_ALL
  ${CMAKE_SOURCE_DIR}/tests/windows/binder-launch-registry-fixture.cc)
target_compile_options(binder_launch_registry_fixture PRIVATE -Wall -Wextra -Werror -fexceptions)

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
