# dex2oat (art/dex2oat/Android.bp, host variant) and the libraries it is made
# of: libart-compiler (art/compiler/Android.bp, the x86 and x86-64 code generators)
# and libart-dex2oat. Built for Windows like dalvikvm, on the same libart, so
# Gesher's boot image comes from the runtime that loads it (docs/boot-image.md,
# ADR 0031). dalvikvm links libart-compiler too, for the JIT
# (docs/compiled-code.md, ADR 0033).

set(_cc "${_art}/compiler")
set(_d2o "${_art}/dex2oat")

# --- libart-compiler --------------------------------------------------------
art_operator_srcs(compiler_ops art/compiler
  linker/linker_patch.h optimizing/graph.h optimizing/locations.h optimizing/nodes.h
  optimizing/optimizing_compiler_stats.h utils/arm/constants_arm.h)
set(_compiler_srcs
  # libart-compiler-defaults: srcs
  debug/elf_debug_writer.cc dex/inline_method_analyser.cc
  driver/assume_value_options.cc driver/compiler_options.cc
  driver/dex_compilation_unit.cc jit/jit_compiler.cc jit/jit_logger.cc
  jni/quick/calling_convention.cc jni/quick/jni_compiler.cc
  optimizing/block_builder.cc optimizing/block_namer.cc
  optimizing/bounds_check_elimination.cc optimizing/builder.cc
  optimizing/cha_guard_optimization.cc optimizing/code_generation_data.cc
  optimizing/code_generator.cc optimizing/code_generator_utils.cc
  optimizing/code_sinking.cc optimizing/constant_folding.cc
  optimizing/constructor_fence_redundancy_elimination.cc
  optimizing/control_flow_simplifier.cc optimizing/data_type.cc
  optimizing/dead_code_elimination.cc optimizing/escape.cc optimizing/graph.cc
  optimizing/graph_checker.cc optimizing/graph_visualizer.cc optimizing/gvn.cc
  optimizing/handle_cache.cc optimizing/induction_var_analysis.cc
  optimizing/induction_var_range.cc optimizing/inliner.cc
  optimizing/instruction_builder.cc optimizing/instruction_list.cc
  optimizing/instruction_simplifier.cc optimizing/intrinsic_objects.cc
  optimizing/intrinsics.cc optimizing/licm.cc optimizing/linear_order.cc
  optimizing/load_store_analysis.cc optimizing/load_store_elimination.cc
  optimizing/locations.cc optimizing/loop_analysis.cc
  optimizing/loop_information.cc optimizing/loop_optimization.cc
  optimizing/nodes.cc optimizing/nodes_vector.cc optimizing/optimization.cc
  optimizing/optimizing_compiler.cc optimizing/parallel_move_resolver.cc
  optimizing/prepare_for_register_allocation.cc
  optimizing/profiling_info_builder.cc optimizing/reference_type_info.cc
  optimizing/reference_type_propagation.cc
  optimizing/register_allocation_resolver.cc optimizing/register_allocator.cc
  optimizing/register_allocator_linear_scan.cc optimizing/scheduler.cc
  optimizing/sharpening.cc optimizing/side_effects_analysis.cc
  optimizing/ssa_builder.cc optimizing/ssa_liveness_analysis.cc
  optimizing/ssa_phi_elimination.cc optimizing/stack_map_stream.cc
  optimizing/superblock_cloner.cc optimizing/write_barrier_elimination.cc
  trampolines/trampoline_compiler.cc utils/assembler.cc
  utils/jni_macro_assembler.cc compiler.cc
  # codegen: x86 and x86_64 (Soong builds the 32-bit variant into the 64-bit
  # one; the x86-64 code generator shares its nodes and simplifiers)
  jni/quick/x86/calling_convention_x86.cc optimizing/code_generator_x86.cc
  optimizing/code_generator_vector_x86.cc optimizing/intrinsics_x86.cc
  optimizing/instruction_simplifier_x86_shared.cc
  optimizing/instruction_simplifier_x86.cc optimizing/pc_relative_fixups_x86.cc
  optimizing/x86_memory_gen.cc utils/x86/assembler_x86.cc
  utils/x86/jni_macro_assembler_x86.cc utils/x86/managed_register_x86.cc
  jni/quick/x86_64/calling_convention_x86_64.cc
  optimizing/intrinsics_x86_64.cc optimizing/instruction_simplifier_x86_64.cc
  optimizing/code_generator_x86_64.cc optimizing/code_generator_vector_x86_64.cc
  utils/x86_64/assembler_x86_64.cc utils/x86_64/jni_macro_assembler_x86_64.cc
  utils/x86_64/managed_register_x86_64.cc)
