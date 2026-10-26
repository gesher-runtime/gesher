// gesher: JNI for the Android .so files the ELF loader loads (phase 6).
//
// ART's host path for System.loadLibrary / System.load is libnativeloader's
// OpenNativeLibrary, which calls plain dlopen, then dlsym for JNI_OnLoad and
// for each Java_* native (art/libnativeloader/native_loader.cpp,
// art/runtime/jni/java_vm_ext.cc). gesher-base's dlopen hands ELF files to the
// loader registered here (<gesher/elf_dlopen.h>), which loads them with
// compat/elf and the bionic shim. Everything ART gets back from dlsym is
// bridged:
//
//  - JNI_OnLoad / JNI_OnUnload: ART calls them with the Microsoft ABI (as for
//    its built-in libraries, docs/decisions/0011); a small stub calls the
//    .so's System V function, passing it the proxy JavaVM below.
//  - Java_*: ART enters natives with the System V ABI already (JNICALL,
//    docs/decisions/0006), so the .so's function is called as it is, through
//    a stub that swaps the JNIEnv* argument for the proxy JNIEnv below and
//    sets the thread up for loaded code first (SetupThreadTls, ADR 0014).
//    Functions the .so registers with RegisterNatives get the same stub.
//
// The .so calls back through the JNIEnv and JavaVM function tables with the
// System V ABI, but ART's tables are Microsoft ABI (built by the same compiler
// as their Gesher callers, docs/decisions/0011). So the .so never sees ART's
// JNIEnv: it gets a per-thread proxy whose table holds System V functions that
// forward to the real JNIEnv's table, and a proxy JavaVM likewise. The
// variadic Call*Method/NewObject entry points are assembly trampolines that
// build a System V va_list (shim_internal.h) and call the va_list forms; those
// read the arguments by the method's parameter types (recorded from
// GetMethodID's signature, else found by reflection) into a jvalue array and
// call ART's Call*MethodA.
//
// A @CriticalNative method gets no JNIEnv; the entry stub only replaces its
// first argument when that is this thread's JNIEnv.

#include "jni_bridge.h"
#include "extra_bionic.h"
#include "shim_internal.h"

#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_dlopen.h>
#include <gesher/elf_loader.h>

#include <jni.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace elf = gesher::elf;
using gesher::bionic::NextFp;
using gesher::bionic::NextGp;
using gesher::bionic::SysvVaList;

#define SYSV __attribute__((sysv_abi))

extern "C" {
// Assembly, below.
void gesher_jni_native_entry();
// The guest (arm64) native entry: like gesher_jni_native_entry but sets the
// thread's guest mode first (ADR 0047).
void gesher_jni_native_entry_guest();
// Called by gesher_jni_native_entry (Microsoft ABI): the JNIEnv* to pass on.
JNIEnv* gesher_jni_enter(JNIEnv* env);
// Called by gesher_jni_native_entry_guest: sets guest mode, then as above.
JNIEnv* gesher_jni_enter_guest(JNIEnv* env);
// JNI_OnLoad / JNI_OnUnload as ART calls them, plus the .so's function.
jint gesher_jni_onload_bridge(JavaVM* vm, void* reserved, void* target);
// The guest JNI_OnLoad bridge: sets guest mode and passes the guest JavaVM.
jint gesher_jni_onload_bridge_guest(JavaVM* vm, void* reserved, void* target);
}

