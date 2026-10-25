// Gesher: libandroid_runtime's input natives and gesher-app's input classes
// (docs/input.md).
//
// register_gesher_input (called from compat/framework/android_runtime.cc's
// table, after the attach stand-ins, which these replace) registers
// core/jni's input natives, built from System V copies of their sources
// (gen_jni_sources.py): InputChannel, InputEventReceiver, InputEventSender,
// KeyEvent, MotionEvent, KeyCharacterMap and VelocityTracker; then
// com.gesher.app.input.WindowInput (an input connection, connection.h) and
// com.gesher.app.input.VirtualKeyboard (the built-in Generic.kcm). Each is
// skipped when its class is not on the class path.
//
// Gesher's own natives are declared JNICALL (System V under GESHER), as ART
// calls natives (docs/decisions/0006-managed-abi.md).

#define LOG_TAG "GesherInput"

#include <jni.h>

#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include <input/Input.h>
#include <input/KeyCharacterMap.h>
#include <log/log.h>
#include <nativehelper/JNIHelp.h>

#include "android_view_InputChannel.h"
#include "android_view_KeyCharacterMap.h"
#include "connection.h"
#include "keymap.h"
#include "win32.h"

namespace android {
int register_android_view_InputChannel(JNIEnv* env);
int register_android_view_InputEventReceiver(JNIEnv* env);
int register_android_view_InputEventSender(JNIEnv* env);
int register_android_view_KeyCharacterMap(JNIEnv* env);
int register_android_view_KeyEvent(JNIEnv* env);
int register_android_view_MotionEvent(JNIEnv* env);
int register_android_view_VelocityTracker(JNIEnv* env);
}  // namespace android

