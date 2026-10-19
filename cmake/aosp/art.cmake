# ART modules, mirroring art/*/Android.bp and the flags art/build/art.go adds
# for host builds.

set(_art "${AOSP_DIR}/art")

# art_defaults + art.go globalFlags/hostFlags (Windows-applicable subset).
add_library(art_defaults INTERFACE)
target_compile_options(art_defaults INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
  -Wall -Wextra -Wstrict-aliasing -fstrict-aliasing -Wunreachable-code
  -Wredundant-decls -Wshadow -Wunused -Wimplicit-fallthrough -Wfloat-equal
  -Wint-to-void-pointer-cast -Wused-but-marked-unused -Wdeprecated
  -Wunreachable-code-break -Wunreachable-code-return -Wno-invalid-offsetof
  -Winconsistent-missing-override -Wno-thread-safety
  -fno-omit-frame-pointer -msse4.2 -mpopcnt)
target_compile_definitions(art_defaults INTERFACE
  _LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS
  ART_DEFAULT_GC_TYPE_IS_CMC ART_USE_GENERATIONAL_GC=1 ART_USE_TLAB=1
  ART_STACK_OVERFLOW_GAP_arm=8192 ART_STACK_OVERFLOW_GAP_arm64=8192
  ART_STACK_OVERFLOW_GAP_riscv64=8192 ART_STACK_OVERFLOW_GAP_x86=8192
  ART_STACK_OVERFLOW_GAP_x86_64=8192
  ART_PAGE_SIZE_AGNOSTIC=1 USE_D8_DESUGAR=1
  ART_FRAME_SIZE_LIMIT=1744
  ART_BASE_ADDRESS=0x60000000
  "ART_BASE_ADDRESS_MIN_DELTA=(-0x1000000)" ART_BASE_ADDRESS_MAX_DELTA=0x1000000)

# generate_operator_out.py: operator<< for enums, as Soong's gensrcs does.
function(art_operator_srcs out local_path)
  set(headers)
  foreach(h IN LISTS ARGN)
    list(APPEND headers "${AOSP_DIR}/${local_path}/${h}")
  endforeach()
  set(file "${CMAKE_BINARY_DIR}/gen/${local_path}/operator_out.cc")
  add_custom_command(
    OUTPUT "${file}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/gen/${local_path}"
    COMMAND ${Python3_EXECUTABLE} art/tools/generate_operator_out.py "${AOSP_DIR}/${local_path}" ${headers} > "${file}"
    WORKING_DIRECTORY "${AOSP_DIR}"
    DEPENDS "${AOSP_DIR}/art/tools/generate_operator_out.py" ${headers}
    VERBATIM)
  set(${out} "${file}" PARENT_SCOPE)
endfunction()

# --- libartpalette (fake palette for hosts) ---------------------------------
add_library(artpalette STATIC ${_art}/libartpalette/system/palette_fake.cc)
target_include_directories(artpalette PUBLIC
  ${_art}/libartpalette/include ${AOSP_DIR}/libnativehelper/include_jni)
target_link_libraries(artpalette PUBLIC base log art_defaults)

# --- libartbase -------------------------------------------------------------
art_operator_srcs(artbase_ops art/libartbase
  arch/instruction_set.h base/allocator.h base/unix_file/fd_file.h)
set(_ab "${_art}/libartbase")
add_library(artbase STATIC
  ${_ab}/arch/instruction_set.cc ${_ab}/base/allocator.cc
  ${_ab}/base/arena_allocator.cc ${_ab}/base/arena_bit_vector.cc
  ${_ab}/base/bit_vector.cc ${_ab}/base/compiler_filter.cc
  ${_ab}/base/file_magic.cc ${_ab}/base/file_utils.cc ${_ab}/base/flags.cc
  ${_ab}/base/hex_dump.cc ${_ab}/base/logging.cc
  ${_ab}/base/calloc_arena_pool.cc ${_ab}/base/membarrier.cc
  ${_ab}/base/memfd.cc ${_ab}/base/memory_region.cc ${_ab}/base/mem_map.cc
  ${_ab}/base/metrics/metrics_common.cc ${_ab}/base/os_linux.cc
  ${_ab}/base/pointer_size.cc ${_ab}/base/runtime_debug.cc
  ${_ab}/base/scoped_arena_allocator.cc ${_ab}/base/scoped_flock.cc
  ${_ab}/base/socket_peer_is_trusted.cc ${_ab}/base/time_utils.cc
  ${_ab}/base/unix_file/fd_file.cc
  ${_ab}/base/unix_file/random_access_file_utils.cc ${_ab}/base/utils.cc
  ${_ab}/base/zip_archive.cc
  # Gesher: the Unix implementation, on top of gesher-base's mmap
  ${_ab}/base/mem_map_unix.cc
  ${artbase_ops})
target_include_directories(artbase PUBLIC ${_ab})
target_link_libraries(artbase PUBLIC art_defaults ziparchive z log artpalette base tinyxml2 gesher_posix)

# --- libdexfile -------------------------------------------------------------
art_operator_srcs(dexfile_ops art/libdexfile
  dex/dex_file.h dex/dex_file_layout.h dex/dex_instruction.h
  dex/dex_instruction_utils.h dex/invoke_type.h)
set(_dx "${_art}/libdexfile")
add_library(dexfile STATIC
  ${_dx}/dex/art_dex_file_loader.cc ${_dx}/dex/descriptors_names.cc
  ${_dx}/dex/dex_file.cc ${_dx}/dex/dex_file_exception_helpers.cc
  ${_dx}/dex/dex_file_layout.cc ${_dx}/dex/dex_file_loader.cc
  ${_dx}/dex/dex_file_tracking_registrar.cc ${_dx}/dex/dex_file_verifier.cc
  ${_dx}/dex/dex_instruction.cc ${_dx}/dex/modifiers.cc ${_dx}/dex/primitive.cc
  ${_dx}/dex/signature.cc ${_dx}/dex/standard_dex_file.cc
  ${_dx}/dex/type_lookup_table.cc ${_dx}/dex/utf.cc
  ${dexfile_ops})
target_include_directories(dexfile PUBLIC ${_dx} ${_dx}/external/include)
target_compile_definitions(dexfile PUBLIC STATIC_LIB)
target_link_libraries(dexfile PUBLIC artbase)

# --- dexdump ----------------------------------------------------------------
set(_dd "${_art}/dexdump")
add_executable(dexdump
  ${_dd}/dexdump_cfg.cc ${_dd}/dexdump_main.cc ${_dd}/dexdump.cc)
target_link_libraries(dexdump PRIVATE dexfile)
