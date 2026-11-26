// Gesher: android.renderscript's natives for what Gesher's RenderScript does
// not have (docs/renderscript.md). Included by the copy of
// android_renderscript_RenderScript.cpp (compat/renderscript/gen_jni_source.py),
// whose method table names these in place of AOSP's functions.
//
// libRS is built CPU only (cmake/aosp/renderscript.cmake), as Android's
// support library builds it: there is no RenderScript graphics (RenderScriptGL
// and its programs, meshes, fonts and A3D files, which need libRS's OpenGL ES
// driver) and no Surface I/O. RenderScriptGL's context is not created, which
// RenderScriptGL reports as Android does for a context the driver cannot make
// (RSDriverException "Failed to create RS context."); every other graphics or
// Surface native throws RSRuntimeException, so nothing reports an object or a
// frame that does not exist.
#pragma once

#include <jni.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

namespace gesher_rs_unavailable {

inline void ThrowGraphics(JNIEnv* env, const char* what) {
  jniThrowExceptionFmt(env, "android/renderscript/RSRuntimeException",
                       "%s: RenderScript graphics is not supported on this device", what);
}

inline void ThrowSurface(JNIEnv* env, const char* what) {
  jniThrowExceptionFmt(env, "android/renderscript/RSRuntimeException",
                       "%s: RenderScript Surface I/O is not supported on this device", what);
}

static jlong nContextCreateGL(JNIEnv*, jobject, jlong, jint, jint, jint, jint, jint, jint, jint,
                              jint, jint, jint, jint, jint, jfloat, jint) {
  ALOGW("RenderScriptGL: no graphics driver, the context is not created");
  return 0;
}
static void nContextSetSurface(JNIEnv* env, jobject, jlong, jint, jint, jobject) {
  ThrowGraphics(env, "nContextSetSurface");
}
static void nContextPause(JNIEnv* env, jobject, jlong) { ThrowGraphics(env, "nContextPause"); }
static void nContextResume(JNIEnv* env, jobject, jlong) { ThrowGraphics(env, "nContextResume"); }

static jlong nFileA3DCreateFromFile(JNIEnv* env, jobject, jlong, jstring) {
  ThrowGraphics(env, "FileA3D");
  return 0;
}
static jlong nFileA3DCreateFromAssetStream(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "FileA3D");
  return 0;
}
static jlong nFileA3DCreateFromAsset(JNIEnv* env, jobject, jlong, jobject, jstring) {
  ThrowGraphics(env, "FileA3D");
  return 0;
}
static jint nFileA3DGetNumIndexEntries(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "FileA3D");
  return 0;
}
static void nFileA3DGetIndexEntries(JNIEnv* env, jobject, jlong, jlong, jint, jintArray,
                                    jobjectArray) {
  ThrowGraphics(env, "FileA3D");
}
static jlong nFileA3DGetEntryByIndex(JNIEnv* env, jobject, jlong, jlong, jint) {
  ThrowGraphics(env, "FileA3D");
  return 0;
}

static jlong nFontCreateFromFile(JNIEnv* env, jobject, jlong, jstring, jfloat, jint) {
  ThrowGraphics(env, "Font");
  return 0;
}
static jlong nFontCreateFromAssetStream(JNIEnv* env, jobject, jlong, jstring, jfloat, jint, jlong) {
  ThrowGraphics(env, "Font");
  return 0;
}
static jlong nFontCreateFromAsset(JNIEnv* env, jobject, jlong, jobject, jstring, jfloat, jint) {
  ThrowGraphics(env, "Font");
  return 0;
}

static jlong nProgramStoreCreate(JNIEnv* env, jobject, jlong, jboolean, jboolean, jboolean,
                                 jboolean, jboolean, jboolean, jint, jint, jint) {
  ThrowGraphics(env, "ProgramStore");
  return 0;
}
static void nProgramBindConstants(JNIEnv* env, jobject, jlong, jlong, jint, jlong) {
  ThrowGraphics(env, "Program.bindConstants");
}
static void nProgramBindTexture(JNIEnv* env, jobject, jlong, jlong, jint, jlong) {
  ThrowGraphics(env, "Program.bindTexture");
}
static void nProgramBindSampler(JNIEnv* env, jobject, jlong, jlong, jint, jlong) {
  ThrowGraphics(env, "Program.bindSampler");
}
static jlong nProgramFragmentCreate(JNIEnv* env, jobject, jlong, jstring, jobjectArray,
                                    jlongArray) {
  ThrowGraphics(env, "ProgramFragment");
  return 0;
}
static jlong nProgramRasterCreate(JNIEnv* env, jobject, jlong, jboolean, jint) {
  ThrowGraphics(env, "ProgramRaster");
  return 0;
}
static jlong nProgramVertexCreate(JNIEnv* env, jobject, jlong, jstring, jobjectArray, jlongArray) {
  ThrowGraphics(env, "ProgramVertex");
  return 0;
}

static void nContextBindRootScript(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "bindRootScript");
}
static void nContextBindProgramStore(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "bindProgramStore");
}
static void nContextBindProgramFragment(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "bindProgramFragment");
}
static void nContextBindProgramVertex(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "bindProgramVertex");
}
static void nContextBindProgramRaster(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "bindProgramRaster");
}

static jlong nMeshCreate(JNIEnv* env, jobject, jlong, jlongArray, jlongArray, jintArray) {
  ThrowGraphics(env, "Mesh");
  return 0;
}
static jint nMeshGetVertexBufferCount(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "Mesh");
  return 0;
}
static jint nMeshGetIndexCount(JNIEnv* env, jobject, jlong, jlong) {
  ThrowGraphics(env, "Mesh");
  return 0;
}
static void nMeshGetVertices(JNIEnv* env, jobject, jlong, jlong, jlongArray, jint) {
  ThrowGraphics(env, "Mesh");
}
static void nMeshGetIndices(JNIEnv* env, jobject, jlong, jlong, jlongArray, jintArray, jint) {
  ThrowGraphics(env, "Mesh");
}

// USAGE_IO allocations are refused when created (frameworks/rs patch 0001,
// rsdAllocation.cpp), so no Allocation has a Surface to give or take.
static jobject nAllocationGetSurface(JNIEnv* env, jobject, jlong, jlong) {
  ThrowSurface(env, "Allocation.getSurface");
  return nullptr;
}
static void nAllocationSetSurface(JNIEnv* env, jobject, jlong, jlong, jobject) {
  ThrowSurface(env, "Allocation.setSurface");
}

}  // namespace gesher_rs_unavailable
