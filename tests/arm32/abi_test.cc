// gesher: explicit AAPCS32 layout, descriptor rejection, and token lifetime checks.
#include <gesher/arm32_abi.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using gesher::arm32::CallFrame;
using gesher::arm32::DecodeReturn;
using gesher::arm32::EncodeReturn;
using gesher::arm32::JniCallMode;
using gesher::arm32::JniMethodSignature;
using gesher::arm32::OpaqueHandleKind;
using gesher::arm32::OpaqueHandleTable;
using gesher::arm32::PackArguments;
using gesher::arm32::PackJniCall;
using gesher::arm32::ParseJniMethodDescriptor;
using gesher::arm32::StackPointerFor;
using gesher::arm32::UnpackArguments;
using gesher::arm32::Value;
using gesher::arm32::ValueKind;

namespace {
int failures = 0;
int checks = 0;

void Check(bool condition, int line, const char* expression) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "abi_test.cc:%d: %s failed\n", line, expression);
  }
}
#define CHECK(...) Check((__VA_ARGS__), __LINE__, #__VA_ARGS__)

void CheckPlacement() {
  CallFrame frame;
  std::string error;
  // AAPCS32's even-register rule leaves r1 unused, and a stack doubleword
  // after an int leaves one stack padding word. Values are hand calculated.
  CHECK(PackArguments({Value::Signed32(1), Value::Float64(2),
                       Value::Signed32(3), Value::Signed64(4)}, &frame, &error));
  CHECK(frame.core == (std::array<uint32_t, 4>{1, 0, 0, 0x40000000}));
  CHECK(frame.stack == (std::vector<uint32_t>{3, 0, 4, 0}));
  CHECK(error.empty());

  // A fundamental 64-bit argument must spill whole; its low word cannot
  // use r3 while the high word uses SP. The next scalar follows both words.
  CHECK(PackArguments({Value::Unsigned32(1), Value::Unsigned32(2),
                       Value::Unsigned32(3), Value::Unsigned64(0x1122334455667788),
                       Value::Unsigned32(9)}, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{1, 2, 3, 0}));
  CHECK(frame.stack == (std::vector<uint32_t>{0x55667788, 0x11223344, 9, 0}));

  // Android softfp sends FP bits through core registers, including signed
  // zero and a NaN payload. No numeric conversion is permitted.
  CHECK(PackArguments({Value::Float32(-0.0f), Value::Float32(1.0f),
                       {ValueKind::F64, 0x7ff8123456789abc},
                       Value::GuestPointer(0xffffffff)}, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{0x80000000, 0x3f800000,
                                             0x56789abc, 0x7ff81234}));
  CHECK(frame.stack == (std::vector<uint32_t>{0xffffffff, 0}));

  CHECK(PackArguments({Value::Unsigned32(1), Value::Unsigned32(2),
                       Value::Unsigned32(3), Value::Unsigned32(4),
                       Value::Unsigned32(5), Value::Float64(3)}, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{1, 2, 3, 4}));
  CHECK(frame.stack == (std::vector<uint32_t>{5, 0, 0, 0x40080000}));
  uint32_t sp = 42;
  CHECK(StackPointerFor(frame, 0x1000, &sp));
  CHECK(sp == 0xff0);
  CHECK(!StackPointerFor(frame, 0x1004, &sp));
  CHECK(sp == 0xff0);
  CHECK(!StackPointerFor(frame, 8, &sp));
  CHECK(sp == 0xff0);

  // Read an independently specified incoming frame, not only an encoded
  // frame produced by the code under test.
  CallFrame incoming{{0xfffffffe, 0xfeedface, 0x55667788, 0x11223344},
                     {0x3f800000, 0xaaaaaaaa, 0x89abcdef, 0x76543210}};
  std::vector<Value> decoded;
  CHECK(UnpackArguments({ValueKind::I32, ValueKind::U64, ValueKind::F32,
                         ValueKind::I64}, incoming, &decoded));
  CHECK(decoded.size() == 4);
  if (decoded.size() == 4) {
    CHECK(decoded[0].bits == 0xfffffffe);
    CHECK(decoded[1].bits == 0x1122334455667788);
    CHECK(decoded[2].bits == 0x3f800000);
    CHECK(decoded[3].bits == 0x7654321089abcdef);
  }
  CHECK(PackArguments({}, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{0, 0, 0, 0}));
  CHECK(frame.stack.empty());
}

