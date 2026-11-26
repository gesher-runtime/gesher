// gesher: scalar ARM32 calling-convention and opaque-token primitives.
#include <gesher/arm32_abi.h>

#include <cstring>
#include <limits>
#include <utility>

namespace gesher::arm32 {
namespace {

// Sources of the rules, not implementation code:
// https://github.com/ARM-software/abi-aa/blob/main/aapcs32/aapcs32.rst
// https://developer.android.com/ndk/guides/abis#v7a
// https://docs.oracle.com/javase/specs/jvms/se24/html/jvms-4.html#jvms-4.3.3
// https://developer.android.com/ndk/guides/jni-tips

bool Fail(std::string* error, const char* message) {
  if (error != nullptr) *error = message;
  return false;
}

void Clear(std::string* error) {
  if (error != nullptr) error->clear();
}

int WordsFor(ValueKind kind) {
  switch (kind) {
    case ValueKind::Void: return 0;
    case ValueKind::I32:
    case ValueKind::U32:
    case ValueKind::F32:
    case ValueKind::Pointer32: return 1;
    case ValueKind::I64:
    case ValueKind::U64:
    case ValueKind::F64: return 2;
  }
  return -1;
}

bool CheckValue(Value value, bool allow_void, std::string* error) {
  const int words = WordsFor(value.kind);
  if (words < 0) return Fail(error, "unsupported AAPCS32 value kind");
  if (words == 0) {
    if (!allow_void) return Fail(error, "void is not an argument type");
    if (value.bits != 0) return Fail(error, "void has no return value bits");
  }
  if (words == 1 && value.bits > UINT32_MAX)
    return Fail(error, "32-bit value has bits outside its guest width");
  return true;
}

struct Location {
  bool stack;
  size_t first;
};

struct Cursor {
  size_t core = 0;
  size_t stack = 0;

