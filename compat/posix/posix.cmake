# gesher-base: the Linux/POSIX interfaces AOSP code expects, on Win32.
# Headers in include/ shadow or extend mingw's; implementations in src/.
add_library(gesher_posix STATIC
  ${CMAKE_CURRENT_LIST_DIR}/src/mman.cc
  ${CMAKE_CURRENT_LIST_DIR}/src/signal.cc)
target_include_directories(gesher_posix BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_options(gesher_posix PRIVATE -Wall -Wextra -Werror)
