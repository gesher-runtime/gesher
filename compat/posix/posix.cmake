# gesher-base: the Linux/POSIX interfaces AOSP code expects, on Win32.
# Headers in include/ shadow or extend mingw's; implementations in src/.
add_library(gesher_posix STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/crash.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/dirent.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/dlfcn.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/internal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/mman.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/pthread.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/signal.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/syscall.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/sysv_shims.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/system.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/thread_register.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/unistd.cc)
target_link_libraries(gesher_posix PUBLIC synchronization psapi)
# Pull in the crash reporter (src/crash.cc), which nothing calls.
target_link_options(gesher_posix INTERFACE -Wl,-u,gesher_crash_reporter_anchor)
target_include_directories(gesher_posix BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
# Every AOSP module sees the Linux-like environment, and links its implementation.
include_directories(BEFORE ${CMAKE_CURRENT_LIST_DIR}/include)
link_libraries(gesher_posix)
target_compile_options(gesher_posix PRIVATE -Wall -Wextra -Werror)