  Location Next(size_t words) {
    // AAPCS32 C.3/C.4: doublewords start in an even register and cannot
    // split. Only scalar types are admitted, so aggregate split rule C.5
    // is intentionally not applied here.
    if (words == 2) core = (core + 1) & ~size_t{1};
    if (core + words <= 4) {
      const size_t first = core;
      core += words;
      return {false, first};
    }
    core = 4;
    // C.7/C.8: once register allocation is exhausted, align the stack
    // address, then store the least significant word first (little endian).
    if (words == 2) stack = (stack + 1) & ~size_t{1};
    const size_t first = stack;
    stack += words;
    return {true, first};
  }
};

bool ParseType(std::string_view input, size_t* position, bool allow_void,
               ValueKind* kind, std::string* error) {
  size_t at = *position;
  if (at == input.size()) return Fail(error, "missing JNI descriptor type");
  size_t dimensions = 0;
  while (at < input.size() && input[at] == '[') {
    if (++dimensions > 255)
      return Fail(error, "JNI array descriptor exceeds 255 dimensions");
    ++at;
  }
  if (at == input.size()) return Fail(error, "missing JNI array element type");
  ValueKind parsed;
  switch (input[at++]) {
    case 'V':
      if (!allow_void || dimensions != 0)
        return Fail(error, "void is permitted only as a JNI method return type");
      parsed = ValueKind::Void;
      break;
    case 'Z':
    case 'C': parsed = ValueKind::U32; break;
    case 'B':
    case 'S':
    case 'I': parsed = ValueKind::I32; break;
    case 'J': parsed = ValueKind::I64; break;
    case 'F': parsed = ValueKind::F32; break;
    case 'D': parsed = ValueKind::F64; break;
    case 'L': {
      bool component_empty = true;
      while (at < input.size() && input[at] != ';') {
        const char ch = input[at++];
        if (ch == '.' || ch == '[' || ch == '\0')
          return Fail(error, "invalid JNI object class name");
        if (ch == '/') {
          if (component_empty) return Fail(error, "empty JNI class-name component");
          component_empty = true;
        } else {
          component_empty = false;
        }
      }
      if (at == input.size() || component_empty)
        return Fail(error, "empty or unterminated JNI object class name");
      ++at;  // ';'
      parsed = ValueKind::Pointer32;
      break;
    }
    default: return Fail(error, "unknown JNI descriptor type");
  }
  *position = at;
  *kind = dimensions != 0 ? ValueKind::Pointer32 : parsed;
  return true;
}

bool CheckSignature(const JniMethodSignature& signature, JniCallMode mode,
                    std::string* error) {
  if (mode != JniCallMode::Instance && mode != JniCallMode::Static &&
      mode != JniCallMode::CriticalNative)
    return Fail(error, "unknown JNI call mode");
  if (WordsFor(signature.result) < 0)
    return Fail(error, "invalid JNI method return kind");
  if (signature.result == ValueKind::U64)
    return Fail(error, "JNI methods do not have unsigned 64-bit values");
  size_t slots = mode == JniCallMode::Instance ? 1 : 0;
  for (ValueKind kind : signature.parameters) {
    const int words = WordsFor(kind);
    if (words <= 0 || kind == ValueKind::U64)
      return Fail(error, "invalid JNI parameter kind");
    slots += static_cast<size_t>(words);
    if (slots > 255) return Fail(error, "JNI method exceeds 255 parameter slots");
    if (mode == JniCallMode::CriticalNative && kind == ValueKind::Pointer32)
      return Fail(error, "CriticalNative methods cannot take managed references");
  }
  if (mode == JniCallMode::CriticalNative && signature.result == ValueKind::Pointer32)
    return Fail(error, "CriticalNative methods cannot return managed references");
  return true;
}

bool ValidHandleKind(OpaqueHandleKind kind) {
  return kind == OpaqueHandleKind::Object || kind == OpaqueHandleKind::Method ||
         kind == OpaqueHandleKind::Field || kind == OpaqueHandleKind::Other;
}

}  // namespace

Value Value::Signed32(int32_t value) {
  return {ValueKind::I32, static_cast<uint32_t>(value)};
}
Value Value::Unsigned32(uint32_t value) { return {ValueKind::U32, value}; }
Value Value::Signed64(int64_t value) {
  return {ValueKind::I64, static_cast<uint64_t>(value)};
}
Value Value::Unsigned64(uint64_t value) { return {ValueKind::U64, value}; }
Value Value::Float32(float value) {
  static_assert(sizeof(float) == sizeof(uint32_t) && std::numeric_limits<float>::is_iec559);
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return {ValueKind::F32, bits};
}
Value Value::Float64(double value) {
  static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559);
  uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return {ValueKind::F64, bits};
}
Value Value::GuestPointer(uint64_t value) { return {ValueKind::Pointer32, value}; }

bool PackArguments(const std::vector<Value>& arguments, CallFrame* result,
                   std::string* error) {
  if (result == nullptr) return Fail(error, "missing AAPCS32 frame output");
  CallFrame frame;
  Cursor cursor;
  for (Value value : arguments) {
    if (!CheckValue(value, false, error)) return false;
    const size_t words = static_cast<size_t>(WordsFor(value.kind));
    const Location location = cursor.Next(words);
    if (location.stack) {
      frame.stack.resize(cursor.stack, 0);
      frame.stack[location.first] = static_cast<uint32_t>(value.bits);
      if (words == 2) frame.stack[location.first + 1] = static_cast<uint32_t>(value.bits >> 32);
    } else {
      frame.core[location.first] = static_cast<uint32_t>(value.bits);
      if (words == 2) frame.core[location.first + 1] = static_cast<uint32_t>(value.bits >> 32);
    }
  }
  if ((frame.stack.size() & 1) != 0) frame.stack.push_back(0);
  *result = std::move(frame);
  Clear(error);
  return true;
}

