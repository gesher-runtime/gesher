// gesher: AInputTransferToken and AInputReceiver (libandroid.so, API 35:
// <android/input_transfer_token_jni.h>, <android/surface_control_input_receiver.h>)
// for an app's ELF libraries (ADR 0089).
//
//  - AInputTransferToken_fromJava / toJava / release: the native
//    InputTransferToken of a Java android.window.InputTransferToken (the one
//    android_window_InputTransferToken.cpp keeps in mNativeObject), reference
//    counted as on Android.
//  - AInputReceiverCallbacks_*: a context and its motion and key callbacks.
//  - AInputReceiver_createBatchedInputReceiver / createUnbatchedInputReceiver:
//    on Android the window manager grants the receiver an input channel for
//    the SurfaceControl's layer (an embedded window under the host's) and
//    moves gestures to it (WindowManager.transferTouchGesture). Gesher's window
//    manager has no embedded windows and routes input by Win32 window, not by
//    layer, so no receiver can be granted: these return null (logged once),
//    and the app keeps receiving its input in its window, as before. No
//    receiver exists, so getInputTransferToken has none to answer for and
//    release takes only null.
#include "jni_bridge.h"

#include "abi_types.h"

#include <gesher/bionic_shim.h>

#include <android_runtime/android_window_InputTransferToken.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

struct AInputTransferToken;
struct AInputReceiver;
struct AInputEvent;

namespace gesher::bionic {
namespace {

void Fatal(const char* what) {
  std::fprintf(stderr, "gesher libandroid: %s\n", what);
  std::fflush(stderr);
  std::abort();
}

AInputTransferToken* BIONIC_SYSV TokenFromJava(JNIEnv* env, jobject token) {
  if (env == nullptr || token == nullptr) Fatal("AInputTransferToken_fromJava: null argument");
  android::InputTransferToken* t =
      android::android_window_InputTransferToken_getNativeInputTransferToken(RealJniEnv(env), token);
  if (t == nullptr) Fatal("AInputTransferToken_fromJava: not an InputTransferToken");
  t->incStrong(reinterpret_cast<void*>(&TokenFromJava));
  return reinterpret_cast<AInputTransferToken*>(t);
}

jobject BIONIC_SYSV TokenToJava(JNIEnv* env, const AInputTransferToken* token) {
  if (env == nullptr || token == nullptr) Fatal("AInputTransferToken_toJava: null argument");
  return android::android_window_InputTransferToken_getJavaInputTransferToken(
      RealJniEnv(env), *reinterpret_cast<const android::InputTransferToken*>(token));
}

void BIONIC_SYSV TokenRelease(AInputTransferToken* token) {
  if (token == nullptr) return;
  reinterpret_cast<android::InputTransferToken*>(token)->decStrong(reinterpret_cast<void*>(&TokenFromJava));
}

using SysvOnEvent = bool(BIONIC_SYSV*)(void* context, AInputEvent* event);

struct Callbacks {
  void* context;
  SysvOnEvent on_motion = nullptr;
  SysvOnEvent on_key = nullptr;
};

Callbacks* BIONIC_SYSV CallbacksCreate(void* context) { return new Callbacks{context}; }

void BIONIC_SYSV CallbacksRelease(Callbacks* callbacks) { delete callbacks; }

void BIONIC_SYSV CallbacksSetMotion(Callbacks* callbacks, SysvOnEvent fn) {
  if (callbacks == nullptr) Fatal("AInputReceiverCallbacks_setMotionEventCallback: null callbacks");
  callbacks->on_motion = fn;
}

void BIONIC_SYSV CallbacksSetKey(Callbacks* callbacks, SysvOnEvent fn) {
  if (callbacks == nullptr) Fatal("AInputReceiverCallbacks_setKeyEventCallback: null callbacks");
  callbacks->on_key = fn;
}

AInputReceiver* NoReceiver() {
  static std::atomic<bool> said{false};
  if (!said.exchange(true)) {
    std::fprintf(stderr,
                 "gesher libandroid: AInputReceiver: Gesher's window manager grants no input "
                 "channels to SurfaceControl layers (no embedded windows); no receiver\n");
    std::fflush(stderr);
  }
  return nullptr;
}

AInputReceiver* BIONIC_SYSV CreateBatched(void*, const AInputTransferToken*, const void*, Callbacks*) {
  return NoReceiver();
}

AInputReceiver* BIONIC_SYSV CreateUnbatched(void*, const AInputTransferToken*, const void*, Callbacks*) {
  return NoReceiver();
}

const AInputTransferToken* BIONIC_SYSV ReceiverToken(AInputReceiver* receiver) {
  if (receiver == nullptr) Fatal("AInputReceiver_getInputTransferToken: null receiver");
  Fatal("AInputReceiver_getInputTransferToken: not a receiver");
  return nullptr;
}

void BIONIC_SYSV ReceiverRelease(AInputReceiver* receiver) {
  if (receiver != nullptr) Fatal("AInputReceiver_release: not a receiver");
}

void Register() {
  struct Entry {
    const char* name;
    void* fn;
  };
#define E(name, fn) {name, reinterpret_cast<void*>(&fn)}
  static const Entry kEntries[] = {
      E("AInputTransferToken_fromJava", TokenFromJava),
      E("AInputTransferToken_toJava", TokenToJava),
      E("AInputTransferToken_release", TokenRelease),
      E("AInputReceiverCallbacks_create", CallbacksCreate),
      E("AInputReceiverCallbacks_release", CallbacksRelease),
      E("AInputReceiverCallbacks_setMotionEventCallback", CallbacksSetMotion),
      E("AInputReceiverCallbacks_setKeyEventCallback", CallbacksSetKey),
      E("AInputReceiver_createBatchedInputReceiver", CreateBatched),
      E("AInputReceiver_createUnbatchedInputReceiver", CreateUnbatched),
      E("AInputReceiver_getInputTransferToken", ReceiverToken),
      E("AInputReceiver_release", ReceiverRelease),
  };
#undef E
  for (const Entry& e : kEntries) AddBionicSymbol(e.name, e.fn);
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::bionic
