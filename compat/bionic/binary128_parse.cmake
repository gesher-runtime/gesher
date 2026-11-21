# Bionic's genuine 113-bit strtold parser, from the existing pinned AOSP
# checkout. Function bodies and original license notices are unchanged.
set(_gesher_gdtoa "${AOSP_DIR}/bionic/libc/upstream-openbsd/lib/libc/gdtoa")
add_library(gesher_bionic_binary128 STATIC
  ${CMAKE_CURRENT_LIST_DIR}/binary128_parse.cc
  ${_gesher_gdtoa}/strtorQ.c
  ${_gesher_gdtoa}/strtodg.c
  ${_gesher_gdtoa}/gethex.c
  ${_gesher_gdtoa}/hexnan.c
  ${_gesher_gdtoa}/hd_init.c
  ${_gesher_gdtoa}/misc.c
  ${_gesher_gdtoa}/smisc.c
  ${_gesher_gdtoa}/gmisc.c
  ${_gesher_gdtoa}/sum.c)
target_include_directories(gesher_bionic_binary128 PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/gdtoa)
# The pinned gdtoa sources use signed wraparound while accumulating mantissa
# bits and large exponents. Preserve those arithmetic assumptions on Windows
# without modifying the upstream parser or other Gesher targets.
target_compile_options(gesher_bionic_binary128 PRIVATE
  -fwrapv -fno-strict-aliasing -frounding-math -ffp-contract=off
  -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter)