bool UnpackArguments(const std::vector<ValueKind>& kinds, const CallFrame& frame,
                     std::vector<Value>* result, std::string* error) {
  if (result == nullptr) return Fail(error, "missing AAPCS32 argument output");
  if ((frame.stack.size() & 1) != 0)
    return Fail(error, "AAPCS32 public stack is not 8-byte aligned");
  std::vector<Value> values;
  values.reserve(kinds.size());
  Cursor cursor;
  for (ValueKind kind : kinds) {
    const int count = WordsFor(kind);
    if (count <= 0) return Fail(error, "invalid AAPCS32 argument kind");
    const size_t words = static_cast<size_t>(count);
    const Location location = cursor.Next(words);
    if (location.stack && cursor.stack > frame.stack.size())
      return Fail(error, "truncated AAPCS32 argument stack");
    const uint32_t* data = location.stack ? frame.stack.data() : frame.core.data();
    uint64_t bits = data[location.first];
    if (words == 2) bits |= static_cast<uint64_t>(data[location.first + 1]) << 32;
    values.push_back({kind, bits});
  }
  *result = std::move(values);
  Clear(error);
  return true;
}

bool StackPointerFor(const CallFrame& frame, uint32_t aligned_stack_top,
                     uint32_t* result, std::string* error) {
  if (result == nullptr) return Fail(error, "missing AAPCS32 stack-pointer output");
  if ((aligned_stack_top & 7) != 0 || (frame.stack.size() & 1) != 0)
    return Fail(error, "AAPCS32 public stack is not 8-byte aligned");
  if (frame.stack.size() > aligned_stack_top / sizeof(uint32_t))
    return Fail(error, "AAPCS32 stack address would underflow");
  *result = aligned_stack_top - static_cast<uint32_t>(frame.stack.size() * sizeof(uint32_t));
  Clear(error);
  return true;
}

bool EncodeReturn(Value value, std::array<uint32_t, 2>* result, std::string* error) {
  if (result == nullptr) return Fail(error, "missing AAPCS32 return-register output");
  if (!CheckValue(value, true, error)) return false;
  const int words = WordsFor(value.kind);
  *result = {words > 0 ? static_cast<uint32_t>(value.bits) : 0,
             words == 2 ? static_cast<uint32_t>(value.bits >> 32) : 0};
  Clear(error);
  return true;
}

bool DecodeReturn(ValueKind kind, const std::array<uint32_t, 2>& registers,
                  Value* result, std::string* error) {
  if (result == nullptr) return Fail(error, "missing AAPCS32 return-value output");
  const int words = WordsFor(kind);
  if (words < 0) return Fail(error, "unsupported AAPCS32 return kind");
  uint64_t bits = words > 0 ? registers[0] : 0;
  if (words == 2) bits |= static_cast<uint64_t>(registers[1]) << 32;
  *result = {kind, bits};
  Clear(error);
  return true;
}

bool ParseJniMethodDescriptor(std::string_view descriptor, JniMethodSignature* result,
                              std::string* error) {
  if (result == nullptr) return Fail(error, "missing JNI descriptor output");
  if (descriptor.empty() || descriptor[0] != '(' || descriptor.size() > 65535)
    return Fail(error, "invalid JNI method descriptor");
  JniMethodSignature parsed;
  size_t at = 1;
  size_t slots = 0;
  while (at < descriptor.size() && descriptor[at] != ')') {
    ValueKind kind;
    if (!ParseType(descriptor, &at, false, &kind, error)) return false;
    slots += static_cast<size_t>(WordsFor(kind));
    if (slots > 255) return Fail(error, "JNI method exceeds 255 parameter slots");
    parsed.parameters.push_back(kind);
  }
  if (at == descriptor.size()) return Fail(error, "missing JNI parameter-list terminator");
  ++at;
  if (!ParseType(descriptor, &at, true, &parsed.result, error)) return false;
  if (at != descriptor.size()) return Fail(error, "trailing data in JNI method descriptor");
  *result = std::move(parsed);
  Clear(error);
  return true;
}