namespace {

using gesher::WindowEvent;
using gesher::input::Connection;

// --- com.gesher.app.input.VirtualKeyboard ---------------------------------------------

jobject JNICALL VirtualKeyboard_load(JNIEnv* env, jclass, jint deviceId) {
  std::unique_ptr<android::KeyCharacterMap> map = gesher::input::LoadGenericKeyCharacterMap();
  if (map == nullptr) {
    jniThrowException(env, "java/lang/IllegalStateException", "Generic.kcm does not load");
    return nullptr;
  }
  return android::android_view_KeyCharacterMap_create(env, deviceId, std::move(map));
}

const JNINativeMethod kVirtualKeyboardMethods[] = {
    {"nLoad", "(I)Landroid/view/KeyCharacterMap;", reinterpret_cast<void*>(&VirtualKeyboard_load)},
};

// --- com.gesher.app.input.WindowInput ---------------------------------------------------

jmethodID gOnWindowEvent;
jmethodID gOnTextEvent;

struct JavaConnection {
  std::unique_ptr<Connection> connection;
  jobject self = nullptr;  // global reference to the WindowInput
};

jlong JNICALL WindowInput_attach(JNIEnv* env, jclass, jobject self, jlong hwnd, jobject channelObj,
                                 jfloat scale, jfloat offsetX, jfloat offsetY, jint displayId,
                                 jint maxInFlight, jboolean initialFocus) {
  gesher::Window* window = gesher::input::WindowFromHwnd(reinterpret_cast<void*>(hwnd));
  if (window == nullptr) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "not a Gesher window");
    return 0;
  }
  std::shared_ptr<android::InputChannel> channel =
      android::android_view_InputChannel_getInputChannel(env, channelObj);
  if (channel == nullptr) {
    jniThrowException(env, "java/lang/IllegalArgumentException", "the InputChannel is disposed");
    return 0;
  }
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  auto* jc = new JavaConnection;
  jc->self = env->NewGlobalRef(self);
  gesher::input::ConnectionOptions o;
  o.translate.scale = scale != 0 ? scale : 1.0f;
  o.translate.offset_x = offsetX;
  o.translate.offset_y = offsetY;
  o.display_id = displayId;
  if (maxInFlight > 0) o.max_in_flight = maxInFlight;
  o.initial_focus = initialFocus;
  jobject target = jc->self;
  // Window events that are not input go to WindowInput.onWindowEvent on the
  // connection's thread, attached to the VM as a daemon while it runs.
  o.on_window_event = [vm, target](const WindowEvent& e) {
    JNIEnv* tenv = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&tenv), JNI_VERSION_1_6) != JNI_OK) {
      JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("gesher-input"), nullptr};
      if (vm->AttachCurrentThreadAsDaemon(&tenv, &args) != JNI_OK) return;
    }
    int32_t a = e.type == WindowEvent::kResize ? e.width
                : e.type == WindowEvent::kChar ? static_cast<int32_t>(e.ch)
                                               : e.button;
    int32_t b = e.type == WindowEvent::kResize ? e.height : e.modifiers;
    tenv->CallVoidMethod(target, gOnWindowEvent, e.type, a, b);
    if (tenv->ExceptionCheck()) {
      ALOGE("WindowInput.onWindowEvent threw");
      tenv->ExceptionDescribe();
      tenv->ExceptionClear();
    }
  };
  // Text for the focused editor goes to WindowInput.onTextEvent (its text
  // listener, the window's input method), which finishes it.
  o.on_text_event = [vm, target](const gesher::input::TextEvent& e, uint32_t seq) {
    JNIEnv* tenv = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&tenv), JNI_VERSION_1_6) != JNI_OK) {
      JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>("gesher-input"), nullptr};
      if (vm->AttachCurrentThreadAsDaemon(&tenv, &args) != JNI_OK) return;
    }
    jstring text = tenv->NewString(reinterpret_cast<const jchar*>(e.text.data()),
                                   static_cast<jsize>(e.text.size()));
    tenv->CallVoidMethod(target, gOnTextEvent, static_cast<jint>(seq), e.kind, text, e.cursor,
                         e.target_start, e.target_end);
    if (tenv->ExceptionCheck()) {
      ALOGE("WindowInput.onTextEvent threw");
      tenv->ExceptionDescribe();
      tenv->ExceptionClear();
    }
    if (text != nullptr) tenv->DeleteLocalRef(text);
  };
  o.on_thread_exit = [vm]() {
    JNIEnv* tenv = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&tenv), JNI_VERSION_1_6) == JNI_OK) {
      vm->DetachCurrentThread();
    }
  };
  jc->connection = std::make_unique<Connection>(window, std::move(channel), std::move(o));
  return reinterpret_cast<jlong>(jc);
}

void JNICALL WindowInput_detach(JNIEnv* env, jclass, jlong ptr) {
  auto* jc = reinterpret_cast<JavaConnection*>(ptr);
  jc->connection.reset();  // joins the thread
  env->DeleteGlobalRef(jc->self);
  delete jc;
}

void JNICALL WindowInput_inject(JNIEnv*, jclass, jlong ptr, jint type, jfloat x, jfloat y,
                                jint button, jint key, jint scan, jint repeat, jint modifiers,
                                jfloat delta, jfloat deltaX, jint pointer, jint tool) {
  WindowEvent e;
  e.type = type;
  e.x = x;
  e.y = y;
  e.button = button;
  e.key = key;
  e.scan = scan;
  e.repeat = repeat;
  e.modifiers = modifiers;
  e.delta = delta;
  e.delta_x = deltaX;
  e.pointer = pointer;
  e.tool = tool;
  reinterpret_cast<JavaConnection*>(ptr)->connection->Inject(e);
}

jlongArray JNICALL WindowInput_stats(JNIEnv* env, jclass, jlong ptr) {
  gesher_input_stats s = reinterpret_cast<JavaConnection*>(ptr)->connection->Stats();
  jlong values[5] = {static_cast<jlong>(s.published), static_cast<jlong>(s.finished),
                     static_cast<jlong>(s.handled), static_cast<jlong>(s.in_flight),
                     static_cast<jlong>(s.dropped)};
  jlongArray out = env->NewLongArray(5);
  if (out != nullptr) env->SetLongArrayRegion(out, 0, 5, values);
  return out;
}

