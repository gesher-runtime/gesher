// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
//
// com.gesher.server.WindowsCaptions (gesher-app.jar): Windows' caption style for the
// system server's CaptioningBridge (caption_win.cc). Registered by devices_jni.cc.
#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include "caption_win.h"
#include "core_jni_helpers.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace captions = gesher::devices::captions;

jint WindowsCaptions_read(JNIEnv* env, jclass, jintArray out) {
  if (out == nullptr || env->GetArrayLength(out) < captions::kValues) return E_INVALIDARG;
  captions::Properties p = {};
  HRESULT hr = captions::Read(&p);
  if (FAILED(hr)) return hr;
  jint values[captions::kValues] = {
      p.font_color,
      static_cast<jint>(p.computed_font_color),
      p.font_opacity,
      p.font_size,
      p.font_style,
      p.font_effect,
      p.background_color,
      static_cast<jint>(p.computed_background_color),
      p.background_opacity,
      p.region_color,
      static_cast<jint>(p.computed_region_color),
      p.region_opacity,
  };
  env->SetIntArrayRegion(out, 0, captions::kValues, values);
  return S_OK;
}

jint WindowsCaptions_startWatch(JNIEnv*, jclass) { return captions::StartWatch(); }

jlong WindowsCaptions_awaitChange(JNIEnv*, jclass, jlong seen, jint timeout_ms) {
  return captions::AwaitChange(seen, timeout_ms < 0 ? 0u : static_cast<uint32_t>(timeout_ms));
}

void WindowsCaptions_stopWatch(JNIEnv*, jclass) { captions::StopWatch(); }

const JNINativeMethod gWindowsCaptionsMethods[] = {
    {"nativeRead", "([I)I", GESHER_JNI(WindowsCaptions_read)},
    {"nativeStartWatch", "()I", GESHER_JNI(WindowsCaptions_startWatch)},
    {"nativeAwaitChange", "(JI)J", GESHER_JNI(WindowsCaptions_awaitChange)},
    {"nativeStopWatch", "()V", GESHER_JNI(WindowsCaptions_stopWatch)},
};

}  // namespace

int register_gesher_captions(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsCaptions", gWindowsCaptionsMethods,
                               NELEM(gWindowsCaptionsMethods));
}

}  // namespace android