void CheckFailures() {
  CallFrame frame{{1, 2, 3, 4}, {5, 6}};
  std::string error;
  CHECK(!PackArguments({{ValueKind::Pointer32, uint64_t{1} << 32}}, &frame, &error));
  CHECK(!error.empty());
  CHECK(frame.core == (std::array<uint32_t, 4>{1, 2, 3, 4}));
  CHECK(frame.stack == (std::vector<uint32_t>{5, 6}));
  CHECK(!PackArguments({Value::GuestPointer(uint64_t{1} << 32)}, &frame));
  CHECK(!PackArguments({{ValueKind::I32, UINT64_MAX}}, &frame));
  CHECK(!PackArguments({{ValueKind::F32, uint64_t{1} << 32}}, &frame));
  CHECK(!PackArguments({{ValueKind::Void, 0}}, &frame));
  CHECK(!PackArguments({{static_cast<ValueKind>(255), 0}}, &frame));
  CHECK(!PackArguments({}, nullptr));
  CHECK(!UnpackArguments({}, frame, nullptr));
  std::vector<Value> decoded{Value::Unsigned32(123)};
  CHECK(!UnpackArguments({ValueKind::I64, ValueKind::I64, ValueKind::I64,
                          ValueKind::I64}, frame, &decoded));
  CHECK(decoded.size() == 1 && decoded[0].bits == 123);
  CHECK(!UnpackArguments({ValueKind::Void}, frame, &decoded));
  frame.stack.push_back(7);
  CHECK(!UnpackArguments({}, frame, &decoded));
  uint32_t sp = 123;
  CHECK(!StackPointerFor(frame, 0x1000, &sp));
  CHECK(sp == 123);
  CHECK(!StackPointerFor(frame, 0x1000, nullptr));
}

void CheckReturns() {
  std::array<uint32_t, 2> registers{1, 2};
  CHECK(EncodeReturn(Value::Signed32(-1), &registers));
  CHECK(registers == (std::array<uint32_t, 2>{0xffffffff, 0}));
  CHECK(EncodeReturn(Value::Signed64(-2), &registers));
  CHECK(registers == (std::array<uint32_t, 2>{0xfffffffe, 0xffffffff}));
  CHECK(EncodeReturn({ValueKind::F64, 0x7ff8123456789abc}, &registers));
  CHECK(registers == (std::array<uint32_t, 2>{0x56789abc, 0x7ff81234}));
  Value value;
  CHECK(DecodeReturn(ValueKind::F64, {0x56789abc, 0x7ff81234}, &value));
  CHECK(value.kind == ValueKind::F64 && value.bits == 0x7ff8123456789abc);
  CHECK(DecodeReturn(ValueKind::F32, {0x7fc12345, 0xffffffff}, &value));
  CHECK(value.kind == ValueKind::F32 && value.bits == 0x7fc12345);
  CHECK(DecodeReturn(ValueKind::Pointer32, {0xfedcba98, 0xffffffff}, &value));
  CHECK(value.bits == 0xfedcba98);
  CHECK(DecodeReturn(ValueKind::Void, {0xffffffff, 0xffffffff}, &value));
  CHECK(value.kind == ValueKind::Void && value.bits == 0);
  CHECK(EncodeReturn(value, &registers));
  CHECK(registers == (std::array<uint32_t, 2>{0, 0}));
  CHECK(!EncodeReturn({ValueKind::Void, 1}, &registers));
  CHECK(!EncodeReturn({ValueKind::Pointer32, UINT64_MAX}, &registers));
  CHECK(!EncodeReturn(value, nullptr));
  CHECK(!DecodeReturn(static_cast<ValueKind>(255), registers, &value));
  CHECK(value.kind == ValueKind::Void && value.bits == 0);
  CHECK(!DecodeReturn(ValueKind::I32, registers, nullptr));
}

