# Libraries whose source lists come from their Android.bp, read at configure
# time by tools/gen/bp_cc.py (one variant: the given Soong targets, x86_64).
#
#   gesher_bp_cc(<prefix> <dir under AOSP_DIR> <module>
#                [TARGETS host,windows] [EXCLUDE <regex>...])
#     sets <prefix>_SRCS, _CFLAGS, _CONLYFLAGS, _CPPFLAGS, _INCLUDES,
#     _EXPORT_INCLUDES and _UNRESOLVED in the caller's scope.
#
#   gesher_bp_library(<target> <prefix> [NO_WERROR] [QUIET])
#     a static library of <prefix>'s sources, flags and include directories
#     (exported ones PUBLIC). NO_WERROR drops -Werror; QUIET adds -w, for
#     third-party code built as its project builds it.

function(gesher_bp_cc prefix dir module)
  cmake_parse_arguments(A "" "TARGETS" "EXCLUDE" ${ARGN})
  if(NOT A_TARGETS)
    set(A_TARGETS host,windows)
  endif()
  set(_args)
  foreach(_x ${A_EXCLUDE})
    list(APPEND _args --exclude "${_x}")
  endforeach()
  set(_out "${CMAKE_BINARY_DIR}/gen/bp/${prefix}.cmake")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/gen/bp")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/gen/bp_cc.py"
            "${AOSP_DIR}" "${dir}" "${module}" "${prefix}" "${_out}"
            --targets "${A_TARGETS}" ${_args}
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "bp_cc.py failed for ${dir}:${module}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${AOSP_DIR}/${dir}/Android.bp" "${CMAKE_SOURCE_DIR}/tools/gen/bp_cc.py")
  include("${_out}")
  foreach(_v SRCS CFLAGS CONLYFLAGS CPPFLAGS INCLUDES EXPORT_INCLUDES UNRESOLVED)
    set(${prefix}_${_v} "${${prefix}_${_v}}" PARENT_SCOPE)
  endforeach()
endfunction()

function(gesher_bp_library target prefix)
  cmake_parse_arguments(A "NO_WERROR;QUIET" "" "" ${ARGN})
  add_library(${target} STATIC ${${prefix}_SRCS})
  set(_c ${${prefix}_CFLAGS})
  set(_conly ${${prefix}_CONLYFLAGS})
  set(_cpp ${${prefix}_CPPFLAGS})
  if(A_NO_WERROR OR A_QUIET)
    list(REMOVE_ITEM _c -Werror)
    list(REMOVE_ITEM _conly -Werror)
    list(REMOVE_ITEM _cpp -Werror)
  endif()
  # Flags Soong's toolchain sets per language (-std=gnu89 for libpng, ...).
  set(_opts)
  foreach(_f ${_c})
    if(_f MATCHES " ")
      # "-Xclang +avx2": one option of two words, kept together.
      list(APPEND _opts "SHELL:${_f}")
    else()
      list(APPEND _opts "$<$<COMPILE_LANGUAGE:C,CXX>:${_f}>")
    endif()
  endforeach()
  foreach(_f ${_conly})
    list(APPEND _opts "$<$<COMPILE_LANGUAGE:C>:${_f}>")
  endforeach()
  foreach(_f ${_cpp})
    list(APPEND _opts "$<$<COMPILE_LANGUAGE:CXX>:${_f}>")
  endforeach()
  if(A_QUIET)
    list(APPEND _opts -w)
  endif()
  target_compile_options(${target} PRIVATE ${_opts})
  set(_priv ${${prefix}_INCLUDES})
  list(REMOVE_ITEM _priv ${${prefix}_EXPORT_INCLUDES})
  target_include_directories(${target} PRIVATE ${_priv} PUBLIC ${${prefix}_EXPORT_INCLUDES})
endfunction()
