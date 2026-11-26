// gesher: execute real ARM/Thumb JNI32 table calls against a host JNIEnv.
// The host is a narrow test double. Real ART/Windows execution is still
// required before enabling APK admission.
#include <gesher/arm32_jni.h>

#include <jni.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using namespace gesher::arm32;
namespace {

void Check(bool passed, const char* message) {
  if (!passed) { std::cerr << "ARM32 JNI32: " << message << '\n'; std::exit(1); }
}
std::vector<uint8_t> Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  Check(bool(file), "open compiled JNI32 ARM/Thumb ELF32");
  return {std::istreambuf_iterator<char>(file), {}};
}
struct FakeObject {
  std::string value;
  bool klass;
};
struct FakeState {
  std::vector<std::unique_ptr<FakeObject>> objects;
  FakeObject* class_object = nullptr;
  unsigned locals = 0;
  unsigned globals = 0;
  unsigned utf_acquired = 0;
  unsigned utf_released = 0;
  FakeObject* Add(std::string value, bool klass) {
    auto object = std::make_unique<FakeObject>();
    object->value = std::move(value);
    object->klass = klass;
    FakeObject* raw = object.get();
    objects.push_back(std::move(object));
    return raw;
  }
};
FakeState* state = nullptr;
FakeObject* Object(jobject value) { return reinterpret_cast<FakeObject*>(value); }
jobject Ref(FakeObject* object) { return reinterpret_cast<jobject>(object); }
void RequireState(JNIEnv* env) { Check(env && state, "host JNIEnv is live"); }

jint JNICALL Version(JNIEnv* env) {
  RequireState(env);
  return JNI_VERSION_1_6;
}
jboolean JNICALL ExceptionCheck(JNIEnv* env) {
  RequireState(env);
  return JNI_FALSE;
}
void JNICALL ExceptionClear(JNIEnv* env) { RequireState(env); }
jthrowable JNICALL ExceptionOccurred(JNIEnv* env) {
  RequireState(env);
  return nullptr;
}
jclass JNICALL FindClass(JNIEnv* env, const char* name) {
  RequireState(env);
  if (!name || std::string(name) != "java/lang/String") return nullptr;
  ++state->locals;
  return reinterpret_cast<jclass>(state->class_object);
}
jstring JNICALL NewStringUTF(JNIEnv* env, const char* value) {
  RequireState(env);
  if (!value) return nullptr;
  ++state->locals;
  return reinterpret_cast<jstring>(state->Add(value, false));
}
jint JNICALL GetStringUTFLength(JNIEnv* env, jstring value) {
  RequireState(env);
  return static_cast<jint>(Object(value)->value.size());
}
jsize JNICALL GetStringLength(JNIEnv* env, jstring value) {
  RequireState(env);
  return static_cast<jsize>(Object(value)->value.size());
}
const char* JNICALL GetStringUTFChars(JNIEnv* env, jstring value, jboolean* is_copy) {
  RequireState(env);
  ++state->utf_acquired;
  if (is_copy) *is_copy = JNI_FALSE;
  return Object(value)->value.c_str();
}
void JNICALL ReleaseStringUTFChars(JNIEnv* env, jstring, const char*) {
  RequireState(env);
  ++state->utf_released;
}
jobject JNICALL NewLocalRef(JNIEnv* env, jobject value) {
  RequireState(env);
  if (value) ++state->locals;
  return value;
}
jobject JNICALL NewGlobalRef(JNIEnv* env, jobject value) {
  RequireState(env);
  if (value) ++state->globals;
  return value;
}
void JNICALL DeleteLocalRef(JNIEnv* env, jobject value) {
  RequireState(env);
  if (value) {
    Check(state->locals > 0, "local reference balance");
    --state->locals;
  }
}
void JNICALL DeleteGlobalRef(JNIEnv* env, jobject value) {
  RequireState(env);
  if (value) {
    Check(state->globals > 0, "global reference balance");
    --state->globals;
  }
}
jboolean JNICALL IsSameObject(JNIEnv* env, jobject a, jobject b) {
  RequireState(env);
  return a == b ? JNI_TRUE : JNI_FALSE;
}
jclass JNICALL GetObjectClass(JNIEnv* env, jobject value) {
  RequireState(env);
  if (!value) return nullptr;
  ++state->locals;
  return reinterpret_cast<jclass>(state->class_object);
}