jboolean JNICALL WindowInput_postMessage(JNIEnv*, jclass, jlong hwnd, jint msg, jlong wparam,
                                         jlong lparam) {
  return gesher::input::PostWindowMessage(reinterpret_cast<void*>(hwnd),
                                          static_cast<uint32_t>(msg),
                                          static_cast<uint64_t>(wparam), lparam);
}

jlong JNICALL WindowInput_clientToScreen(JNIEnv*, jclass, jlong hwnd, jint x, jint y) {
  int32_t sx = x, sy = y;
  if (!gesher::input::ClientToScreenPoint(reinterpret_cast<void*>(hwnd), &sx, &sy)) {
    sx = x;
    sy = y;
  }
  return (static_cast<jlong>(sy) << 32) | static_cast<uint32_t>(sx);
}

Connection* ConnectionOf(jlong ptr) {
  return reinterpret_cast<JavaConnection*>(ptr)->connection.get();
}

void JNICALL WindowInput_setTextInput(JNIEnv*, jclass, jlong ptr, jboolean active, jboolean ime,
                                      jint enterAction) {
  gesher::input::TextInputState state;
  state.active = active;
  state.ime = ime;
  state.enter_action = enterAction;
  ConnectionOf(ptr)->SetTextInput(state);
}

void JNICALL WindowInput_setCaret(JNIEnv*, jclass, jlong ptr, jint x, jint y, jint width,
                                  jint height) {
  ConnectionOf(ptr)->SetCaret(x, y, width, height);
}

void JNICALL WindowInput_completeComposition(JNIEnv*, jclass, jlong ptr, jboolean cancel) {
  ConnectionOf(ptr)->CompleteComposition(cancel);
}

jint JNICALL WindowInput_showTouchKeyboard(JNIEnv*, jclass, jlong ptr, jboolean show) {
  return ConnectionOf(ptr)->ShowTouchKeyboard(show);
}

void JNICALL WindowInput_finishTextEvent(JNIEnv*, jclass, jlong ptr, jint seq) {
  ConnectionOf(ptr)->FinishTextEvent(static_cast<uint32_t>(seq));
}

// {hkl as hex, BCP 47 locale, display name} for each keyboard layout.
jobjectArray JNICALL WindowInput_keyboardLayouts(JNIEnv* env, jclass) {
  std::vector<gesher::input::KeyboardLayout> layouts = gesher::input::KeyboardLayouts();
  jclass string_class = env->FindClass("java/lang/String");
  jobjectArray out =
      env->NewObjectArray(static_cast<jsize>(layouts.size() * 3), string_class, nullptr);
  if (out == nullptr) return nullptr;
  for (size_t i = 0; i < layouts.size(); i++) {
    char hkl[32];
    snprintf(hkl, sizeof hkl, "%llx", static_cast<unsigned long long>(layouts[i].hkl));
    const std::string* fields[3] = {nullptr, &layouts[i].locale, &layouts[i].name};
    for (int f = 0; f < 3; f++) {
      jstring v = env->NewStringUTF(f == 0 ? hkl : fields[f]->c_str());
      env->SetObjectArrayElement(out, static_cast<jsize>(i * 3 + f), v);
      env->DeleteLocalRef(v);
    }
  }
  return out;
}

jlong JNICALL WindowInput_keyboardLayout(JNIEnv*, jclass, jlong hwnd) {
  return static_cast<jlong>(gesher::input::ThreadKeyboardLayout(reinterpret_cast<void*>(hwnd)));
}

