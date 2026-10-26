// Gesher media: JNI helpers (jni_util.h).

#include "jni_util.h"

#include <atomic>
#include <string>
#include <vector>

#include "../audio/log.h"

#define SYSV __attribute__((sysv_abi))

namespace gesher::media {
namespace {

std::atomic<JavaVM*> gVm{nullptr};

// The stand-ins: System V functions that take no parameters. ART calls
// natives with the System V ABI, where the caller pops the arguments, so one
// function answers every signature; floating-point results come back in
// xmm0, the others in rax.
extern "C" SYSV jlong gesher_media_Standin_zero() { return 0; }
extern "C" SYSV jdouble gesher_media_Standin_zeroFloat() { return 0.0; }

std::string Descriptor(JNIEnv* env, jclass type, jmethodID getName) {
  auto name = static_cast<jstring>(env->CallObjectMethod(type, getName));
  std::string n = Utf8(env, name);
  env->DeleteLocalRef(name);
  if (n == "int") return "I";
  if (n == "long") return "J";
  if (n == "boolean") return "Z";
  if (n == "byte") return "B";
  if (n == "char") return "C";
  if (n == "short") return "S";
  if (n == "float") return "F";
  if (n == "double") return "D";
  if (n == "void") return "V";
  for (char& c : n) {
    if (c == '.') c = '/';
  }
  return n[0] == '[' ? n : "L" + n + ";";
}

}  // namespace

JavaVM* Vm() { return gVm.load(); }

void SetVm(JNIEnv* env) {
  if (gVm.load() != nullptr) return;
  JavaVM* vm = nullptr;
  env->GetJavaVM(&vm);
  gVm.store(vm);
}

JNIEnv* AttachedEnv(const char* name) {
  JavaVM* vm = Vm();
  if (vm == nullptr) return nullptr;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
  JavaVMAttachArgs args = {JNI_VERSION_1_6, const_cast<char*>(name), nullptr};
  if (vm->AttachCurrentThreadAsDaemon(&env, &args) != JNI_OK) return nullptr;
  // Detached when the thread ends (ART aborts on a native thread that exits attached:
  // a codec's worker, a player's video thread).
  struct Detacher {
    JavaVM* vm = nullptr;
    ~Detacher() {
      if (vm != nullptr) vm->DetachCurrentThread();
    }
  };
  static thread_local Detacher detacher;
  detacher.vm = vm;
  return env;
}

bool CheckException(JNIEnv* env, const char* where) {
  if (!env->ExceptionCheck()) return false;
  MEDIA_LOGE("%s threw", where);
  env->ExceptionDescribe();
  env->ExceptionClear();
  return true;
}

std::string Utf8(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::string();
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c != nullptr ? c : "";
  if (c != nullptr) env->ReleaseStringUTFChars(s, c);
  return r;
}

std::u16string Utf16(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::u16string();
  jsize n = env->GetStringLength(s);
  std::u16string r(static_cast<size_t>(n), u'\0');
  env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(r.data()));
  return r;
}

bool RegisterStandins(JNIEnv* env, const char* className) {
  SetVm(env);
  jclass cls = env->FindClass(className);
  if (cls == nullptr) {
    env->ExceptionClear();
    MEDIA_LOGI("%s not on the class path; its natives skipped", className);
    return false;
  }
  jclass classClass = env->FindClass("java/lang/Class");
  jclass methodClass = env->FindClass("java/lang/reflect/Method");
  // ART's getDeclaredMethods resolves every method's types first (and fails on
  // one it cannot load); the unchecked list does not, and only the natives'
  // types are resolved below.
  jmethodID list = env->GetMethodID(classClass, "getDeclaredMethodsUnchecked",
                                    "(Z)[Ljava/lang/reflect/Method;");
  jobjectArray methods = nullptr;
  if (list != nullptr) {
    methods = static_cast<jobjectArray>(env->CallObjectMethod(cls, list, JNI_FALSE));
  } else {
    env->ExceptionClear();
  }
  if (methods == nullptr) {
    env->ExceptionClear();
    jmethodID all = env->GetMethodID(classClass, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    methods = static_cast<jobjectArray>(env->CallObjectMethod(cls, all));
    if (methods == nullptr) {
      CheckException(env, "getDeclaredMethods");
      return true;
    }
  }
  jmethodID getModifiers = env->GetMethodID(methodClass, "getModifiers", "()I");
  jmethodID getName = env->GetMethodID(methodClass, "getName", "()Ljava/lang/String;");
  jmethodID getParams = env->GetMethodID(methodClass, "getParameterTypes", "()[Ljava/lang/Class;");
  jmethodID getReturn = env->GetMethodID(methodClass, "getReturnType", "()Ljava/lang/Class;");
  jmethodID className_ = env->GetMethodID(classClass, "getName", "()Ljava/lang/String;");
  jsize n = env->GetArrayLength(methods);
  int registered = 0;
  for (jsize i = 0; i < n; i++) {
    jobject m = env->GetObjectArrayElement(methods, i);
    if ((env->CallIntMethod(m, getModifiers) & 0x100 /* Modifier.NATIVE */) == 0) {
      env->DeleteLocalRef(m);
      continue;
    }
    auto params = static_cast<jobjectArray>(env->CallObjectMethod(m, getParams));
    auto ret = static_cast<jclass>(env->CallObjectMethod(m, getReturn));
    std::string name = Utf8(env, static_cast<jstring>(env->CallObjectMethod(m, getName)));
    if (env->ExceptionCheck() || params == nullptr || ret == nullptr) {
      env->ExceptionClear();
      MEDIA_LOGW("%s.%s: its types do not resolve; no stand-in", className, name.c_str());
      env->DeleteLocalRef(m);
      continue;
    }
    std::string sig = "(";
    jsize np = env->GetArrayLength(params);
    for (jsize p = 0; p < np; p++) {
      auto t = static_cast<jclass>(env->GetObjectArrayElement(params, p));
      sig += Descriptor(env, t, className_);
      env->DeleteLocalRef(t);
    }
    std::string r = Descriptor(env, ret, className_);
    sig += ")" + r;
    void* fn = r == "F" || r == "D" ? reinterpret_cast<void*>(&gesher_media_Standin_zeroFloat)
                                    : reinterpret_cast<void*>(&gesher_media_Standin_zero);
    JNINativeMethod nm = {name.c_str(), sig.c_str(), fn};
    if (env->RegisterNatives(cls, &nm, 1) == 0) {
      registered++;
    } else {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(params);
    env->DeleteLocalRef(ret);
    env->DeleteLocalRef(m);
  }
  env->DeleteLocalRef(methods);
  env->DeleteLocalRef(cls);
  (void)registered;  // every native of the class now answers something
  return true;
}

int RegisterMethods(JNIEnv* env, const char* className, const JNINativeMethod* methods, int count) {
  SetVm(env);
  jclass cls = env->FindClass(className);
  if (cls == nullptr) {
    env->ExceptionClear();
    return 0;
  }
  int done = 0;
  for (int i = 0; i < count; i++) {
    if (env->RegisterNatives(cls, &methods[i], 1) == 0) {
      done++;
    } else {
      env->ExceptionClear();
      MEDIA_LOGW("%s.%s%s: not in framework.jar; not registered", className, methods[i].name,
                 methods[i].signature);
    }
  }
  env->DeleteLocalRef(cls);
  return done;
}

}  // namespace gesher::media
