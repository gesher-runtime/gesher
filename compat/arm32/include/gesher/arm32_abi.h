// gesher: typed scalar AAPCS32 marshalling for Android's armeabi-v7a softfp ABI.
// This module does not implement JNI, translate host pointers, or advertise an ABI.
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::arm32 {

enum class ValueKind : uint8_t { Void, I32, U32, I64, U64, F32, F64, Pointer32 };

// IEEE floating-point values are carried as their raw bits. Narrow values use
// only bits 0..31 (including negative I32 values). Pointer32 is a guest virtual
// address or an explicit opaque guest token, never a truncated host pointer.
struct Value {
  ValueKind kind = ValueKind::Void;
  uint64_t bits = 0;

  static Value Signed32(int32_t value);
  static Value Unsigned32(uint32_t value);
  static Value Signed64(int64_t value);
  static Value Unsigned64(uint64_t value);
  static Value Float32(float value);
  static Value Float64(double value);
  // Keep all incoming bits so marshalling can reject an out-of-range address
  // instead of silently narrowing a host-width integer at this call site.
  static Value GuestPointer(uint64_t value);
};

// stack[0] belongs at the callee's entry SP. Padding words are zero. The stack
// vector has an even number of words, preserving the public SP's 8-byte alignment.
// No implicit variadic promotion, aggregate passing, or hard-float PCS is supported.
struct CallFrame {
  std::array<uint32_t, 4> core{};
  std::vector<uint32_t> stack;
};

// Outputs are unchanged on failure. A null error pointer is allowed; a null
// result pointer is rejected. Successful operations clear a supplied error.
bool PackArguments(const std::vector<Value>& arguments, CallFrame* result,
                   std::string* error = nullptr);
bool UnpackArguments(const std::vector<ValueKind>& kinds, const CallFrame& frame,
                     std::vector<Value>* result, std::string* error = nullptr);
bool StackPointerFor(const CallFrame& frame, uint32_t aligned_stack_top,
                     uint32_t* result, std::string* error = nullptr);
bool EncodeReturn(Value value, std::array<uint32_t, 2>* result,
                  std::string* error = nullptr);
bool DecodeReturn(ValueKind kind, const std::array<uint32_t, 2>& registers,
                  Value* result, std::string* error = nullptr);

// A descriptor contains only Java method parameters, without the implicit
// JNIEnv* and receiver/jclass. Arrays and references use Pointer32; byte/short/int
// use I32, boolean/char use U32. The actual Java primitive range checks remain
// the caller's responsibility. Reference resolution is deliberately separate.
struct JniMethodSignature {
  std::vector<ValueKind> parameters;
  ValueKind result = ValueKind::Void;
};

// Parses JVM method-descriptor grammar, including the 255 parameter-slot and
// array-dimension limits. It does not resolve classes or decode class-file UTF-8.
bool ParseJniMethodDescriptor(std::string_view descriptor,
                              JniMethodSignature* result,
                              std::string* error = nullptr);

enum class JniCallMode { Instance, Static, CriticalNative };

// Ordinary modes prepend the explicit guest JNIEnv and receiver/class token.
// CriticalNative means a static primitive-only method, with no implicit args;
// both supplied implicit tokens must be zero. No native method is registered or
// called by this helper. Variadic JNI table functions are outside its contract.
bool PackJniCall(const JniMethodSignature& signature, JniCallMode mode,
                 uint64_t guest_env, uint64_t guest_receiver_or_class,
                 const std::vector<Value>& arguments, CallFrame* result,
                 std::string* error = nullptr);

enum class OpaqueHandleKind : uint8_t { Object, Method, Field, Other };

// A bounded, thread-safe mapping from explicit 32-bit opaque tokens to complete
// host-width values. Tokens are NOT guest memory addresses. Zero denotes null.
// Each insertion creates a distinct token; removal invalidates it. A slot is
// retired before its 16-bit generation could wrap, so stale tokens never revive
// within this table's lifetime. Tokens belong to this table, not other tables.
//
// This table does not retain/release JNI references, enforce JNI thread ownership,
// or extend a host object's lifetime. A caller must supply those policies and
// synchronize host destruction with outstanding uses of a resolved value.
class OpaqueHandleTable {
 public:
  explicit OpaqueHandleTable(uint16_t capacity = UINT16_MAX) : capacity_(capacity) {}
  bool Insert(uintptr_t host_value, OpaqueHandleKind kind, uint32_t* result,
              std::string* error = nullptr);
  bool Resolve(uint32_t token, OpaqueHandleKind kind, uintptr_t* result,
               std::string* error = nullptr) const;
  bool Remove(uint32_t token, OpaqueHandleKind kind, std::string* error = nullptr);

 private:
  struct Slot {
    uintptr_t host_value;
    OpaqueHandleKind kind;
    uint16_t generation = 1;
    bool live = true;
  };
  const uint16_t capacity_;
  mutable std::mutex mutex_;
  std::vector<Slot> slots_;
  std::vector<uint16_t> free_;
};

}  // namespace gesher::arm32