namespace {

// --- the proxies -------------------------------------------------------------

constexpr size_t kEnvSlots = sizeof(JNINativeInterface) / sizeof(void*);
constexpr size_t kVmSlots = sizeof(JNIInvokeInterface) / sizeof(void*);
void* g_env_table[kEnvSlots];
void* g_vm_table[kVmSlots];

// Laid out like JNIEnv and JavaVM: the function table first.
struct ProxyEnv {
  void* const* functions;
  JNIEnv* real;
};
struct ProxyVm {
  void* const* functions;
};

ProxyVm g_vm{g_vm_table};
std::atomic<JavaVM*> g_real_vm{nullptr};
thread_local ProxyEnv t_env{g_env_table, nullptr};
thread_local bool t_thread_ready = false;

// --- guest (arm64) proxy tables (ADR 0047) -----------------------------------
// A guest .so calls through its JNIEnv/JavaVM function tables with AAPCS64, and
// executes the entries as guest code; so a guest gets tables whose every slot is
// a guest trampoline (gesher::arm64::BindHostFunction) into the same System V
// host entries an x86-64 .so calls directly. A guest is handed its own proxies,
// t_env_guest and g_vm_guest, whose tables are the guest ones; both proxies
// wrap the same real JNIEnv, so a thread can serve guest and host natives in
// any nesting. The host entries tell which one called them from the proxy
// they get (IsGuestEnv / IsGuestVm). x86-64 libraries' path is unchanged.
void* g_env_table_guest[kEnvSlots];
void* g_vm_table_guest[kVmSlots];
ProxyVm g_vm_guest{g_vm_table_guest};
thread_local ProxyEnv t_env_guest{g_env_table_guest, nullptr};
// Whether this thread last entered a guest (only for JNI_GetCreatedJavaVMs,
// which has no proxy argument to tell by).
thread_local bool t_guest_mode = false;
std::once_flag g_guest_tables_once;
bool g_guest_tables_ok = false;

bool IsGuestEnv(JNIEnv* env) {
  return env != nullptr && reinterpret_cast<ProxyEnv*>(env)->functions == g_env_table_guest;
}
bool IsGuestVm(JavaVM* vm) { return vm == reinterpret_cast<JavaVM*>(&g_vm_guest); }

// The JNIEnv slots that take a va_list (below, after GESHER_JNI_TYPES).
void RegisterVaListSlots();

void BuildGuestTables() {
  std::call_once(g_guest_tables_once, [] {
    if (!gesher::arm64::EnsureLoaded()) return;
    RegisterVaListSlots();
    for (size_t i = 0; i < kEnvSlots; ++i) {
      g_env_table_guest[i] =
          g_env_table[i] ? gesher::arm64::BindHostFunction(g_env_table[i]) : nullptr;
    }
    for (size_t i = 0; i < kVmSlots; ++i) {
      g_vm_table_guest[i] =
          g_vm_table[i] ? gesher::arm64::BindHostFunction(g_vm_table[i]) : nullptr;
    }
    g_guest_tables_ok = true;
  });
}

JavaVM* ProxyVmPtr() { return reinterpret_cast<JavaVM*>(&g_vm); }

JavaVM* GuestVmPtr() {
  BuildGuestTables();
  return g_guest_tables_ok ? reinterpret_cast<JavaVM*>(&g_vm_guest) : ProxyVmPtr();
}

// The proxy JavaVM for the side `guest` says.
JavaVM* VmFor(bool guest) { return guest ? GuestVmPtr() : ProxyVmPtr(); }

JavaVM* RealVm() {
  JavaVM* vm = g_real_vm.load(std::memory_order_acquire);
  if (vm == nullptr) {
    jsize n = 0;
    if (JNI_GetCreatedJavaVMs(&vm, 1, &n) != JNI_OK || n < 1) return nullptr;
    g_real_vm.store(vm, std::memory_order_release);
  }
  return vm;
}

// Loaded code may use initial-exec TLS through %fs:0 (ADR 0014): the thread
// needs its bionic TLS block before it enters the .so.
void EnsureThread() {
  if (!t_thread_ready) {
    t_thread_ready = true;
    elf::SetupThreadTls();
  }
}

JNIEnv* Wrap(JNIEnv* real) {
  if (real == nullptr) return nullptr;
  t_env.real = real;
  return reinterpret_cast<JNIEnv*>(&t_env);
}

// The guest's proxy for `real`: the same real JNIEnv behind the guest table.
JNIEnv* WrapGuest(JNIEnv* real) {
  if (real == nullptr) return nullptr;
  BuildGuestTables();
  if (!g_guest_tables_ok) return Wrap(real);
  t_env.real = real;
  t_env_guest.real = real;
  return reinterpret_cast<JNIEnv*>(&t_env_guest);
}

JNIEnv* WrapFor(bool guest, JNIEnv* real) { return guest ? WrapGuest(real) : Wrap(real); }

inline JNIEnv* Real(JNIEnv* env) { return reinterpret_cast<ProxyEnv*>(env)->real; }

// --- stubs ---------------------------------------------------------------------

std::mutex g_stub_mutex;
uint8_t* g_stub_page = nullptr;
size_t g_stub_used = 0;
constexpr size_t kStubSize = 32;
constexpr size_t kStubPageSize = 0x10000;
std::unordered_map<void*, void*> g_native_stubs;  // under g_stub_mutex
std::unordered_map<void*, void*> g_onload_stubs;  // under g_stub_mutex

// `mov <reg>, imm64(ctx); mov rax, imm64(entry); jmp rax`. `rex` and `op`
// encode `mov <reg>, imm64`: 49 BB is r11, 49 B8 is r8.
void* EmitStubLocked(uint8_t rex, uint8_t op, void* ctx, void* entry) {
  if (g_stub_page == nullptr || g_stub_used + kStubSize > kStubPageSize) {
    g_stub_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStubPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub_page == nullptr) return nullptr;
    g_stub_used = 0;
  }
  uint8_t* p = g_stub_page + g_stub_used;
  g_stub_used += kStubSize;
  uint64_t c = reinterpret_cast<uint64_t>(ctx), e = reinterpret_cast<uint64_t>(entry);
  p[0] = rex;
  p[1] = op;
  std::memcpy(p + 2, &c, 8);
  p[10] = 0x48;
  p[11] = 0xB8;  // mov rax, imm64
  std::memcpy(p + 12, &e, 8);
  p[20] = 0xFF;
  p[21] = 0xE0;  // jmp rax
  FlushInstructionCache(GetCurrentProcess(), p, kStubSize);
  return p;
}

// The System V entry ART calls for the .so's native `fn`. For a guest (arm64)
// native, the entry sets the thread's guest mode, swaps ART's JNIEnv for the
// guest proxy, and enters the guest through a host_entry stub (ADR 0047).
void* NativeEntry(void* fn) {
  if (fn == nullptr) return nullptr;
  std::lock_guard<std::mutex> lock(g_stub_mutex);
  void*& stub = g_native_stubs[fn];
  if (stub == nullptr) {
    if (gesher::arm64::IsGuestCode(fn)) {
      void* he = gesher::arm64::HostEntry(fn);
      stub = EmitStubLocked(0x49, 0xBB, he, reinterpret_cast<void*>(&gesher_jni_native_entry_guest));
    } else {
      stub = EmitStubLocked(0x49, 0xBB, fn, reinterpret_cast<void*>(&gesher_jni_native_entry));
    }
  }
  return stub;
}

