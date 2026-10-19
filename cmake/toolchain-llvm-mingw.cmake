# Cross toolchain: llvm-mingw targeting x86_64 Windows (UCRT).
# Works from a Linux or Windows host. Point LLVM_MINGW at the unpacked
# release, or put its bin/ directory on PATH.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_triple x86_64-w64-mingw32)
if(DEFINED ENV{LLVM_MINGW} AND NOT LLVM_MINGW)
  set(LLVM_MINGW "$ENV{LLVM_MINGW}")
endif()
if(LLVM_MINGW)
  set(_bin "${LLVM_MINGW}/bin/")
endif()

if(CMAKE_HOST_WIN32)
  set(_exe ".exe")
endif()

set(CMAKE_C_COMPILER "${_bin}${_triple}-clang${_exe}")
set(CMAKE_CXX_COMPILER "${_bin}${_triple}-clang++${_exe}")
set(CMAKE_RC_COMPILER "${_bin}${_triple}-windres${_exe}")
set(CMAKE_AR "${_bin}llvm-ar${_exe}")
set(CMAKE_RANLIB "${_bin}llvm-ranlib${_exe}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
