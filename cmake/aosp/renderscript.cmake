# Legacy RenderScript (docs/renderscript.md): android.renderscript's natives,
# librs_jni.so, as a builtin library of dalvikvm.
#
# libRS (frameworks/rs) as its RS_COMPATIBILITY_LIB variant builds it, the
# CPU-only runtime the support library ships (support.bp's
# libRSSupport_defaults): the context and its objects (Elements, Types,
# Allocations, Samplers, scripts and script groups), the reference CPU driver
# (driver/rsd*) and cpu_ref's intrinsics in their portable C++ (no NEON, no
# SSSE3 path). Left out, as frameworks/rs patch 0001 makes them fail where
# Android fails: the BLAS intrinsic (libblasV8 and gemmlowp), compiled
# ScriptC kernels (bitcode needs bcc and libclcore, which Gesher does not
# have), USAGE_IO allocations (a Surface producer or consumer) and the
# graphics API (RenderScriptGL). The four rsg sources are generated from
# rs.spec and rs_compat.spec by compat/renderscript/rsg_generator.py.
#
# The JNI is frameworks/base rs/jni/android_renderscript_RenderScript.cpp,
# copied into the build tree with System V tables and Gesher's edits
# (compat/renderscript/gen_jni_source.py); compat/renderscript/renderscript_jni.cc
# registers it as librs_jni.so, the library RenderScript.<clinit> loads.
# The support library's own librsjni.so and libRSSupport.so are an app's ELF
# objects and are not affected; no libRS.so is registered, so the support
# library keeps its compatibility path.
#
# Included after graphics.cmake: the JNI uses hwui's android::graphics::Bitmap.

set(_rs "${AOSP_DIR}/frameworks/rs")
set(_crs "${CMAKE_SOURCE_DIR}/compat/renderscript")
set(_gen_rs "${CMAKE_BINARY_DIR}/gen/renderscript")

# --- The rsg sources (rs_generated_headers_support, rs_generated_sources_support)
set(_rs_gen_outputs)
foreach(_t rsgApiStructs.h rsgApiFuncDecl.h rsgApi.cpp rsgApiReplay.cpp)
  list(APPEND _rs_gen_outputs "${_gen_rs}/${_t}")
  add_custom_command(
    OUTPUT "${_gen_rs}/${_t}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen_rs}"
    COMMAND ${Python3_EXECUTABLE} "${_crs}/rsg_generator.py" "${_rs}/${_t}_rsg" "${_gen_rs}/${_t}"
            "${_rs}/rs.spec" "${_rs}/rs_compat.spec"
    DEPENDS "${_crs}/rsg_generator.py" "${_rs}/${_t}_rsg" "${_rs}/rs.spec" "${_rs}/rs_compat.spec"
    COMMENT "rsg: ${_t}"
    VERBATIM)
endforeach()

# --- libRS, CPU only --------------------------------------------------------------
add_library(renderscript_runtime STATIC
  ${_rs}/rsAllocation.cpp ${_rs}/rsApiAllocation.cpp ${_rs}/rsApiContext.cpp
  ${_rs}/rsApiDevice.cpp ${_rs}/rsApiElement.cpp ${_rs}/rsApiType.cpp ${_rs}/rsClosure.cpp
  ${_rs}/rsComponent.cpp ${_rs}/rsContext.cpp ${_rs}/rsCppUtils.cpp ${_rs}/rsDevice.cpp
  ${_rs}/rsDriverLoader.cpp ${_rs}/rsElement.cpp ${_rs}/rsFifoSocket.cpp ${_rs}/rsObjectBase.cpp
  ${_rs}/rsMatrix2x2.cpp ${_rs}/rsMatrix3x3.cpp ${_rs}/rsMatrix4x4.cpp ${_rs}/rsMutex.cpp
  ${_rs}/rsSampler.cpp ${_rs}/rsScript.cpp ${_rs}/rsScriptC.cpp ${_rs}/rsScriptC_Lib.cpp
  ${_rs}/rsScriptGroup.cpp ${_rs}/rsScriptGroup2.cpp ${_rs}/rsScriptIntrinsic.cpp
  ${_rs}/rsSignal.cpp ${_rs}/rsStream.cpp ${_rs}/rsThreadIO.cpp ${_rs}/rsType.cpp
  ${_rs}/driver/rsdAllocation.cpp ${_rs}/driver/rsdBcc.cpp ${_rs}/driver/rsdCore.cpp
  ${_rs}/driver/rsdElement.cpp ${_rs}/driver/rsdRuntimeStubs.cpp ${_rs}/driver/rsdSampler.cpp
  ${_rs}/driver/rsdScriptGroup.cpp ${_rs}/driver/rsdType.cpp
  ${_rs}/cpu_ref/rsCpuCore.cpp ${_rs}/cpu_ref/rsCpuExecutable.cpp ${_rs}/cpu_ref/rsCpuScript.cpp
  ${_rs}/cpu_ref/rsCpuRuntimeMath.cpp ${_rs}/cpu_ref/rsCpuScriptGroup.cpp
  ${_rs}/cpu_ref/rsCpuScriptGroup2.cpp ${_rs}/cpu_ref/rsCpuIntrinsic.cpp
  ${_rs}/cpu_ref/rsCpuIntrinsic3DLUT.cpp ${_rs}/cpu_ref/rsCpuIntrinsicBlend.cpp
  ${_rs}/cpu_ref/rsCpuIntrinsicBlur.cpp ${_rs}/cpu_ref/rsCpuIntrinsicColorMatrix.cpp
  ${_rs}/cpu_ref/rsCpuIntrinsicConvolve3x3.cpp ${_rs}/cpu_ref/rsCpuIntrinsicConvolve5x5.cpp
  ${_rs}/cpu_ref/rsCpuIntrinsicHistogram.cpp ${_rs}/cpu_ref/rsCpuIntrinsicLUT.cpp
  ${_rs}/cpu_ref/rsCpuIntrinsicResize.cpp ${_rs}/cpu_ref/rsCpuIntrinsicYuvToRGB.cpp
  ${_crs}/rs_compat.cc
  ${_rs_gen_outputs})