// The Microsoft ABI JNI_OnLoad (or JNI_OnUnload) ART calls for the .so's `fn`.
void* OnLoadEntry(void* fn) {
  std::lock_guard<std::mutex> lock(g_stub_mutex);
  void*& stub = g_onload_stubs[fn];
  if (stub == nullptr) {
    if (gesher::arm64::IsGuestCode(fn)) {
      void* he = gesher::arm64::HostEntry(fn);
      stub = EmitStubLocked(0x49, 0xB8, he,
                            reinterpret_cast<void*>(&gesher_jni_onload_bridge_guest));
    } else {
      stub = EmitStubLocked(0x49, 0xB8, fn, reinterpret_cast<void*>(&gesher_jni_onload_bridge));
    }
  }
  return stub;
}

// --- parameter types, for the va_list forms ------------------------------------

// One character per parameter: Z B C S I J F D, or L for a reference.
std::shared_mutex g_kinds_mutex;
std::unordered_map<jmethodID, std::string> g_kinds;

std::string KindsOfSignature(const char* sig) {
  std::string out;
  if (sig == nullptr || *sig != '(') return out;
  for (const char* p = sig + 1; *p != '\0' && *p != ')'; ++p) {
    if (*p == '[' || *p == 'L') {
      while (*p == '[') ++p;
      if (*p == 'L') {
        while (*p != '\0' && *p != ';') ++p;
      }
      if (*p == '\0') break;
      out += 'L';
    } else {
      out += *p;
    }
  }
  return out;
}

void Remember(jmethodID mid, const char* sig) {
  if (mid == nullptr) return;
  std::string kinds = KindsOfSignature(sig);
  std::unique_lock<std::shared_mutex> lock(g_kinds_mutex);
  g_kinds[mid] = std::move(kinds);
}

// For a jmethodID that did not come through the proxy's GetMethodID (from
// FromReflectedMethod, say): Executable.getParameterTypes().
std::string KindsByReflection(JNIEnv* r, jmethodID mid, jclass cls, jobject obj,
                              bool is_static) {
  std::string out;
  jclass own = nullptr;
  if (cls == nullptr && obj != nullptr) cls = own = r->GetObjectClass(obj);
  jobject method = r->ToReflectedMethod(cls, mid, is_static ? JNI_TRUE : JNI_FALSE);
  jclass executable = r->FindClass("java/lang/reflect/Executable");
  jclass klass = r->FindClass("java/lang/Class");
  if (method != nullptr && executable != nullptr && klass != nullptr) {
    jmethodID get_types =
        r->GetMethodID(executable, "getParameterTypes", "()[Ljava/lang/Class;");
    jmethodID get_name = r->GetMethodID(klass, "getName", "()Ljava/lang/String;");
    auto types = static_cast<jobjectArray>(r->CallObjectMethodA(method, get_types, nullptr));
    jsize n = types != nullptr ? r->GetArrayLength(types) : 0;
    for (jsize i = 0; i < n; ++i) {
      jobject type = r->GetObjectArrayElement(types, i);
      auto name = static_cast<jstring>(r->CallObjectMethodA(type, get_name, nullptr));
      const char* s = name != nullptr ? r->GetStringUTFChars(name, nullptr) : nullptr;
      char kind = 'L';
      if (s != nullptr) {
        static const struct {
          const char* name;
          char kind;
        } kPrimitives[] = {{"boolean", 'Z'}, {"byte", 'B'}, {"char", 'C'}, {"short", 'S'},
                           {"int", 'I'},     {"long", 'J'}, {"float", 'F'}, {"double", 'D'}};
        for (const auto& p : kPrimitives) {
          if (std::strcmp(s, p.name) == 0) kind = p.kind;
        }
        r->ReleaseStringUTFChars(name, s);
      }
      out += kind;
      r->DeleteLocalRef(name);
      r->DeleteLocalRef(type);
    }
    r->DeleteLocalRef(types);
  }
  if (r->ExceptionCheck()) {
    std::fprintf(stderr, "gesher jni: cannot find the parameter types of a jmethodID\n");
    r->ExceptionDescribe();
    r->ExceptionClear();
  }
  r->DeleteLocalRef(method);
  r->DeleteLocalRef(executable);
  r->DeleteLocalRef(klass);
  r->DeleteLocalRef(own);
  return out;
}

std::string Kinds(JNIEnv* r, jmethodID mid, jclass cls, jobject obj, bool is_static) {
  {
    std::shared_lock<std::shared_mutex> lock(g_kinds_mutex);
    auto it = g_kinds.find(mid);
    if (it != g_kinds.end()) return it->second;
  }
  std::string kinds = KindsByReflection(r, mid, cls, obj, is_static);
  std::unique_lock<std::shared_mutex> lock(g_kinds_mutex);
  g_kinds[mid] = kinds;
  return kinds;
}

// The .so's variable arguments as a jvalue array.
class Args {
 public:
  Args(const std::string& kinds, SysvVaList* ap) : values_(kinds.size() + 1) {
    for (size_t i = 0; i < kinds.size(); ++i) {
      jvalue& v = values_[i];
      switch (kinds[i]) {
        case 'Z': v.z = static_cast<jboolean>(NextGp(ap)); break;
        case 'B': v.b = static_cast<jbyte>(NextGp(ap)); break;
        case 'C': v.c = static_cast<jchar>(NextGp(ap)); break;
        case 'S': v.s = static_cast<jshort>(NextGp(ap)); break;
        case 'I': v.i = static_cast<jint>(NextGp(ap)); break;
        case 'J': v.j = static_cast<jlong>(NextGp(ap)); break;
        case 'F': v.f = static_cast<jfloat>(NextFp(ap)); break;  // promoted to double
        case 'D': v.d = NextFp(ap); break;
        default: v.l = reinterpret_cast<jobject>(NextGp(ap)); break;
      }
    }
  }
  const jvalue* data() const { return values_.data(); }