const JNINativeMethod kWindowInputMethods[] = {
    {"nAttach", "(Lcom/gesher/app/input/WindowInput;JLandroid/view/InputChannel;FFFIIZ)J",
     reinterpret_cast<void*>(&WindowInput_attach)},
    {"nDetach", "(J)V", reinterpret_cast<void*>(&WindowInput_detach)},
    {"nInject", "(JIFFIIIIIFFII)V", reinterpret_cast<void*>(&WindowInput_inject)},
    {"nStats", "(J)[J", reinterpret_cast<void*>(&WindowInput_stats)},
    {"nPostMessage", "(JIJJ)Z", reinterpret_cast<void*>(&WindowInput_postMessage)},
    {"nClientToScreen", "(JII)J", reinterpret_cast<void*>(&WindowInput_clientToScreen)},
    {"nSetTextInput", "(JZZI)V", reinterpret_cast<void*>(&WindowInput_setTextInput)},
    {"nSetCaret", "(JIIII)V", reinterpret_cast<void*>(&WindowInput_setCaret)},
    {"nCompleteComposition", "(JZ)V", reinterpret_cast<void*>(&WindowInput_completeComposition)},
    {"nFinishTextEvent", "(JI)V", reinterpret_cast<void*>(&WindowInput_finishTextEvent)},
    {"nShowTouchKeyboard", "(JZ)I", reinterpret_cast<void*>(&WindowInput_showTouchKeyboard)},
    {"nKeyboardLayouts", "()[Ljava/lang/String;",
     reinterpret_cast<void*>(&WindowInput_keyboardLayouts)},
    {"nKeyboardLayout", "(J)J", reinterpret_cast<void*>(&WindowInput_keyboardLayout)},
};

int RegisterWindowInput(JNIEnv* env) {
  jclass clazz = env->FindClass("com/gesher/app/input/WindowInput");
  gOnWindowEvent = env->GetMethodID(clazz, "onWindowEvent", "(III)V");
  gOnTextEvent = env->GetMethodID(clazz, "onTextEvent", "(IILjava/lang/String;III)V");
  env->DeleteLocalRef(clazz);
  if (gOnWindowEvent == nullptr || gOnTextEvent == nullptr) return -1;
  return jniRegisterNativeMethods(env, "com/gesher/app/input/WindowInput", kWindowInputMethods,
                                  sizeof(kWindowInputMethods) / sizeof(kWindowInputMethods[0]));
}

int RegisterVirtualKeyboard(JNIEnv* env) {
  return jniRegisterNativeMethods(
      env, "com/gesher/app/input/VirtualKeyboard", kVirtualKeyboardMethods,
      sizeof(kVirtualKeyboardMethods) / sizeof(kVirtualKeyboardMethods[0]));
}

}  // namespace

// AndroidRuntime.cpp's order for the framework's input natives.
int register_gesher_input(JNIEnv* env) {
  const struct {
    const char* className;
    int (*registerFunction)(JNIEnv*);
  } kRows[] = {
      {"android/view/InputChannel", android::register_android_view_InputChannel},
      {"android/view/InputEventReceiver", android::register_android_view_InputEventReceiver},
      {"android/view/InputEventSender", android::register_android_view_InputEventSender},
      {"android/view/KeyEvent", android::register_android_view_KeyEvent},
      {"android/view/MotionEvent", android::register_android_view_MotionEvent},
      {"android/view/KeyCharacterMap", android::register_android_view_KeyCharacterMap},
      {"android/view/VelocityTracker", android::register_android_view_VelocityTracker},
      {"com/gesher/app/input/WindowInput", RegisterWindowInput},
      {"com/gesher/app/input/VirtualKeyboard", RegisterVirtualKeyboard},
  };
  for (const auto& r : kRows) {
    jclass clazz = env->FindClass(r.className);
    if (clazz == nullptr) {
      env->ExceptionClear();
      continue;
    }
    env->DeleteLocalRef(clazz);
    if (r.registerFunction(env) < 0) {
      ALOGE("libandroid_runtime: registering the natives of %s failed", r.className);
      return -1;
    }
  }
  return 0;
}