void CheckDescriptors() {
  JniMethodSignature signature;
  CHECK(ParseJniMethodDescriptor("([I[[Ljava/lang/String;ZBCSIJFD)Ljava/lang/Object;",
                                 &signature));
  CHECK(signature.parameters == (std::vector<ValueKind>{ValueKind::Pointer32,
      ValueKind::Pointer32, ValueKind::U32, ValueKind::I32, ValueKind::U32,
      ValueKind::I32, ValueKind::I32, ValueKind::I64, ValueKind::F32, ValueKind::F64}));
  CHECK(signature.result == ValueKind::Pointer32);
  const std::vector<std::string> invalid{
      "", "I", "()", "(I", "()VI", "(V)V", "([V)V", "([)V", "(Q)V",
      "(L;)V", "(Ljava/lang/String)V", "(Ljava.lang.String;)V",
      "(L/foo;)V", "(Lfoo//bar;)V", "(Lfoo/;)V", "()([I", "(...)V",
      std::string("(Lfoo\0bar;)V", 12)};
  for (const std::string& descriptor : invalid) {
    std::string error;
    CHECK(!ParseJniMethodDescriptor(descriptor, &signature, &error));
    CHECK(!error.empty());
    CHECK(signature.result == ValueKind::Pointer32 && signature.parameters.size() == 10);
  }
  CHECK(!ParseJniMethodDescriptor("()V", nullptr));
  CHECK(ParseJniMethodDescriptor("(" + std::string(255, '[') + "I)V", &signature));
  CHECK(signature.parameters == (std::vector<ValueKind>{ValueKind::Pointer32}));
  CHECK(!ParseJniMethodDescriptor("(" + std::string(256, '[') + "I)V", &signature));
  CHECK(ParseJniMethodDescriptor("(" + std::string(255, 'I') + ")V", &signature));
  CHECK(signature.parameters.size() == 255);
  CHECK(!ParseJniMethodDescriptor("(" + std::string(256, 'I') + ")V", &signature));
  CHECK(!ParseJniMethodDescriptor("(" + std::string(128, 'J') + ")V", &signature));
  CHECK(ParseJniMethodDescriptor("()V", &signature));
  CHECK(signature.parameters.empty() && signature.result == ValueKind::Void);
}

void CheckJniFrames() {
  JniMethodSignature signature;
  CHECK(ParseJniMethodDescriptor("(IJD)J", &signature));
  const std::vector<Value> arguments{Value::Signed32(7),
      Value::Signed64(0x1122334455667788), Value::Float64(2)};
  CallFrame frame;
  CHECK(PackJniCall(signature, JniCallMode::Static, 0x1000, 0x2000, arguments, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{0x1000, 0x2000, 7, 0}));
  CHECK(frame.stack == (std::vector<uint32_t>{0x55667788, 0x11223344, 0, 0x40000000}));
  CHECK(PackJniCall(signature, JniCallMode::CriticalNative, 0, 0, arguments, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{7, 0, 0x55667788, 0x11223344}));
  CHECK(frame.stack == (std::vector<uint32_t>{0, 0x40000000}));
  CHECK(!PackJniCall(signature, JniCallMode::CriticalNative, 1, 0, arguments, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Instance, 0, 2, arguments, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 0, arguments, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, uint64_t{1} << 32,
                     2, arguments, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1,
                     uint64_t{1} << 32, arguments, &frame));
  CHECK(!PackJniCall(signature, static_cast<JniCallMode>(255), 1, 2, arguments, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 2, {}, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 2,
                     {Value::Unsigned32(7), arguments[1], arguments[2]}, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 2, arguments, nullptr));
  CHECK(ParseJniMethodDescriptor("(Ljava/lang/String;)V", &signature));
  CHECK(!PackJniCall(signature, JniCallMode::CriticalNative, 0, 0,
                     {Value::GuestPointer(1)}, &frame));
  CHECK(PackJniCall(signature, JniCallMode::Instance, 1, 2,
                    {Value::GuestPointer(0)}, &frame));
  CHECK(frame.core == (std::array<uint32_t, 4>{1, 2, 0, 0}));
  CHECK(ParseJniMethodDescriptor("()[I", &signature));
  CHECK(!PackJniCall(signature, JniCallMode::CriticalNative, 0, 0, {}, &frame));
  CHECK(ParseJniMethodDescriptor("(" + std::string(255, 'I') + ")V", &signature));
  std::vector<Value> many(255, Value::Signed32(1));
  CHECK(PackJniCall(signature, JniCallMode::Static, 1, 2, many, &frame));
  CHECK(!PackJniCall(signature, JniCallMode::Instance, 1, 2, many, &frame));
  signature.parameters = {ValueKind::Void};
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 2, {{}}, &frame));
  signature.parameters = {ValueKind::U64};
  CHECK(!PackJniCall(signature, JniCallMode::Static, 1, 2,
                     {Value::Unsigned64(1)}, &frame));
}