 private:
  std::vector<jvalue> values_;
};

// --- the System V JNIEnv table ---------------------------------------------------

// A function with no varargs: forwarded as it is to the real JNIEnv.
template <auto M>
struct Fwd;
template <class R, class... A, R (*JNINativeInterface::*M)(JNIEnv*, A...)>
struct Fwd<M> {
  static R SYSV Call(JNIEnv* env, A... args) {
    JNIEnv* r = Real(env);
    return (r->functions->*M)(r, args...);
  }
};

jmethodID SYSV GetMethodIDSysv(JNIEnv* env, jclass c, const char* name, const char* sig) {
  JNIEnv* r = Real(env);
  jmethodID mid = r->functions->GetMethodID(r, c, name, sig);
  Remember(mid, sig);
  return mid;
}

jmethodID SYSV GetStaticMethodIDSysv(JNIEnv* env, jclass c, const char* name, const char* sig) {
  JNIEnv* r = Real(env);
  jmethodID mid = r->functions->GetStaticMethodID(r, c, name, sig);
  Remember(mid, sig);
  return mid;
}

jint SYSV GetJavaVMSysv(JNIEnv* env, JavaVM** vm) {
  JNIEnv* r = Real(env);
  JavaVM* real = nullptr;
  jint rc = r->functions->GetJavaVM(r, &real);
  if (rc == JNI_OK) {
    g_real_vm.store(real, std::memory_order_release);
    *vm = VmFor(IsGuestEnv(env));
  }
  return rc;
}

jint SYSV RegisterNativesSysv(JNIEnv* env, jclass c, const JNINativeMethod* methods, jint n) {
  JNIEnv* r = Real(env);
  std::vector<JNINativeMethod> entries(methods, methods + (n > 0 ? n : 0));
  for (JNINativeMethod& m : entries) m.fnPtr = NativeEntry(m.fnPtr);
  return r->functions->RegisterNatives(r, c, entries.data(), n);
}

// --- the System V JavaVM table ---------------------------------------------------

jint SYSV DestroyJavaVMSysv(JavaVM*) {
  JavaVM* vm = RealVm();
  return vm->functions->DestroyJavaVM(vm);
}

jint SYSV AttachCurrentThreadSysv(JavaVM* proxy, JNIEnv** penv, void* args) {
  EnsureThread();
  JavaVM* vm = RealVm();
  JNIEnv* env = nullptr;
  jint rc = vm->functions->AttachCurrentThread(vm, &env, args);
  if (penv != nullptr) *penv = rc == JNI_OK ? WrapFor(IsGuestVm(proxy), env) : nullptr;
  return rc;
}

jint SYSV AttachCurrentThreadAsDaemonSysv(JavaVM* proxy, JNIEnv** penv, void* args) {
  EnsureThread();
  JavaVM* vm = RealVm();
  JNIEnv* env = nullptr;
  jint rc = vm->functions->AttachCurrentThreadAsDaemon(vm, &env, args);
  if (penv != nullptr) *penv = rc == JNI_OK ? WrapFor(IsGuestVm(proxy), env) : nullptr;
  return rc;
}

jint SYSV DetachCurrentThreadSysv(JavaVM*) {
  JavaVM* vm = RealVm();
  jint rc = vm->functions->DetachCurrentThread(vm);
  if (rc == JNI_OK) {
    t_env.real = nullptr;
    t_env_guest.real = nullptr;
  }
  return rc;
}

jint SYSV GetEnvSysv(JavaVM* proxy, void** penv, jint version) {
  JavaVM* vm = RealVm();
  void* env = nullptr;
  jint rc = vm->functions->GetEnv(vm, &env, version);
  // A JNI version gets the proxy; anything else (JVMTI) is passed on as it is.
  if (rc == JNI_OK && (version & 0xffff0000) == 0x00010000) {
    env = WrapFor(IsGuestVm(proxy), static_cast<JNIEnv*>(env));
  }
  if (penv != nullptr) *penv = rc == JNI_OK ? env : nullptr;
  return rc;
}

// JNI_GetCreatedJavaVMs for the .so (libnativehelper's, API 31+).
jint SYSV GetCreatedJavaVMsSysv(JavaVM** vms, jsize size, jsize* count) {
  bool have = RealVm() != nullptr;
  if (have && size > 0 && vms != nullptr) vms[0] = VmFor(t_guest_mode);
  if (count != nullptr) *count = have ? 1 : 0;
  return JNI_OK;
}

}  // namespace

// --- the va_list and variadic forms -------------------------------------------

#define GESHER_JNI_TYPES(X) \
  X(Object, jobject)        \
  X(Boolean, jboolean)      \
  X(Byte, jbyte)            \
  X(Char, jchar)            \
  X(Short, jshort)          \
  X(Int, jint)              \
  X(Long, jlong)            \
  X(Float, jfloat)          \
  X(Double, jdouble)        \
  X(Void, void)