void Run(const char* path) {
  FakeState fake;
  fake.class_object = fake.Add("java/lang/String", true);
  state = &fake;
  JNINativeInterface_ functions{};
  functions.GetVersion = Version;
  functions.ExceptionCheck = ExceptionCheck;
  functions.ExceptionClear = ExceptionClear;
  functions.ExceptionOccurred = ExceptionOccurred;
  functions.FindClass = FindClass;
  functions.NewStringUTF = NewStringUTF;
  functions.GetStringUTFLength = GetStringUTFLength;
  functions.GetStringLength = GetStringLength;
  functions.GetStringUTFChars = GetStringUTFChars;
  functions.ReleaseStringUTFChars = ReleaseStringUTFChars;
  functions.NewLocalRef = NewLocalRef;
  functions.NewGlobalRef = NewGlobalRef;
  functions.DeleteLocalRef = DeleteLocalRef;
  functions.DeleteGlobalRef = DeleteGlobalRef;
  functions.IsSameObject = IsSameObject;
  functions.GetObjectClass = GetObjectClass;
  JNIEnv env{};
  env.functions = &functions;

  const auto bytes = Read(path);
  Program program([&](std::string_view name, std::vector<uint8_t>* output, std::string*) {
    if (name != "gesher_arm32_jni_fixture.so") return false;
    *output = bytes;
    return true;
  });
  Jni32 bridge(program, &env);
  std::string error;
  if (!bridge.Install(&error)) std::cerr << "install JNI32: " << error << '\n';
  Check(bridge.GuestEnvironment() != 0, "install real 32-bit JNIEnv and function table");
  int registration_calls = 0;
  std::vector<RegisteredNative32> observed_natives;
  // The fake environment is intentionally NOT a real ART trampoline
  // installer. Returning false proves JNI_ERR reaches the guest instead of
  // claiming native registration succeeded without callable host methods.
  Check(bridge.SetNativeRegistrationHandler(
          [&](jclass klass, const std::vector<RegisteredNative32>& methods,
              std::string*) {
            Check(klass == reinterpret_cast<jclass>(fake.class_object),
                  "registered JNI class matches real host object");
            ++registration_calls;
            observed_natives = methods;
            return false;
          }, &error),
        "attach explicit rejecting ART registration handler");
  uint32_t table = 0, version = 0, missing = 1;
  Check(program.Memory().Read32(bridge.GuestEnvironment(), &table) &&
        program.Memory().Read32(table + 4 * 4, &version) && version != 0 &&
        program.Memory().Read32(table + 5 * 4, &missing) && missing == 0,
        "JNI32 has guest thunk entry and fails closed for missing methods");
  Check(!program.Memory().Write32(table + 4 * 4, 0),
        "JNI32 table is read-only to guest code");
  uint32_t vm_table = 0, get_env = 0;
  Check(bridge.GuestJavaVM() != 0 &&
        program.Memory().Read32(bridge.GuestJavaVM(), &vm_table) &&
        program.Memory().Read32(vm_table + 6 * 4, &get_env) &&
        get_env != 0 &&
        !program.Memory().Write32(vm_table + 6 * 4, 0),
        "guest JavaVM GetEnv slot exists with read-only function table");

  if (!program.LoadLibrary("gesher_arm32_jni_fixture.so", &error))
    std::cerr << "load JNI32: " << error << '\n';
  Check(program.LibraryCount() == 1, "load compiled JNI32 guest ELF");
  Check(program.Initialize(&error), "initialize guest JNI library");

  jint onload_version = 0;
  if (!bridge.CallOnLoad("gesher_arm32_jni_fixture.so", &onload_version, &error))
    std::cerr << "JNI32 OnLoad: " << error << '\n';
  Check(onload_version == JNI_VERSION_1_6 && error.empty(),
        "real guest JNI_OnLoad retrieves JNIEnv through 32-bit JavaVM");
  Value load_state;
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_onload_state",
          {}, ValueKind::I32, &load_state, &error) &&
        load_state.bits == 1,
        "JNI32 guest JNI_OnLoad ran and updated guest library state");

  Value vm_result;
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_vm_bad_version",
          {Value::GuestPointer(bridge.GuestJavaVM())}, ValueKind::I32,
          &vm_result, &error) &&
        static_cast<int32_t>(vm_result.bits) == JNI_EVERSION,
        "JavaVM GetEnv returns JNI_EVERSION for unsupported JNI version");
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_vm_unknown_version",
          {Value::GuestPointer(bridge.GuestJavaVM())}, ValueKind::I32,
          &vm_result, &error) &&
        static_cast<int32_t>(vm_result.bits) == JNI_EVERSION,
        "JavaVM GetEnv rejects an undefined JNI version between 1.4 and 1.6");
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_vm_attach",
          {Value::GuestPointer(bridge.GuestJavaVM())}, ValueKind::I32,
          &vm_result, &error) &&
        static_cast<int32_t>(vm_result.bits) == 77,
        "JavaVM attach returns the real owning JNI thread, detachment fails closed");
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_env_get_vm",
          {Value::GuestPointer(bridge.GuestEnvironment())}, ValueKind::I32,
          &vm_result, &error) &&
        static_cast<int32_t>(vm_result.bits) == 88,
        "guest JNIEnv GetJavaVM and JavaVM GetEnv round-trip");
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_get_created_vm",
          {Value::GuestPointer(bridge.GuestEnvironment())}, ValueKind::I32,
          &vm_result, &error) &&
        static_cast<int32_t>(vm_result.bits) == 99,
        "guest ELF import JNI_GetCreatedJavaVMs resolves a typed 32-bit host thunk");

  auto call = [&](const char* name, Value* value) {
    return program.CallExport("gesher_arm32_jni_fixture.so", name,
            {Value::GuestPointer(bridge.GuestEnvironment())}, ValueKind::I32,
            value, &error);
  };
  Value result;
  Check(call("jni32_version", &result) && result.bits == JNI_VERSION_1_6,
        "real guest JNI GetVersion");
  if (!call("jni32_contract", &result)) std::cerr << "jni32_contract: " << error << '\n';
  Check(result.bits == 42, "guest JNI references, UTF strings and ABI methods");
  Check(fake.globals == 0 && fake.utf_acquired == fake.utf_released &&
        fake.utf_acquired == 1, "global refs and UTF buffers released exactly once");
  Check(bridge.ReleaseLocals(&error) && fake.locals == 0,
        "local references retired at explicit JNI frame boundary");

  Value register_result;
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_register_natives",
          {Value::GuestPointer(bridge.GuestEnvironment())}, ValueKind::I32,
          &register_result, &error) &&
        static_cast<int32_t>(register_result.bits) == JNI_ERR &&
        registration_calls == 1 && observed_natives.size() == 1 &&
        observed_natives[0].name == "nativeAdd" &&
        observed_natives[0].descriptor == "(II)I" &&
        observed_natives[0].guest_entry != 0,
        "real ARM/Thumb RegisterNatives is decoded but refuses fake ART success");
  Check(!program.CallExport("gesher_arm32_jni_fixture.so",
          "jni32_invalid_registration",
          {Value::GuestPointer(bridge.GuestEnvironment())}, ValueKind::I32,
          &register_result, &error) &&
        registration_calls == 1,
        "non-executable guest native entry fails before calling ART handler");
  Check(bridge.ReleaseLocals(&error) && fake.locals == 0,
        "RegisterNatives JNI local class reference cleaned up after failure");

  // ART -> ARM32: primitive, reference and 64-bit CriticalNative method calls
  // must pass through descriptor-aware AAPCS32 marshalling, never through a
  // narrowed 64-bit Java object pointer.
  const jobject receiver = Ref(fake.class_object);
  jvalue inputs[2]{};
  jvalue host_result{};
  inputs[0].i = 9;
  inputs[1].i = 21;
  if (!bridge.CallNative("gesher_arm32_jni_fixture.so",
                         "Java_gesher_Test_add", "(II)I",
                         JniCallMode::Instance, receiver, inputs, 2,
                         &host_result, &error))
    std::cerr << "JNI32 host-to-guest add: " << error << '\n';
  Check(host_result.i == 31 && error.empty() && fake.locals == 0,
        "ART-to-ARM32 named native integer call and local ref cleanup");

  jobject host_string = Ref(fake.Add("abcdef", false));
  inputs[0].l = host_string;
  Check(bridge.CallNative("gesher_arm32_jni_fixture.so",
                          "Java_gesher_Test_length", "(Ljava/lang/String;)I",
                          JniCallMode::Instance, receiver, inputs, 1,
                          &host_result, &error) &&
        host_result.i == 6 && fake.locals == 0,
        "ART jobject argument is a managed 32-bit guest token");

  Check(bridge.CallNative("gesher_arm32_jni_fixture.so",
                          "Java_gesher_Test_echo",
                          "(Ljava/lang/String;)Ljava/lang/String;",
                          JniCallMode::Instance, receiver, inputs, 1,
                          &host_result, &error) &&
        host_result.l == host_string && fake.locals == 1,
        "guest jobject return becomes a new host JNI local reference");
  DeleteLocalRef(&env, host_result.l);
  Check(fake.locals == 0, "caller retains ownership of returned host reference");

  inputs[0].j = 0x100000002LL;
  inputs[1].i = 7;
  Check(bridge.CallNative("gesher_arm32_jni_fixture.so",
                          "Java_gesher_Test_critical", "(JI)J",
                          JniCallMode::CriticalNative, nullptr, inputs, 2,
                          &host_result, &error) &&
        host_result.j == 0x100000009LL && fake.locals == 0,
        "CriticalNative has no JNIEnv, with aligned 64-bit AAPCS32 arguments");

  host_result.i = 123;
  Check(!bridge.CallNative("gesher_arm32_jni_fixture.so",
                           "Java_gesher_Test_add", "(II)I",
                           JniCallMode::Instance, receiver, inputs, 1,
                           &host_result, &error) &&
        host_result.i == 123 && fake.locals == 0,
        "wrong JNI descriptor argument count fails without changing result");

  Check(!call("jni32_stale", &result) && !error.empty(),
        "stale JNI guest reference must not resolve");
  Check(!call("jni32_wrong_env", &result) && !error.empty(),
        "forged guest JNIEnv pointer must be rejected");
  Check(!call("jni32_unsupported", &result) && !error.empty(),
        "unsupported JNI method is not reported successful");
  Check(call("jni32_version", &result) && result.bits == JNI_VERSION_1_6,
        "JNI32 recovers after rejected guest operations");
  Check(bridge.ReleaseLocals(&error), "guest local reference frame complete");
  Check(bridge.CallOnUnload("gesher_arm32_jni_fixture.so", &error),
        "guest JNI_OnUnload executes with JavaVM32 before library teardown");
  Check(program.CallExport("gesher_arm32_jni_fixture.so", "jni32_onload_state",
          {}, ValueKind::I32, &load_state, &error) &&
        load_state.bits == 0,
        "guest JNI_OnUnload cleared its actual native state");
  Check(program.Shutdown(&error), "close guest JNI module");

  {
    Program retired([](std::string_view, std::vector<uint8_t>*, std::string*) {
      return false;
    });
    uint32_t env_address = 0, vm_address = 0;
    {
      Jni32 ephemeral(retired, &env);
      Check(ephemeral.Install(&error), "install temporary JNI bridge");
      env_address = ephemeral.GuestEnvironment();
      vm_address = ephemeral.GuestJavaVM();
      Check(retired.Memory().IsMapped(env_address, 4096) &&
            retired.Memory().IsMapped(vm_address, 4096),
            "temporary bridge owns live guest JNIEnv and JavaVM pages");
    }
    Check(!retired.Memory().IsMapped(env_address, 4096) &&
          !retired.Memory().IsMapped(vm_address, 4096) &&
          !retired.Memory().IsMapped(0xd0004000, 4096) &&
          !retired.Memory().IsMapped(0xd0006000, 4096),
          "destroyed JNI bridge retires all four owned guest mapping pages");
  }
  for (uint32_t occupied : {0xd0005000u, 0xd0006000u}) {
    Program collision([](std::string_view, std::vector<uint8_t>*, std::string*) {
      return false;
    });
    constexpr auto rw = MemoryPermission::kRead | MemoryPermission::kWrite;
    Check(collision.Memory().Map(occupied, 4096, rw) &&
          collision.Memory().Write32(occupied, 0xfeedface),
          "set up pre-existing guest JavaVM page");
    Jni32 cannot_install(collision, &env);
    Check(!cannot_install.Install(&error) &&
          cannot_install.GuestEnvironment() == 0,
          "JNI32 refuses occupied guest JavaVM pages");
    uint32_t preserved = 0;
    Check(collision.Memory().Read32(occupied, &preserved) &&
          preserved == 0xfeedface &&
          !collision.Memory().IsMapped(0xd0003000, 4096) &&
          !collision.Memory().IsMapped(0xd0004000, 4096),
          "failed JNI32 installation preserves foreign page and rolls back own");
  }
  state = nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "compiled JNI32 ARM/Thumb fixture argument");
  Run(argv[1]);
  std::cout << "ARM32 JNI pointer-width and reference boundary passed\n";
  return 0;
}
