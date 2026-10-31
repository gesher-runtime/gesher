# Explicit SOURCE qualification targets only; never ALL or production hooks.
set(_file_mremap_root "${PROJECT_SOURCE_DIR}")
add_executable(file_mremap_lifetime EXCLUDE_FROM_ALL lifetime_cases.c)
add_executable(file_mremap_alias EXCLUDE_FROM_ALL shared_alias.c)
foreach(_target file_mremap_lifetime file_mremap_alias)
  target_compile_options(${_target} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${_target} PRIVATE gesher_posix)
  set_target_properties(${_target} PROPERTIES LINKER_LANGUAGE CXX)
endforeach()
add_executable(file_mremap_prepare EXCLUDE_FROM_ALL alias_prepare_test.cc
  alias_prepare_noexcept_caller.cc "${_file_mremap_root}/compat/posix/src/mman_alias_prepare.cc")
# Test-only sources occur in no production target. Helper's existing source-local
# -fexceptions option remains identical to its production helper compilation.
set_source_files_properties(alias_prepare_test.cc PROPERTIES COMPILE_OPTIONS "-fexceptions")
set_source_files_properties(alias_prepare_noexcept_caller.cc PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_compile_options(file_mremap_prepare PRIVATE -Wall -Wextra -Werror)
# This TU is test-only; throwing allocator faults are caught inside the helper.
# Never apply this source property or a target-wide exception flag to mman.cc.
set_source_files_properties(alias_failure_adapter.cc PROPERTIES COMPILE_OPTIONS "-fexceptions")
add_library(file_mremap_failure_objects OBJECT
  "${_file_mremap_root}/compat/posix/src/mman.cc" alias_failure_adapter.cc)
set_target_properties(file_mremap_failure_objects PROPERTIES EXCLUDE_FROM_ALL TRUE)
# TARGET-SCOPED: never source-global properties on shared production mman.cc.
target_compile_definitions(file_mremap_failure_objects PRIVATE GESHER_FILE_MREMAP_FAILURE_TEST=1)
target_compile_options(file_mremap_failure_objects PRIVATE -Wall -Wextra -Werror)
foreach(_name alias_failure_test lifetime_cases)
  add_executable(file_mremap_${_name}_failure EXCLUDE_FROM_ALL ${_name}.c
    $<TARGET_OBJECTS:file_mremap_failure_objects>)
  target_compile_definitions(file_mremap_${_name}_failure PRIVATE GESHER_FILE_MREMAP_FAILURE_TEST=1)
  target_compile_options(file_mremap_${_name}_failure PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(file_mremap_${_name}_failure PRIVATE gesher_posix)
  set_target_properties(file_mremap_${_name}_failure PROPERTIES LINKER_LANGUAGE CXX)
endforeach()