namespace {
// The JNIEnv slots that take a va_list (Call*MethodV, NewObjectV) and its
// argument index: the guest's AAPCS64 va_list is converted when it calls them.
void RegisterVaListSlots() {
#define VA_SLOT(name, index)                                                       \
  gesher::arm64::AddVaListFunction(g_env_table[offsetof(JNINativeInterface, name) / \
                                               sizeof(void*)],                     \
                                   index);
#define VA_SLOTS(Name, T)                   \
  VA_SLOT(Call##Name##MethodV, 3)           \
  VA_SLOT(CallNonvirtual##Name##MethodV, 4) \
  VA_SLOT(CallStatic##Name##MethodV, 3)
  GESHER_JNI_TYPES(VA_SLOTS)
  VA_SLOT(NewObjectV, 3)
#undef VA_SLOTS
#undef VA_SLOT
}
}  // namespace

extern "C" {

#define GESHER_JNI_DEFINE_V(Name, T)                                                       \
  T SYSV gesher_jni_Call##Name##MethodV(JNIEnv* env, jobject obj, jmethodID mid,           \
                                        SysvVaList* ap) {                                  \
    JNIEnv* r = Real(env);                                                                 \
    Args args(Kinds(r, mid, nullptr, obj, false), ap);                                     \
    return r->functions->Call##Name##MethodA(r, obj, mid, args.data());                    \
  }                                                                                        \
  T SYSV gesher_jni_CallNonvirtual##Name##MethodV(JNIEnv* env, jobject obj, jclass c,      \
                                                  jmethodID mid, SysvVaList* ap) {         \
    JNIEnv* r = Real(env);                                                                 \
    Args args(Kinds(r, mid, c, obj, false), ap);                                           \
    return r->functions->CallNonvirtual##Name##MethodA(r, obj, c, mid, args.data());       \
  }                                                                                        \
  T SYSV gesher_jni_CallStatic##Name##MethodV(JNIEnv* env, jclass c, jmethodID mid,        \
                                              SysvVaList* ap) {                            \
    JNIEnv* r = Real(env);                                                                 \
    Args args(Kinds(r, mid, c, nullptr, true), ap);                                        \
    return r->functions->CallStatic##Name##MethodA(r, c, mid, args.data());                \
  }                                                                                        \
  void gesher_jni_Call##Name##Method();                                                    \
  void gesher_jni_CallNonvirtual##Name##Method();                                          \
  void gesher_jni_CallStatic##Name##Method();
GESHER_JNI_TYPES(GESHER_JNI_DEFINE_V)

jobject SYSV gesher_jni_NewObjectV(JNIEnv* env, jclass c, jmethodID mid, SysvVaList* ap) {
  JNIEnv* r = Real(env);
  Args args(Kinds(r, mid, c, nullptr, false), ap);
  return r->functions->NewObjectA(r, c, mid, args.data());
}
void gesher_jni_NewObject();

JNIEnv* gesher_jni_enter(JNIEnv* env) {
  EnsureThread();
  t_guest_mode = false;
  if (env == nullptr) return env;
  if (t_env.real == env) return reinterpret_cast<JNIEnv*>(&t_env);
  // Is it this thread's JNIEnv? A @CriticalNative method's first argument is not.
  JavaVM* vm = RealVm();
  void* current = nullptr;
  if (vm != nullptr && vm->functions->GetEnv(vm, &current, JNI_VERSION_1_6) == JNI_OK &&
      current == env) {
    return Wrap(env);
  }
  return env;
}

jint gesher_jni_onload_bridge(JavaVM* vm, void* reserved, void* target) {
  g_real_vm.store(vm, std::memory_order_release);
  EnsureThread();
  return reinterpret_cast<jint(SYSV*)(JavaVM*, void*)>(target)(ProxyVmPtr(), reserved);
}

// The guest JNI_OnLoad bridge: `target` is a host_entry stub entering the guest
// JNI_OnLoad; the thread becomes guest-serving and gets the guest JavaVM proxy.
jint gesher_jni_onload_bridge_guest(JavaVM* vm, void* reserved, void* target) {
  t_guest_mode = true;
  g_real_vm.store(vm, std::memory_order_release);
  EnsureThread();
  return reinterpret_cast<jint(SYSV*)(JavaVM*, void*)>(target)(GuestVmPtr(), reserved);
}

// The guest native entry: as gesher_jni_enter, but hands the guest its proxy
// (the guest table over the same real JNIEnv).
JNIEnv* gesher_jni_enter_guest(JNIEnv* env) {
  EnsureThread();
  t_guest_mode = true;
  if (env == nullptr) return env;
  if (t_env.real == env || t_env_guest.real == env) return WrapGuest(env);
  JavaVM* vm = RealVm();
  void* current = nullptr;
  if (vm != nullptr && vm->functions->GetEnv(vm, &current, JNI_VERSION_1_6) == JNI_OK &&
      current == env) {
    return WrapGuest(env);
  }
  return env;
}

}  // extern "C"

