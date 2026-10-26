# gesher-base: the Linux/POSIX interfaces AOSP code expects, on Win32.
# Headers in include/ shadow or extend mingw's; implementations in src/.
add_library(gesher_posix STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/binder.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/crash.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/dirent.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/dlfcn.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/epoll.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/errno.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/fault_signals.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/fd.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/fnmatch.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/fsops.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/ftw.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/inotify.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/internal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/libcap.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/links.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/logging.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/memalign.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/mman.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/misc.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/netdb.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/path.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/process.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/procfs.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/pthread.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/select.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/signal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/sleep.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/socket.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/stat.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/syscall.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/sysv_shims.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/system.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/system_properties.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/thread_register.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/unistd.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/users.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/windows_zones.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/winsock.cc)
# Windows time zone ids to IANA ids (src/windows_zones.cc), from the pinned
# ICU's CLDR windowsZones (ADR 0045).
set(_gesher_posix_gen "${CMAKE_BINARY_DIR}/gen/gesher_posix")
set(_windows_zones_txt "${AOSP_DIR}/external/icu/icu4c/source/data/misc/windowsZones.txt")
add_custom_command(
  OUTPUT "${_gesher_posix_gen}/windows_zones_table.inc"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${_gesher_posix_gen}"
  COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_LIST_DIR}/gen_windows_zones.py"
          "${_windows_zones_txt}" "${_gesher_posix_gen}/windows_zones_table.inc"
  DEPENDS "${CMAKE_CURRENT_LIST_DIR}/gen_windows_zones.py" "${_windows_zones_txt}"
  VERBATIM)
target_sources(gesher_posix PRIVATE "${_gesher_posix_gen}/windows_zones_table.inc")
target_include_directories(gesher_posix PRIVATE "${_gesher_posix_gen}")
target_link_libraries(gesher_posix PUBLIC synchronization psapi ws2_32 iphlpapi bcrypt shell32
  pthread)
# Pull in the crash reporter (src/crash.cc), which nothing calls.
target_link_options(gesher_posix INTERFACE -Wl,-u,gesher_crash_reporter_anchor)
# Every process keeps a signal mailbox for kill() from other processes
# (src/process.cc), which nothing calls either.
target_link_options(gesher_posix INTERFACE -Wl,-u,gesher_signal_mailbox_anchor)
# posix_memalign() memory goes to free()/realloc() (src/memalign.cc).
target_link_options(gesher_posix INTERFACE -Wl,--wrap=free -Wl,--wrap=realloc)
target_include_directories(gesher_posix BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
# Every AOSP module sees the Linux-like environment, and links its implementation.
include_directories(BEFORE ${CMAKE_CURRENT_LIST_DIR}/include)
link_libraries(gesher_posix)
target_compile_options(gesher_posix PRIVATE -Wall -Wextra -Werror)
# /dev/binder (src/binder.cc) speaks the kernel driver's ABI, from bionic's
# uapi header; searched last, so gesher-base's <linux/*.h> come first.
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/src/binder.cc PROPERTIES
  COMPILE_OPTIONS "-idirafter;${AOSP_DIR}/bionic/libc/kernel/uapi")