void CheckHandles() {
  OpaqueHandleTable table(2);
  uint32_t first = 0, second = 0, third = 0;
  // Retain the actual host width. These deliberately high addresses are opaque
  // test values, never dereferenced or exposed as guest memory.
  const uintptr_t host_value = sizeof(uintptr_t) == 8
      ? static_cast<uintptr_t>(UINT64_C(0x123456789abcdef0)) : UINT32_C(0x9abcdef0);
  CHECK(table.Insert(host_value, OpaqueHandleKind::Object, &first));
  CHECK(first != 0);
  uintptr_t resolved = 0;
  CHECK(table.Resolve(first, OpaqueHandleKind::Object, &resolved));
  CHECK(resolved == host_value);
  CHECK(!table.Resolve(first, OpaqueHandleKind::Method, &resolved));
  CHECK(resolved == host_value);
  CHECK(!table.Remove(first, OpaqueHandleKind::Method));
  CHECK(table.Resolve(first, OpaqueHandleKind::Object, &resolved));
  CHECK(table.Insert(host_value, OpaqueHandleKind::Object, &second));
  CHECK(first != second);
  CHECK(!table.Insert(host_value, OpaqueHandleKind::Object, &third));
  CHECK(third == 0);
  CHECK(table.Remove(first, OpaqueHandleKind::Object));
  CHECK(!table.Resolve(first, OpaqueHandleKind::Object, &resolved));
  CHECK(!table.Remove(first, OpaqueHandleKind::Object));
  CHECK(table.Insert(123, OpaqueHandleKind::Field, &third));
  CHECK(third != first && third != second);
  CHECK(!table.Resolve(first, OpaqueHandleKind::Object, &resolved));
  CHECK(table.Resolve(third, OpaqueHandleKind::Field, &resolved));
  CHECK(resolved == 123);
  CHECK(table.Resolve(second, OpaqueHandleKind::Object, &resolved));
  CHECK(resolved == host_value);
  CHECK(!table.Resolve(1, OpaqueHandleKind::Object, &resolved));
  CHECK(!table.Resolve(0x10000, OpaqueHandleKind::Object, &resolved));
  CHECK(!table.Resolve(0x1ffff, OpaqueHandleKind::Object, &resolved));
  CHECK(table.Insert(0, OpaqueHandleKind::Object, &third));
  CHECK(third == 0);
  CHECK(table.Resolve(0, OpaqueHandleKind::Method, &resolved));
  CHECK(resolved == 0);
  CHECK(table.Remove(0, OpaqueHandleKind::Field));
  CHECK(!table.Insert(1, static_cast<OpaqueHandleKind>(255), &first));
  CHECK(!table.Insert(1, OpaqueHandleKind::Object, nullptr));
  CHECK(!table.Resolve(0, OpaqueHandleKind::Object, nullptr));
  CHECK(!table.Resolve(0, static_cast<OpaqueHandleKind>(255), &resolved));
  CHECK(!table.Remove(0, static_cast<OpaqueHandleKind>(255)));

  // A stale token must not become valid when the generation reaches its
  // maximum. A capacity-one table exhausts permanently instead of wrapping.
  OpaqueHandleTable generations(1);
  bool generation_ok = true;
  uint32_t original = 0;
  CHECK(generations.Insert(1, OpaqueHandleKind::Other, &original));
  uint32_t current = original;
  for (uint32_t generation = 1; generation <= UINT16_MAX; ++generation) {
    generation_ok &= current >> 16 == generation;
    generation_ok &= generations.Remove(current, OpaqueHandleKind::Other);
    generation_ok &= !generations.Resolve(original, OpaqueHandleKind::Other, &resolved);
    if (generation != UINT16_MAX)
      generation_ok &= generations.Insert(1, OpaqueHandleKind::Other, &current);
  }
  CHECK(generation_ok);
  CHECK(!generations.Insert(1, OpaqueHandleKind::Other, &current));
  CHECK(!generations.Resolve(original, OpaqueHandleKind::Other, &resolved));

  // Parallel create/resolve/remove runs through one table, not isolated tables
  // per thread. Each worker owns its token until removal.
  OpaqueHandleTable shared(8);
  std::atomic<bool> threads_ok{true};
  std::vector<std::thread> workers;
  for (uintptr_t thread = 1; thread <= 4; ++thread) {
    workers.emplace_back([&shared, &threads_ok, thread] {
      for (uintptr_t i = 1; i <= 2000; ++i) {
        const uintptr_t value = (thread << 24) | i;
        uint32_t token = 0;
        uintptr_t actual = 0;
        if (!shared.Insert(value, OpaqueHandleKind::Other, &token) ||
            !shared.Resolve(token, OpaqueHandleKind::Other, &actual) || actual != value ||
            !shared.Remove(token, OpaqueHandleKind::Other)) {
          threads_ok.store(false);
          return;
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  CHECK(threads_ok.load());
}

}  // namespace

int main() {
  CheckPlacement();
  CheckFailures();
  CheckReturns();
  CheckDescriptors();
  CheckJniFrames();
  CheckHandles();
  std::printf("ARM32 typed ABI: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