list(TRANSFORM _compiler_srcs PREPEND "${_cc}/")
add_library(art_compiler STATIC ${_compiler_srcs} ${compiler_ops})
target_include_directories(art_compiler PUBLIC ${_cc} ${_art}/disassembler)
target_compile_definitions(art_compiler PUBLIC ART_ENABLE_CODEGEN_x86 ART_ENABLE_CODEGEN_x86_64)
target_link_libraries(art_compiler PUBLIC art_runtime)
add_dependencies(art_compiler art_asm_support_headers)

# The JIT: the runtime's jit_create() is libart-compiler's (jit/jit_compiler.cc).
target_link_libraries(dalvikvm PRIVATE art_compiler)
# libart-compiler's graph visualizer asks for a disassembler (dex2oat's stand-in).
target_sources(dalvikvm PRIVATE ${CMAKE_SOURCE_DIR}/compat/art/disassembler_standin.cc)

# --- libart-dex2oat ---------------------------------------------------------
art_operator_srcs(dex2oat_ops art/dex2oat linker/image_writer.h)
set(_dex2oat_srcs
  aot_class_linker.cc dex/quick_compiler_callbacks.cc dex/verification_results.cc
  driver/compiled_method.cc driver/compiled_method_storage.cc
  driver/compiler_driver.cc interpreter/interpreter_switch_impl1.cc
  linker/code_info_table_deduper.cc linker/elf_writer_quick.cc
  linker/image_writer.cc linker/multi_oat_relative_patcher.cc
  linker/oat_writer.cc linker/relative_patcher.cc sdk_checker.cc transaction.cc
  utils/swap_space.cc
  # codegen: x86 and x86_64
  linker/x86/relative_patcher_x86.cc linker/x86/relative_patcher_x86_base.cc
  linker/x86_64/relative_patcher_x86_64.cc)
list(TRANSFORM _dex2oat_srcs PREPEND "${_d2o}/")
add_library(art_dex2oat STATIC ${_dex2oat_srcs} ${dex2oat_ops})
# <openssl/sha.h>: the build ID's SHA-1 (libcrypto_static in Soong), over BCrypt.
target_include_directories(art_dex2oat PUBLIC ${_d2o} ${CMAKE_SOURCE_DIR}/compat/art/include)
target_link_libraries(art_dex2oat PUBLIC art_compiler)

# --- dex2oat ----------------------------------------------------------------
add_executable(dex2oat ${_d2o}/dex2oat.cc ${_d2o}/dex2oat_options.cc
  ${CMAKE_SOURCE_DIR}/compat/art/sha1_bcrypt.cc
  ${CMAKE_SOURCE_DIR}/compat/art/disassembler_standin.cc)
target_link_libraries(dex2oat PRIVATE art_dex2oat bcrypt)
# A linker map, for tools/symbolize.py (the executable is stripped).
target_link_options(dex2oat PRIVATE -Wl,-Map,${CMAKE_BINARY_DIR}/dex2oat.map)

# --- profman ----------------------------------------------------------------
# art/profman/Android.bp: profiles from text (the boot image profile,
# tools/boot-image/compile.ps1) and the merge of an app's JIT profile into its
# reference profile before a speed-profile compile (com.gesher.server.Dexopt;
# docs/compiled-code.md, ADR 0054). libprofile, libdexfile and libartbase only.
set(_pm "${_art}/profman")
add_executable(profman ${_pm}/profman.cc ${_pm}/boot_image_profile.cc
  ${_pm}/profile_assistant.cc ${_pm}/inline_cache_format_util.cc)
target_include_directories(profman PRIVATE ${_pm} ${_pm}/include)
target_link_libraries(profman PRIVATE art_profile)

# --- oatdump ---------------------------------------------------------------
# The pinned host oatdump uses ART's real disassembler and inspects the same
# oat/vdex/image formats as this runtime (art/oatdump and disassembler/Android.bp).
add_library(art_disassembler STATIC
  ${_art}/disassembler/disassembler.cc ${_art}/disassembler/disassembler_x86.cc)

target_include_directories(art_disassembler PUBLIC ${_art}/disassembler)
target_compile_definitions(art_disassembler PRIVATE ART_ENABLE_CODEGEN_x86 ART_ENABLE_CODEGEN_x86_64)
target_link_libraries(art_disassembler PUBLIC art_runtime)

add_executable(oatdump ${_art}/oatdump/oatdump.cc)
target_link_libraries(oatdump PRIVATE art_compiler art_disassembler)
target_link_options(oatdump PRIVATE -Wl,-Map,${CMAKE_BINARY_DIR}/oatdump.map)