# compat/renderscript/include: the little of bionic's <sys/system_properties.h>
# and <android/dlext.h> libRS includes, and gesher_renderscript.h (below).
target_include_directories(renderscript_runtime BEFORE PUBLIC ${_crs}/include)
target_include_directories(renderscript_runtime PUBLIC ${_rs} "${_gen_rs}")
# GLES/gl.h, GLES2/gl2.h: rsdAllocation.h's GL types (the GL driver is not built).
target_include_directories(renderscript_runtime PRIVATE ${AOSP_DIR}/frameworks/native/opengl/include)
target_compile_definitions(renderscript_runtime PUBLIC RS_COMPATIBILITY_LIB)
# include/gesher_renderscript.h: bionic's __DEPRECATED_IN, for libRS's headers
# here and in librs_jni.
target_compile_options(renderscript_runtime PUBLIC
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${_crs}/include/gesher_renderscript.h>")
# graphics_native_headers: android/native_window.h (rsdAllocation.cpp's
# ANativeWindow type; the compatibility variant has no Surface I/O).
target_link_libraries(renderscript_runtime PUBLIC utils cutils log base
  PRIVATE graphics_native_headers bcrypt)
# support.bp's cflags (less -Werror: AOSP code, as framework.cmake builds it).
target_compile_options(renderscript_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter
  -Wno-unused-variable -Wno-overloaded-virtual -Wno-deprecated-declarations
  -Wno-implicit-fallthrough -Wno-cast-function-type-mismatch -Wno-inconsistent-missing-override
  # rsd_cpu.h names pthread_key_t, which bionic's <sys/types.h> brings in.
  "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include pthread.h>")

# --- librs_jni.so ----------------------------------------------------------------------
set(_rs_jni_src rs/jni/android_renderscript_RenderScript.cpp)
add_custom_command(
  OUTPUT "${_gen_rs}/jni/android_renderscript_RenderScript.cpp"
  COMMAND ${Python3_EXECUTABLE} "${_crs}/gen_jni_source.py" "${_gen_rs}/jni"
          "${AOSP_DIR}/frameworks/base" ${_rs_jni_src}
  DEPENDS "${_crs}/gen_jni_source.py" "${CMAKE_SOURCE_DIR}/tools/jni-sysv-patch.py"
          "${AOSP_DIR}/frameworks/base/${_rs_jni_src}"
  COMMENT "System V copy of librs_jni's natives"
  VERBATIM)

# An OBJECT library: its static initializer registers the builtin library.
add_library(gesher_renderscript OBJECT
  "${_gen_rs}/jni/android_renderscript_RenderScript.cpp"
  ${_crs}/renderscript_jni.cc)
target_include_directories(gesher_renderscript PRIVATE
  ${_crs}
  ${_fw}/core/jni ${_fw}/core/jni/include ${CMAKE_SOURCE_DIR}/compat/framework/include
  # android/graphics/bitmap.h (android::graphics::Bitmap).
  ${_fw}/libs/hwui/apex/include
  # gui/GLConsumer.h, which the JNI includes and does not use.
  ${CMAKE_SOURCE_DIR}/compat/opengl/include)
target_link_libraries(gesher_renderscript PUBLIC renderscript_runtime hwui androidfw
  hostgraphics nativehelper utils cutils log base)
gesher_graphics_cxx(gesher_renderscript)
target_compile_options(gesher_renderscript PRIVATE -Wall -Wno-unused-parameter
  -Wno-unused-function -Wno-unused-variable -Wno-error)

target_link_libraries(dalvikvm PRIVATE gesher_renderscript)
