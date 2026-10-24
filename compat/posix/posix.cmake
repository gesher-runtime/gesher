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
  ${CMAKE_CURRENT_LIST_DIR}/src/internal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/memalign.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/mman.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/misc.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/netdb.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/path.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/pthread.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/select.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/signal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/sleep.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/socket.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/stat.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/syscall.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/sysv_shims.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/system.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/thread_register.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/unistd.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/users.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/winsock.cc)
target_link_libraries(gesher_posix PUBLIC synchronization psapi ws2_32 iphlpapi bcrypt shell32
  pthread)
# Pull in the crash reporter (src/crash.cc), which nothing calls.
target_link_options(gesher_posix INTERFACE -Wl,-u,gesher_crash_reporter_anchor)
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
