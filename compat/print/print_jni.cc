// Gesher (print): com.gesher.server.WindowsPrint's natives (gesher-app.jar),
// registered by android_runtime.cc. The Win32 side is print_win32.cc.

#include <jni.h>
#include <nativehelper/JNIHelp.h>

#include <cstdio>
#include <string>
#include <vector>

#include "core_jni_helpers.h"
#include "print_win32.h"

namespace android {

int RegisterGesherNatives(JNIEnv* env, const char* className, const JNINativeMethod* methods,
                          int count);  // services_natives.cc

namespace {

namespace pr = gesher::print;

std::wstring Str(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::wstring();
  const jchar* chars = env->GetStringChars(s, nullptr);
  if (chars == nullptr) return std::wstring();
  std::wstring result(reinterpret_cast<const wchar_t*>(chars), env->GetStringLength(s));
  env->ReleaseStringChars(s, chars);
  return result;
}

jstring JStr(JNIEnv* env, const std::wstring& s) {
  return env->NewString(reinterpret_cast<const jchar*>(s.data()), static_cast<jsize>(s.size()));
}

std::vector<BYTE> Bytes(JNIEnv* env, jbyteArray array) {
  std::vector<BYTE> bytes;
  if (array == nullptr) return bytes;
  bytes.resize(env->GetArrayLength(array));
  if (!bytes.empty()) env->GetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<jbyte*>(bytes.data()));
  return bytes;
}

jbyteArray JBytes(JNIEnv* env, const std::vector<BYTE>& bytes) {
  jbyteArray array = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (array && !bytes.empty()) {
    env->SetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return array;
}

std::vector<int> Ints(JNIEnv* env, jintArray array) {
  std::vector<int> ints;
  if (array == nullptr) return ints;
  ints.resize(env->GetArrayLength(array));
  if (!ints.empty()) env->GetIntArrayRegion(array, 0, static_cast<jsize>(ints.size()), reinterpret_cast<jint*>(ints.data()));
  return ints;
}

void Throw(JNIEnv* env, const char* cls, const char* what, HRESULT hr) {
  char message[160];
  std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
  jniThrowException(env, cls, message);
}

void ThrowIo(JNIEnv* env, const char* what, HRESULT hr) { Throw(env, "java/io/IOException", what, hr); }

struct Activity {
  JNIEnv* env;
  jobject active;
  jmethodID get;
};

// BooleanSupplier.getAsBoolean on the calling thread; an exception counts as inactive.
bool IsActive(void* context) {
  auto* activity = static_cast<Activity*>(context);
  if (activity->env->ExceptionCheck()) return false;
  bool result = activity->env->CallBooleanMethod(activity->active, activity->get) == JNI_TRUE;
  return !activity->env->ExceptionCheck() && result;
}

bool MakeActivity(JNIEnv* env, jobject active, Activity* activity) {
  if (active == nullptr) {
    jniThrowNullPointerException(env, "Print lifetime is required");
    return false;
  }
  jclass cls = env->GetObjectClass(active);
  if (!cls) return false;
  jmethodID get = env->GetMethodID(cls, "getAsBoolean", "()Z");
  env->DeleteLocalRef(cls);
  if (!get) return false;
  *activity = Activity{env, active, get};
  return true;
}

jstring WindowsPrint_defaultPrinter(JNIEnv* env, jclass) {
  std::wstring name;
  HRESULT hr = pr::DefaultPrinter(&name);
  return hr == S_OK ? JStr(env, name) : nullptr;
}

jbyteArray WindowsPrint_defaultSettings(JNIEnv* env, jclass, jstring printer) {
  std::vector<BYTE> devmode;
  HRESULT hr = pr::DefaultSettings(Str(env, printer), &devmode);
  if (FAILED(hr)) { ThrowIo(env, "DocumentProperties", hr); return nullptr; }
  return JBytes(env, devmode);
}

jbyteArray WindowsPrint_apply(JNIEnv* env, jclass, jstring printer, jbyteArray settings, jintArray values) {
  std::vector<int> v = Ints(env, values);
  if (v.size() != 5) { jniThrowException(env, "java/lang/IllegalArgumentException", "request"); return nullptr; }
  pr::Request request;
  request.width = v[0];
  request.height = v[1];
  request.landscape = v[2];
  request.color = v[3];
  request.duplex = v[4];
  std::vector<BYTE> devmode = Bytes(env, settings);
  HRESULT hr = pr::ApplyRequest(Str(env, printer), request, &devmode);
  if (FAILED(hr)) { ThrowIo(env, "DocumentProperties", hr); return nullptr; }
  return JBytes(env, devmode);
}

jobjectArray WindowsPrint_describe(JNIEnv* env, jclass, jstring printer, jbyteArray settings) {
  pr::Description d;
  HRESULT hr = pr::Describe(Str(env, printer), Bytes(env, settings), &d);
  if (FAILED(hr)) { ThrowIo(env, "Printer settings", hr); return nullptr; }
  // WindowsPrint.PAPER .. MARGIN_BOTTOM.
  jint values[15] = {d.paper, d.width_mils, d.height_mils, d.landscape, d.color, d.color_capable, d.duplex,
                     d.duplex_capable, d.dpi_x, d.dpi_y, d.copies, d.margins_mils[0], d.margins_mils[1],
                     d.margins_mils[2], d.margins_mils[3]};
  jintArray ints = env->NewIntArray(15);
  if (!ints) return nullptr;
  env->SetIntArrayRegion(ints, 0, 15, values);
  jclass object = env->FindClass("java/lang/Object");
  if (!object) return nullptr;
  jobjectArray result = env->NewObjectArray(2, object, nullptr);
  env->DeleteLocalRef(object);
  if (!result) return nullptr;
  env->SetObjectArrayElement(result, 0, ints);
  jstring name = JStr(env, d.paper_name);
  if (!name) return nullptr;
  env->SetObjectArrayElement(result, 1, name);
  return result;
}

jobjectArray WindowsPrint_choose(JNIEnv* env, jclass, jstring printer, jbyteArray settings, jint page_count,
                                 jint owner_pid, jobject active) {
  Activity activity;
  if (!MakeActivity(env, active, &activity)) return nullptr;
  pr::Choice choice;
  HRESULT hr = pr::ChoosePrinter(Str(env, printer), Bytes(env, settings), page_count,
                                 static_cast<DWORD>(owner_pid), IsActive, &activity, &choice);
  if (env->ExceptionCheck()) return nullptr;
  if (FAILED(hr)) { ThrowIo(env, "Windows print dialog", hr); return nullptr; }
  if (hr != S_OK) return nullptr;
  jclass object = env->FindClass("java/lang/Object");
  if (!object) return nullptr;
  jobjectArray result = env->NewObjectArray(3, object, nullptr);
  env->DeleteLocalRef(object);
  if (!result) return nullptr;
  jstring name = JStr(env, choice.printer);
  jbyteArray devmode = JBytes(env, choice.devmode);
  jintArray pages = env->NewIntArray(static_cast<jsize>(choice.pages.size()));
  if (!name || !devmode || !pages) return nullptr;
  if (!choice.pages.empty()) {
    env->SetIntArrayRegion(pages, 0, static_cast<jsize>(choice.pages.size()),
                           reinterpret_cast<const jint*>(choice.pages.data()));
  }
  env->SetObjectArrayElement(result, 0, name);
  env->SetObjectArrayElement(result, 1, devmode);
  env->SetObjectArrayElement(result, 2, pages);
  return result;
}

jint WindowsPrint_print(JNIEnv* env, jclass, jstring printer, jbyteArray settings, jstring pdf, jintArray pages,
                        jstring name, jstring output, jobject active) {
  Activity activity;
  if (!MakeActivity(env, active, &activity)) return 0;
  DWORD job = 0;
  int printed = 0;
  HRESULT hr = pr::PrintPdf(Str(env, printer), Bytes(env, settings), Str(env, pdf), Ints(env, pages),
                            Str(env, name), Str(env, output), IsActive, &activity, &job, &printed);
  if (env->ExceptionCheck()) return 0;
  if (hr == E_ABORT) {
    jniThrowException(env, "java/io/InterruptedIOException", "Print job canceled");
    return 0;
  }
  if (FAILED(hr)) { ThrowIo(env, "Windows printing", hr); return 0; }
  return static_cast<jint>(job);
}

#define PRINT_STRING "Ljava/lang/String;"
#define PRINT_ACTIVE "Ljava/util/function/BooleanSupplier;"

const JNINativeMethod gWindowsPrintMethods[] = {
    {"nativeDefaultPrinter", "()" PRINT_STRING, GESHER_JNI(WindowsPrint_defaultPrinter)},
    {"nativeDefaultSettings", "(" PRINT_STRING ")[B", GESHER_JNI(WindowsPrint_defaultSettings)},
    {"nativeApply", "(" PRINT_STRING "[B[I)[B", GESHER_JNI(WindowsPrint_apply)},
    {"nativeDescribe", "(" PRINT_STRING "[B)[Ljava/lang/Object;", GESHER_JNI(WindowsPrint_describe)},
    {"nativeChoose", "(" PRINT_STRING "[BII" PRINT_ACTIVE ")[Ljava/lang/Object;", GESHER_JNI(WindowsPrint_choose)},
    {"nativePrint", "(" PRINT_STRING "[B" PRINT_STRING "[I" PRINT_STRING PRINT_STRING PRINT_ACTIVE ")I",
     GESHER_JNI(WindowsPrint_print)},
};

}  // namespace

int register_gesher_print(JNIEnv* env) {
  return RegisterGesherNatives(env, "com/gesher/server/WindowsPrint", gWindowsPrintMethods,
                               NELEM(gWindowsPrintMethods));
}

}  // namespace android
