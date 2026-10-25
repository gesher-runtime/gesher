// Gesher: the media JNI libraries the framework loads by name, as builtin
// libraries (docs/media.md): libmedia_jni.so (MediaPlayer and the rest of
// android.media's player, codec and metadata classes), libsoundpool.so
// (SoundPool) and libaudioeffect_jni.so (AudioEffect, Visualizer: stand-ins).
// Each JNI_OnLoad registers stand-ins for every native of its classes, then
// Gesher's.

#include <jni.h>

#include <gesher/builtin_library.h>

#include "../audio/log.h"
#include "jni_util.h"
#include "media_jni.h"

namespace gesher::media {
namespace {

JNIEnv* EnvOf(JavaVM* vm) {
  JNIEnv* env = nullptr;
  return vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK ? env : nullptr;
}

}  // namespace
}  // namespace gesher::media

extern "C" jint JNI_OnLoad_media_jni(JavaVM* vm, void*) {
  using namespace gesher::media;
  JNIEnv* env = EnvOf(vm);
  if (env == nullptr) return JNI_ERR;
  SetVm(env);
  RegisterMediaPlayer(env);
  RegisterMediaJniOthers(env);
  return JNI_VERSION_1_6;
}

extern "C" jint JNI_OnLoad_soundpool(JavaVM* vm, void*) {
  using namespace gesher::media;
  JNIEnv* env = EnvOf(vm);
  if (env == nullptr) return JNI_ERR;
  SetVm(env);
  RegisterSoundPool(env);
  return JNI_VERSION_1_6;
}

extern "C" jint JNI_OnLoad_audioeffect_jni(JavaVM* vm, void*) {
  using namespace gesher::media;
  JNIEnv* env = EnvOf(vm);
  if (env == nullptr) return JNI_ERR;
  SetVm(env);
  // No audio effects: AudioEffect's and Visualizer's natives answer as a
  // device without the effect would (setup fails, so the effect is not
  // created).
  RegisterStandins(env, "android/media/audiofx/AudioEffect");
  RegisterStandins(env, "android/media/audiofx/Visualizer");
  return JNI_VERSION_1_6;
}

namespace {

const gesher_builtin_symbol kMediaJni[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_media_jni)},
};
const gesher_builtin_symbol kSoundPool[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_soundpool)},
};
const gesher_builtin_symbol kAudioEffect[] = {
    {"JNI_OnLoad", reinterpret_cast<void*>(&JNI_OnLoad_audioeffect_jni)},
};

[[maybe_unused]] const bool registered = [] {
  gesher_register_builtin_library("libmedia_jni.so", kMediaJni, 1);
  gesher_register_builtin_library("libsoundpool.so", kSoundPool, 1);
  gesher_register_builtin_library("libaudioeffect_jni.so", kAudioEffect, 1);
  return true;
}();

}  // namespace
