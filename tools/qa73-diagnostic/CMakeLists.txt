cmake_minimum_required(VERSION 3.24)
project(qa73_native_diagnostic LANGUAGES C CXX ASM)
get_filename_component(GESHER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(NOT WIN32)
  message(FATAL_ERROR "Use the checked-in LLVM-MinGW toolchain")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Compile the original component, including all actual POSIX providers, source
# flags, OFD/import anchors and real MinGW scanf parsers, without editing it.
add_subdirectory("${GESHER_ROOT}/tools/pipe-component" component)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/diagnostic.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/diagnostic.py"
  generate --out "${CMAKE_BINARY_DIR}/generated" COMMAND_ERROR_IS_FATAL ANY)

function(production_flags target)
  target_compile_definitions(${target} PRIVATE GESHER USE_MINGW WIN32_LEAN_AND_MEAN
    NOMINMAX NOGDI _USE_MATH_DEFINES __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS
    __STDC_LIMIT_MACROS _WIN32_WINNT=0x0A00 WINVER=0x0A00 _FILE_OFFSET_BITS=64
    _POSIX_THREAD_SAFE_FUNCTIONS)
  target_compile_options(${target} PRIVATE -mno-ms-bitfields -fno-exceptions
    -fsized-deallocation -Wall -Wextra -Werror)
  target_link_options(${target} PRIVATE -static)
endfunction()

add_executable(ofd_spawn_original "${GESHER_ROOT}/tests/exec/ofd-spawn-test.cc")
add_executable(ofd_spawn_observed "${CMAKE_BINARY_DIR}/generated/ofd-spawn-test.cc")
add_executable(stdio_ofd_observed "${CMAKE_BINARY_DIR}/generated/stdio_ofd_test.cc")
add_executable(native_child_observed "${CMAKE_BINARY_DIR}/generated/ofd-native-child.cc")
add_executable(puzzles_pipe_probe puzzles_pipe_probe.cc)
foreach(target ofd_spawn_original ofd_spawn_observed puzzles_pipe_probe)
  production_flags(${target})
  target_link_libraries(${target} PRIVATE gesher_exec_spawn)
endforeach()
production_flags(stdio_ofd_observed)
target_compile_options(stdio_ofd_observed PRIVATE -fno-builtin)
target_include_directories(stdio_ofd_observed PRIVATE "${GESHER_ROOT}/tests/native")
target_link_libraries(stdio_ofd_observed PRIVATE gesher_posix)
# This remains an ordinary UCRT PE: no Gesher POSIX providers or importer.
production_flags(native_child_observed)

add_executable(binder_observer binder_observer.cc)
target_compile_definitions(binder_observer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(binder_observer PRIVATE -Wall -Wextra -Werror)
target_link_options(binder_observer PRIVATE -static -municode)
target_link_libraries(binder_observer PRIVATE dbghelp)
