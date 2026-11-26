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
  uint32_t table = 0, version = 0, missing = 1;
  Check(program.Memory().Read32(bridge.GuestEnvironment(), &table) &&
        program.Memory().Read32(table + 4 * 4, &version) && version != 0 &&
        program.Memory().Read32(table + 5 * 4, &missing) && missing == 0,
        "JNI32 has guest thunk entry and fails closed for missing methods");
  Check(!program.Memory().Write32(table + 4 * 4, 0),
        "JNI32 table is read-only to guest code");

  if (!program.LoadLibrary("gesher_arm32_jni_fixture.so", &error))
    std::cerr << "load JNI32: " << error << '\n';
  Check(program.LibraryCount() == 1, "load compiled JNI32 guest ELF");
  Check(program.Initialize(&error), "initialize guest JNI library");
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
  Check(program.Shutdown(&error), "close guest JNI module");
  state = nullptr;
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "compiled JNI32 ARM/Thumb fixture argument");
  Run(argv[1]);
  std::cout << "ARM32 JNI pointer-width and reference boundary passed\n";
  return 0;
}