// The variadic entry points: K fixed arguments (env, obj or class, [class,]
// method), the va_list as argument K+1 (GESHER_VA_TRAMPOLINE).
#define GESHER_JNI_TRAMPOLINES(Name, T)                                                    \
  GESHER_VA_TRAMPOLINE("gesher_jni_Call" #Name "Method", "gesher_jni_Call" #Name "MethodV", \
                       "24", "%rcx")                                                       \
  GESHER_VA_TRAMPOLINE("gesher_jni_CallNonvirtual" #Name "Method",                         \
                       "gesher_jni_CallNonvirtual" #Name "MethodV", "32", "%r8")           \
  GESHER_VA_TRAMPOLINE("gesher_jni_CallStatic" #Name "Method",                             \
                       "gesher_jni_CallStatic" #Name "MethodV", "24", "%rcx")
asm(".text\n"
    GESHER_JNI_TYPES(GESHER_JNI_TRAMPOLINES)
    GESHER_VA_TRAMPOLINE("gesher_jni_NewObject", "gesher_jni_NewObjectV", "24", "%rcx"));

// The entry of every native in a loaded .so: ART calls it (System V) through a
// stub that put the .so's function in %r11. The argument registers are saved,
// gesher_jni_enter (Microsoft ABI) sets the thread up and maps %rdi, ART's
// JNIEnv*, to the proxy; then the registers are restored and the .so's
// function is jumped to with the caller's stack arguments where they were.
// On entry %rsp is 8 mod 16; after the push and 224 bytes it is 16-aligned.
// The same body for the host entry (calls gesher_jni_enter) and the guest entry
// (calls gesher_jni_enter_guest, which sets the thread's guest mode).
#define GESHER_JNI_NATIVE_ENTRY(NAME, ENTER)                   \
  ".globl " NAME "\n"                                          \
  ".p2align 4\n" NAME ":\n"                                    \
  "  pushq %rbp\n"                                             \
  "  movq %rsp, %rbp\n"                                        \
  "  subq $224, %rsp\n"                                        \
  "  movq %rdi, 32(%rsp)\n"                                    \
  "  movq %rsi, 40(%rsp)\n"                                    \
  "  movq %rdx, 48(%rsp)\n"                                    \
  "  movq %rcx, 56(%rsp)\n"                                    \
  "  movq %r8, 64(%rsp)\n"                                     \
  "  movq %r9, 72(%rsp)\n"                                     \
  "  movq %r11, 80(%rsp)\n"                                    \
  "  movaps %xmm0, 96(%rsp)\n"                                 \
  "  movaps %xmm1, 112(%rsp)\n"                                \
  "  movaps %xmm2, 128(%rsp)\n"                                \
  "  movaps %xmm3, 144(%rsp)\n"                                \
  "  movaps %xmm4, 160(%rsp)\n"                                \
  "  movaps %xmm5, 176(%rsp)\n"                                \
  "  movaps %xmm6, 192(%rsp)\n"                                \
  "  movaps %xmm7, 208(%rsp)\n"                                \
  "  movq %rdi, %rcx\n"                                        \
  "  call " ENTER "\n"                                         \
  "  movq %rax, %rdi\n"                                        \
  "  movq 40(%rsp), %rsi\n"                                    \
  "  movq 48(%rsp), %rdx\n"                                    \
  "  movq 56(%rsp), %rcx\n"                                    \
  "  movq 64(%rsp), %r8\n"                                     \
  "  movq 72(%rsp), %r9\n"                                     \
  "  movq 80(%rsp), %r11\n"                                    \
  "  movaps 96(%rsp), %xmm0\n"                                 \
  "  movaps 112(%rsp), %xmm1\n"                                \
  "  movaps 128(%rsp), %xmm2\n"                                \
  "  movaps 144(%rsp), %xmm3\n"                                \
  "  movaps 160(%rsp), %xmm4\n"                                \
  "  movaps 176(%rsp), %xmm5\n"                                \
  "  movaps 192(%rsp), %xmm6\n"                                \
  "  movaps 208(%rsp), %xmm7\n"                                \
  "  movq %rbp, %rsp\n"                                        \
  "  popq %rbp\n"                                              \
  "  jmpq *%r11\n"
asm(".text\n" GESHER_JNI_NATIVE_ENTRY("gesher_jni_native_entry", "gesher_jni_enter")
        GESHER_JNI_NATIVE_ENTRY("gesher_jni_native_entry_guest", "gesher_jni_enter_guest"));

namespace {

void SetEnvSlot(size_t offset, void* fn) { g_env_table[offset / sizeof(void*)] = fn; }
void SetVmSlot(size_t offset, void* fn) { g_vm_table[offset / sizeof(void*)] = fn; }

void FillTables() {
#define FWD(name) \
  SetEnvSlot(offsetof(JNINativeInterface, name), \
             reinterpret_cast<void*>(&Fwd<&JNINativeInterface::name>::Call));
  FWD(GetVersion) FWD(DefineClass) FWD(FindClass) FWD(FromReflectedMethod)
  FWD(FromReflectedField) FWD(ToReflectedMethod) FWD(GetSuperclass) FWD(IsAssignableFrom)
  FWD(ToReflectedField) FWD(Throw) FWD(ThrowNew) FWD(ExceptionOccurred) FWD(ExceptionDescribe)
  FWD(ExceptionClear) FWD(FatalError) FWD(PushLocalFrame) FWD(PopLocalFrame) FWD(NewGlobalRef)
  FWD(DeleteGlobalRef) FWD(DeleteLocalRef) FWD(IsSameObject) FWD(NewLocalRef)
  FWD(EnsureLocalCapacity) FWD(AllocObject) FWD(NewObjectA) FWD(GetObjectClass)
  FWD(IsInstanceOf)
  FWD(CallObjectMethodA) FWD(CallBooleanMethodA) FWD(CallByteMethodA) FWD(CallCharMethodA)
  FWD(CallShortMethodA) FWD(CallIntMethodA) FWD(CallLongMethodA) FWD(CallFloatMethodA)
  FWD(CallDoubleMethodA) FWD(CallVoidMethodA)
  FWD(CallNonvirtualObjectMethodA) FWD(CallNonvirtualBooleanMethodA)
  FWD(CallNonvirtualByteMethodA) FWD(CallNonvirtualCharMethodA) FWD(CallNonvirtualShortMethodA)
  FWD(CallNonvirtualIntMethodA) FWD(CallNonvirtualLongMethodA) FWD(CallNonvirtualFloatMethodA)
  FWD(CallNonvirtualDoubleMethodA) FWD(CallNonvirtualVoidMethodA)
  FWD(GetFieldID) FWD(GetObjectField) FWD(GetBooleanField) FWD(GetByteField) FWD(GetCharField)
  FWD(GetShortField) FWD(GetIntField) FWD(GetLongField) FWD(GetFloatField) FWD(GetDoubleField)
  FWD(SetObjectField) FWD(SetBooleanField) FWD(SetByteField) FWD(SetCharField)
  FWD(SetShortField) FWD(SetIntField) FWD(SetLongField) FWD(SetFloatField) FWD(SetDoubleField)
  FWD(CallStaticObjectMethodA) FWD(CallStaticBooleanMethodA) FWD(CallStaticByteMethodA)
  FWD(CallStaticCharMethodA) FWD(CallStaticShortMethodA) FWD(CallStaticIntMethodA)
  FWD(CallStaticLongMethodA) FWD(CallStaticFloatMethodA) FWD(CallStaticDoubleMethodA)
  FWD(CallStaticVoidMethodA)
  FWD(GetStaticFieldID) FWD(GetStaticObjectField) FWD(GetStaticBooleanField)
  FWD(GetStaticByteField) FWD(GetStaticCharField) FWD(GetStaticShortField)
  FWD(GetStaticIntField) FWD(GetStaticLongField) FWD(GetStaticFloatField)
  FWD(GetStaticDoubleField) FWD(SetStaticObjectField) FWD(SetStaticBooleanField)
  FWD(SetStaticByteField) FWD(SetStaticCharField) FWD(SetStaticShortField)
  FWD(SetStaticIntField) FWD(SetStaticLongField) FWD(SetStaticFloatField)
  FWD(SetStaticDoubleField)
  FWD(NewString) FWD(GetStringLength) FWD(GetStringChars) FWD(ReleaseStringChars)
  FWD(NewStringUTF) FWD(GetStringUTFLength) FWD(GetStringUTFChars) FWD(ReleaseStringUTFChars)
  FWD(GetArrayLength) FWD(NewObjectArray) FWD(GetObjectArrayElement) FWD(SetObjectArrayElement)
  FWD(NewBooleanArray) FWD(NewByteArray) FWD(NewCharArray) FWD(NewShortArray) FWD(NewIntArray)
  FWD(NewLongArray) FWD(NewFloatArray) FWD(NewDoubleArray)
  FWD(GetBooleanArrayElements) FWD(GetByteArrayElements) FWD(GetCharArrayElements)
  FWD(GetShortArrayElements) FWD(GetIntArrayElements) FWD(GetLongArrayElements)
  FWD(GetFloatArrayElements) FWD(GetDoubleArrayElements)
  FWD(ReleaseBooleanArrayElements) FWD(ReleaseByteArrayElements)
  FWD(ReleaseCharArrayElements) FWD(ReleaseShortArrayElements) FWD(ReleaseIntArrayElements)
  FWD(ReleaseLongArrayElements) FWD(ReleaseFloatArrayElements) FWD(ReleaseDoubleArrayElements)
  FWD(GetBooleanArrayRegion) FWD(GetByteArrayRegion) FWD(GetCharArrayRegion)
  FWD(GetShortArrayRegion) FWD(GetIntArrayRegion) FWD(GetLongArrayRegion)
  FWD(GetFloatArrayRegion) FWD(GetDoubleArrayRegion)
  FWD(SetBooleanArrayRegion) FWD(SetByteArrayRegion) FWD(SetCharArrayRegion)
  FWD(SetShortArrayRegion) FWD(SetIntArrayRegion) FWD(SetLongArrayRegion)
  FWD(SetFloatArrayRegion) FWD(SetDoubleArrayRegion)
  FWD(UnregisterNatives) FWD(MonitorEnter) FWD(MonitorExit) FWD(GetStringRegion)
  FWD(GetStringUTFRegion) FWD(GetPrimitiveArrayCritical) FWD(ReleasePrimitiveArrayCritical)
  FWD(GetStringCritical) FWD(ReleaseStringCritical) FWD(NewWeakGlobalRef)
  FWD(DeleteWeakGlobalRef) FWD(ExceptionCheck) FWD(NewDirectByteBuffer)
  FWD(GetDirectBufferAddress) FWD(GetDirectBufferCapacity) FWD(GetObjectRefType)
#undef FWD

#define SPECIAL(name, fn) \
  SetEnvSlot(offsetof(JNINativeInterface, name), reinterpret_cast<void*>(&fn));
  SPECIAL(GetMethodID, GetMethodIDSysv)
  SPECIAL(GetStaticMethodID, GetStaticMethodIDSysv)
  SPECIAL(GetJavaVM, GetJavaVMSysv)
  SPECIAL(RegisterNatives, RegisterNativesSysv)
  SPECIAL(NewObject, gesher_jni_NewObject)
  SPECIAL(NewObjectV, gesher_jni_NewObjectV)
#define VARARGS(Name, T)                                                   \
  SPECIAL(Call##Name##Method, gesher_jni_Call##Name##Method)               \
  SPECIAL(Call##Name##MethodV, gesher_jni_Call##Name##MethodV)             \
  SPECIAL(CallNonvirtual##Name##Method, gesher_jni_CallNonvirtual##Name##Method)   \
  SPECIAL(CallNonvirtual##Name##MethodV, gesher_jni_CallNonvirtual##Name##MethodV) \
  SPECIAL(CallStatic##Name##Method, gesher_jni_CallStatic##Name##Method)   \
  SPECIAL(CallStatic##Name##MethodV, gesher_jni_CallStatic##Name##MethodV)
  GESHER_JNI_TYPES(VARARGS)
#undef VARARGS
#undef SPECIAL

  SetVmSlot(offsetof(JNIInvokeInterface, DestroyJavaVM),
            reinterpret_cast<void*>(&DestroyJavaVMSysv));
  SetVmSlot(offsetof(JNIInvokeInterface, AttachCurrentThread),
            reinterpret_cast<void*>(&AttachCurrentThreadSysv));
  SetVmSlot(offsetof(JNIInvokeInterface, DetachCurrentThread),
            reinterpret_cast<void*>(&DetachCurrentThreadSysv));
  SetVmSlot(offsetof(JNIInvokeInterface, GetEnv), reinterpret_cast<void*>(&GetEnvSysv));
  SetVmSlot(offsetof(JNIInvokeInterface, AttachCurrentThreadAsDaemon),
            reinterpret_cast<void*>(&AttachCurrentThreadAsDaemonSysv));

  for (size_t i = 4; i < kEnvSlots; ++i) {
    if (g_env_table[i] == nullptr) {
      std::fprintf(stderr, "gesher jni: JNIEnv slot %zu has no System V entry\n", i);
      std::abort();
    }
  }
}

// --- gesher-base's dlopen of ELF files -----------------------------------------

std::once_flag g_install_once;

void Install() {
  std::call_once(g_install_once, [] {
    FillTables();
    gesher::bionic::InstallBionicShim();
    gesher::bionic::AddBionicSymbol("JNI_GetCreatedJavaVMs",
                                    reinterpret_cast<void*>(&GetCreatedJavaVMsSysv));
  });
}

void* ElfOpen(const char* path, int /*flags*/, char* error, size_t error_size) {
  Install();
  elf::LoadOptions options;
  // DT_NEEDED libraries the APK ships (libc++_shared.so, ...) sit next to it.
  std::string dir(path);
  size_t slash = dir.find_last_of("/\\");
  dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
  options.search_paths.push_back(dir);
  std::string message;
  elf::Library* lib = elf::Load(path, &message, options);
  if (lib == nullptr) std::snprintf(error, error_size, "%s", message.c_str());
  // The app's library directory: where its dlopen() of a bare name looks.
  if (lib != nullptr) gesher::bionic::NoteLibraryDirectory(dir);
  if (lib != nullptr && elf::IsAarch64(lib)) {
    // An app's arm64-v8a library (System.loadLibrary): say so in its log.
    std::fprintf(stderr, "gesher arm64: %s is arm64-v8a: translated (gesher_arm64.dll)\n",
                 elf::Soname(lib));
  }
  return lib;
}

void* ElfSym(void* handle, const char* symbol) {
  void* p = elf::Lookup(static_cast<elf::Library*>(handle), symbol);
  if (p == nullptr) return nullptr;
  if (std::strcmp(symbol, "JNI_OnLoad") == 0 || std::strcmp(symbol, "JNI_OnUnload") == 0) {
    return OnLoadEntry(p);
  }
  if (std::strncmp(symbol, "Java_", 5) == 0) return NativeEntry(p);
  return p;
}

int ElfClose(void* handle) {
  elf::Close(static_cast<elf::Library*>(handle));
  return 0;
}

const gesher_elf_dlopen_ops kElfOps = {&ElfOpen, &ElfSym, &ElfClose};

[[maybe_unused]] const bool g_registered = (gesher_set_elf_dlopen(&kElfOps), true);

}  // namespace

JNIEnv* gesher::bionic::RealJniEnv(JNIEnv* env) {
  if (env == nullptr) return nullptr;
  auto* proxy = reinterpret_cast<ProxyEnv*>(env);
  return (proxy->functions == g_env_table || proxy->functions == g_env_table_guest) ? proxy->real
                                                                                    : env;
}

JNIEnv* gesher::bionic::ProxyJniEnv(JNIEnv* real) {
  Install();
  EnsureThread();
  // A guest (arm64) .so may be handed this proxy (NativeActivity): its calls
  // through the host table reach the host entries directly (the translator's
  // host-pointer path), which must know the va_list ones.
  if (gesher::arm64::Available()) BuildGuestTables();
  if (real == nullptr) return nullptr;
  void* const* f = reinterpret_cast<ProxyEnv*>(real)->functions;
  if (f == g_env_table || f == g_env_table_guest) return real;
  return Wrap(real);
}

JavaVM* gesher::bionic::ProxyJavaVm(JNIEnv* real) {
  Install();
  JNIEnv* r = RealJniEnv(real);
  JavaVM* vm = nullptr;
  if (r != nullptr && r->functions->GetJavaVM(r, &vm) == JNI_OK) {
    g_real_vm.store(vm, std::memory_order_release);
  }
  return ProxyVmPtr();
}

void gesher::bionic::EnterLoadedCode() { EnsureThread(); }