bool PackJniCall(const JniMethodSignature& signature, JniCallMode mode,
                 uint64_t guest_env, uint64_t guest_receiver_or_class,
                 const std::vector<Value>& arguments, CallFrame* result,
                 std::string* error) {
  if (result == nullptr) return Fail(error, "missing JNI call-frame output");
  if (!CheckSignature(signature, mode, error)) return false;
  if (guest_env > UINT32_MAX || guest_receiver_or_class > UINT32_MAX)
    return Fail(error, "implicit JNI address or token exceeds the guest width");
  if (arguments.size() != signature.parameters.size())
    return Fail(error, "JNI argument count does not match its descriptor");
  std::vector<Value> native_arguments;
  native_arguments.reserve(arguments.size() + 2);
  if (mode == JniCallMode::CriticalNative) {
    if (guest_env != 0 || guest_receiver_or_class != 0)
      return Fail(error, "CriticalNative calls have no implicit JNI arguments");
  } else {
    if (guest_env == 0 || guest_receiver_or_class == 0)
      return Fail(error, "JNI calls require a guest environment and receiver/class");
    native_arguments.push_back(Value::GuestPointer(guest_env));
    native_arguments.push_back(Value::GuestPointer(guest_receiver_or_class));
  }
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (arguments[i].kind != signature.parameters[i])
      return Fail(error, "JNI argument kind does not match its descriptor");
    native_arguments.push_back(arguments[i]);
  }
  return PackArguments(native_arguments, result, error);
}

bool OpaqueHandleTable::Insert(uintptr_t host_value, OpaqueHandleKind kind,
                               uint32_t* result, std::string* error) {
  if (result == nullptr) return Fail(error, "missing opaque-handle output");
  if (!ValidHandleKind(kind)) return Fail(error, "invalid opaque-handle kind");
  if (host_value == 0) {
    *result = 0;
    Clear(error);
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  uint16_t index;
  if (free_.empty()) {
    if (slots_.size() >= capacity_)
      return Fail(error, "opaque-handle capacity exhausted");
    index = static_cast<uint16_t>(slots_.size());
    slots_.push_back({host_value, kind});
  } else {
    index = free_.back();
    free_.pop_back();
    Slot& slot = slots_[index];
    slot.host_value = host_value;
    slot.kind = kind;
    slot.live = true;
  }
  *result = (uint32_t{slots_[index].generation} << 16) | (uint32_t{index} + 1);
  Clear(error);
  return true;
}

bool OpaqueHandleTable::Resolve(uint32_t token, OpaqueHandleKind kind,
                                uintptr_t* result, std::string* error) const {
  if (result == nullptr) return Fail(error, "missing opaque-host-value output");
  if (!ValidHandleKind(kind)) return Fail(error, "invalid opaque-handle kind");
  if (token == 0) {
    *result = 0;
    Clear(error);
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t one_based_index = token & UINT16_MAX;
  const uint32_t generation = token >> 16;
  if (one_based_index == 0 || one_based_index > slots_.size() || generation == 0)
    return Fail(error, "invalid opaque handle");
  const Slot& slot = slots_[one_based_index - 1];
  if (!slot.live || slot.generation != generation || slot.kind != kind)
    return Fail(error, "stale or mismatched opaque handle");
  *result = slot.host_value;
  Clear(error);
  return true;
}

bool OpaqueHandleTable::Remove(uint32_t token, OpaqueHandleKind kind, std::string* error) {
  if (!ValidHandleKind(kind)) return Fail(error, "invalid opaque-handle kind");
  if (token == 0) {
    Clear(error);
    return true;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const uint32_t one_based_index = token & UINT16_MAX;
  const uint32_t generation = token >> 16;
  if (one_based_index == 0 || one_based_index > slots_.size() || generation == 0)
    return Fail(error, "invalid opaque handle");
  Slot& slot = slots_[one_based_index - 1];
  if (!slot.live || slot.generation != generation || slot.kind != kind)
    return Fail(error, "stale or mismatched opaque handle");
  // Append before mutating the live slot; an allocation failure cannot leave
  // a live token invalidated without completing this operation.
  if (slot.generation != UINT16_MAX)
    free_.push_back(static_cast<uint16_t>(one_based_index - 1));
  slot.host_value = 0;
  slot.live = false;
  if (slot.generation != UINT16_MAX) ++slot.generation;
  Clear(error);
  return true;
}

}  // namespace gesher::arm32
