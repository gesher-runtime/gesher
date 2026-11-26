// gesher: JNI calls issued by a 32-bit ARM guest through an explicit host ABI.
// This is a subset of the Android JNI table. Missing operations are not
// simulated: their table pointers remain zero and guest execution faults.
#include <gesher/arm32_jni.h>

#include <gesher/arm32_abi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gesher::arm32 {
namespace {
constexpr uint32_t kGuestEnv = 0xd0003000;
constexpr uint32_t kGuestTable = 0xd0004000;
constexpr uint32_t kGuestVm = 0xd0005000;
constexpr uint32_t kGuestVmTable = 0xd0006000;
constexpr uint64_t kStringStart = 0xd0100000;
constexpr uint64_t kStringEnd = 0xd1000000;
constexpr uint32_t kMaxUtfBytes = 1024 * 1024;
constexpr auto kRW = MemoryPermission::kRead | MemoryPermission::kWrite;
constexpr uint32_t kPage = GuestMemory::kPageSize;

bool Fail(std::string* error, const char* message) {
  if (error) *error = message;
  return false;
}

bool ReadGuestString(GuestMemory& memory, uint32_t address,
                     std::string* output, std::string* error) {
  if (!address) return Fail(error, "JNI32 null guest string pointer");
  std::string string;
  for (uint32_t i = 0; i < kMaxUtfBytes; ++i) {
    if (uint64_t{address} + i > UINT32_MAX)
      return Fail(error, "JNI32 guest string address overflow");
    uint8_t byte = 0;
    if (!memory.Read8(address + i, &byte, error)) return false;
    if (!byte) { *output = std::move(string); return true; }
    string.push_back(static_cast<char>(byte));
  }
  return Fail(error, "JNI32 guest string exceeds limit");
}

constexpr size_t Slot(size_t offset) { return offset / sizeof(void*); }

// Parsing the Java type codes is independent of guest ABI word allocation.
// ParseJniMethodDescriptor checks full JVM grammar and limits first.
bool RawParameterTypes(std::string_view descriptor, std::vector<char>* types) {
  if (descriptor.empty() || descriptor.front() != '(') return false;
  for (size_t at = 1; at < descriptor.size() && descriptor[at] != ')';) {
    char type = descriptor[at++];
    if (type == '[') {
      while (at < descriptor.size() && descriptor[at] == '[') ++at;
      if (at == descriptor.size()) return false;
      if (descriptor[at] == 'L') {
        at = descriptor.find(';', at);
        if (at == std::string_view::npos) return false;
        ++at;
      } else {
        ++at;  // primitive array's element
      }
    } else if (type == 'L') {
      at = descriptor.find(';', at);
      if (at == std::string_view::npos) return false;
      ++at;
    }
    types->push_back(type);
  }
  return true;
}

char ReturnTypeCode(std::string_view descriptor) {
  const size_t close = descriptor.find(')');
  return close == std::string_view::npos || close + 1 == descriptor.size()
             ? '\0' : descriptor[close + 1];
}

}  // namespace

struct Jni32::State : std::enable_shared_from_this<Jni32::State> {
  struct Ref {
    jobject object = nullptr;
    bool global = false;
  };
  struct UtfCopy {
    uint32_t owner = 0;
    uint32_t length = 0;
    uint32_t mapped_size = 0;
  };

  Program& program;
  JNIEnv* const real;
  const std::thread::id owner = std::this_thread::get_id();
  OpaqueHandleTable handles;
  std::map<uint32_t, Ref> references;
  std::map<uint32_t, UtfCopy> utf_copies;
  uint64_t next_string = kStringStart;
  bool active = false;
  bool attempted = false;

  State(Program& p, JNIEnv* e) : program(p), real(e) {}

  bool CheckThread(std::string* error) const {
    if (!active || !real || std::this_thread::get_id() != owner)
      return Fail(error, "JNI32 call outside installed owning Java thread");
    return true;
  }

  bool Args(const std::vector<Value>& a, size_t n, std::string* error) const {
    if (!CheckThread(error)) return false;
    if (a.size() != n || a.empty() || a[0].bits != kGuestEnv)
      return Fail(error, "JNI32 invalid guest JNIEnv pointer or argument count");
    return true;
  }

  bool Lookup(uint32_t token, jobject* object, std::string* error) const {
    if (token == 0) { *object = nullptr; return true; }
    uintptr_t value = 0;
    if (!handles.Resolve(token, OpaqueHandleKind::Object, &value, error))
      return false;
    if (!references.contains(token))
      return Fail(error, "JNI32 object token is not live");
    *object = reinterpret_cast<jobject>(value);
    return true;
  }

  bool Adopt(jobject object, bool global, uint32_t* result, std::string* error) {
    if (object == nullptr) { *result = 0; return true; }
    uint32_t token = 0;
    if (!handles.Insert(reinterpret_cast<uintptr_t>(object),
                        OpaqueHandleKind::Object, &token, error)) {
      if (global) real->DeleteGlobalRef(object); else real->DeleteLocalRef(object);
      return false;
    }
    try {
      references.emplace(token, Ref{object, global});
    } catch (...) {
      handles.Remove(token, OpaqueHandleKind::Object);
      if (global) real->DeleteGlobalRef(object); else real->DeleteLocalRef(object);
      return Fail(error, "JNI32 cannot retain guest reference");
    }
    *result = token;
    return true;
  }

  bool Delete(uint32_t token, bool global, std::string* error) {
    if (token == 0) return true;
    auto found = references.find(token);
    if (found == references.end() || found->second.global != global)
      return Fail(error, "JNI32 token has wrong reference scope");
    if (!handles.Remove(token, OpaqueHandleKind::Object, error)) return false;
    if (global) real->DeleteGlobalRef(found->second.object);
    else real->DeleteLocalRef(found->second.object);
    references.erase(found);
    return true;
  }

  void ClearReferences(bool globals) {
    for (auto it = references.begin(); it != references.end();) {
      if (!globals && it->second.global) { ++it; continue; }
      const uint32_t token = it->first;
      (void)handles.Remove(token, OpaqueHandleKind::Object);
      if (it->second.global) real->DeleteGlobalRef(it->second.object);
      else real->DeleteLocalRef(it->second.object);
      it = references.erase(it);
    }
  }

  bool UtfBytes(jstring string, uint32_t owner_token, uint32_t is_copy,
                uint32_t* guest_address, std::string* error) {
    const char* host = real->GetStringUTFChars(string, nullptr);
    if (!host) { *guest_address = 0; return true; } // real exception is pending
    const jsize length = real->GetStringUTFLength(string);
    if (length < 0 || static_cast<uint64_t>(length) > kMaxUtfBytes) {
      real->ReleaseStringUTFChars(string, host);
      return Fail(error, "JNI32 UTF payload exceeds supported memory limit");
    }
    const uint64_t allocation = (uint64_t{static_cast<uint32_t>(length)} + 1 +
                                 kPage - 1) & ~(uint64_t{kPage} - 1);
    const uint64_t address = next_string;
    if (address >= kStringEnd || allocation > kStringEnd - address) {
      real->ReleaseStringUTFChars(string, host);
      return Fail(error, "JNI32 UTF allocation address space exhausted");
    }
    if (is_copy && !program.Memory().IsMapped(is_copy, 1, MemoryPermission::kWrite)) {
      real->ReleaseStringUTFChars(string, host);
      return Fail(error, "JNI32 isCopy is not writable guest memory");
    }
    const bool mapped = program.Memory().Map(static_cast<uint32_t>(address), allocation, kRW, error);
    if (!mapped) { real->ReleaseStringUTFChars(string, host); return false; }
    const bool written = program.Memory().Write(static_cast<uint32_t>(address),
                                                host, static_cast<size_t>(length) + 1, error) &&
                        (!is_copy || program.Memory().Write8(is_copy, 1, error));
    real->ReleaseStringUTFChars(string, host);
    if (!written) {
      program.Memory().Unmap(static_cast<uint32_t>(address), allocation);
      return false;
    }
    try {
      utf_copies.emplace(static_cast<uint32_t>(address),
                         UtfCopy{owner_token, static_cast<uint32_t>(length),
                                 static_cast<uint32_t>(allocation)});
    } catch (...) {
      program.Memory().Unmap(static_cast<uint32_t>(address), allocation);
      return Fail(error, "JNI32 UTF copy bookkeeping failed");
    }
    next_string += allocation + kPage;
    *guest_address = static_cast<uint32_t>(address);
    return true;
  }

  bool ReleaseUtf(uint32_t token, uint32_t address, std::string* error) {
    auto found = utf_copies.find(address);
    if (found == utf_copies.end() || found->second.owner != token)
      return Fail(error, "JNI32 invalid string UTF release");
    if (!program.Memory().Unmap(address, found->second.mapped_size, error)) return false;
    utf_copies.erase(found);
    return true;
  }

  bool Register(const char* name, size_t slot, std::vector<ValueKind> kinds,
                ValueKind result, HostCallback callback, std::string* error) {
    constexpr size_t slots = sizeof(JNINativeInterface_) / sizeof(void*);
    if (slot >= slots || slot >= kPage / 4)
      return Fail(error, "JNI32 slot is outside guest function table");
    const std::string symbol = std::string("jni32.") + name;
    // Program owns its thunks beyond the lifetime of the Jni32 wrapper.
    // Retain State so the callback never calls a dangling [this] capture;
    // the State's active flag prevents access after Jni32 destruction.
    HostCallback retained = [self = shared_from_this(),
                             callback = std::move(callback)](
                                 const std::vector<Value>& values,
                                 Value* output, std::string* failure) {
      return callback(values, output, failure);
    };
    if (!program.BindHostSymbol(symbol, std::move(kinds), result,
                                std::move(retained), error)) return false;
    uint32_t address = 0;
    if (!program.HostSymbolAddress(symbol, &address))
      return Fail(error, "JNI32 bound host thunk address is absent");
    return program.Memory().Write32(kGuestTable + static_cast<uint32_t>(slot) * 4,
                                    address, error);
  }

  bool VmArgs(const std::vector<Value>& a, size_t n,
              std::string* error) const {
    if (!CheckThread(error)) return false;
    if (a.size() != n || a.empty() || a[0].bits != kGuestVm)
      return Fail(error, "JNI32 invalid guest JavaVM pointer or argument count");
    return true;
  }

  bool RegisterVm(const char* name, size_t slot,
                  std::vector<ValueKind> kinds, ValueKind result,
                  HostCallback callback, std::string* error) {
    constexpr size_t slots = sizeof(JNIInvokeInterface_) / sizeof(void*);
    if (slot >= slots || slot >= kPage / 4)
      return Fail(error, "JNI32 JavaVM slot exceeds guest invoke table");
    const std::string symbol = std::string("jni32.vm.") + name;
    HostCallback retained = [self = shared_from_this(),
                             callback = std::move(callback)](
                                 const std::vector<Value>& a,
                                 Value* out, std::string* failure) {
      return callback(a, out, failure);
    };
    if (!program.BindHostSymbol(symbol, std::move(kinds),
                                result, std::move(retained), error)) return false;
    uint32_t address = 0;
    if (!program.HostSymbolAddress(symbol, &address))
      return Fail(error, "JNI32 JavaVM host thunk address is absent");
    return program.Memory().Write32(kGuestVmTable +
                   static_cast<uint32_t>(slot) * 4, address, error);
  }

  bool Install(std::string* error) {
    if (attempted) return Fail(error, "JNI32 installation may run only once");
    attempted = true;
    if (!real || std::this_thread::get_id() != owner)
      return Fail(error, "JNI32 requires a real JNIEnv on its owning thread");
    auto& memory = program.Memory();
    if (!memory.Map(kGuestEnv, kPage, kRW, error)) return false;
    if (!memory.Map(kGuestTable, kPage, kRW, error)) {
      memory.Unmap(kGuestEnv, kPage);
      return false;
    }
    // Do not unmap a pre-existing guest page if an address reservation
    // collides: only pages this Install call successfully mapped are owned.
    if (!memory.Map(kGuestVm, kPage, kRW, error)) {
      (void)memory.Unmap(kGuestEnv, kPage);
      (void)memory.Unmap(kGuestTable, kPage);
      return false;
    }
    if (!memory.Map(kGuestVmTable, kPage, kRW, error)) {
      (void)memory.Unmap(kGuestVm, kPage);
      (void)memory.Unmap(kGuestEnv, kPage);
      (void)memory.Unmap(kGuestTable, kPage);
      return false;
    }
    if (!memory.Write32(kGuestEnv, kGuestTable, error) ||
        !memory.Write32(kGuestVm, kGuestVmTable, error)) {
      (void)memory.Unmap(kGuestVmTable, kPage);
      (void)memory.Unmap(kGuestVm, kPage);
      (void)memory.Unmap(kGuestEnv, kPage);
      (void)memory.Unmap(kGuestTable, kPage);
      return false;
    }
    active = true;
    using K = ValueKind;
    constexpr auto P = K::Pointer32, I = K::I32, U = K::U32, V = K::Void;
    // Every implemented callback checks the guest JNIEnv pointer, the host
    // Java thread and reference tokens. A failure stops the guest explicitly.
    const bool ok =
        Register("GetVersion", Slot(offsetof(JNINativeInterface_, GetVersion)),
                 {P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 1, e)) return false;
          *out = Value::Signed32(real->GetVersion());
          return true;
        }, error) &&
        Register("ExceptionCheck", Slot(offsetof(JNINativeInterface_, ExceptionCheck)),
                 {P}, U, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 1, e)) return false;
          *out = Value::Unsigned32(real->ExceptionCheck() ? 1 : 0);
          return true;
        }, error) &&
        Register("ExceptionClear", Slot(offsetof(JNINativeInterface_, ExceptionClear)),
                 {P}, V, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 1, e)) return false;
          real->ExceptionClear();
          *out = {V, 0};
          return true;
        }, error) &&
        Register("ExceptionOccurred", Slot(offsetof(JNINativeInterface_, ExceptionOccurred)),
                 {P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 1, e)) return false;
          uint32_t token = 0;
          if (!Adopt(real->ExceptionOccurred(), false, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("FindClass", Slot(offsetof(JNINativeInterface_, FindClass)),
                 {P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          std::string name;
          if (!ReadGuestString(program.Memory(), static_cast<uint32_t>(a[1].bits), &name, e))
            return false;
          uint32_t token = 0;
          if (!Adopt(real->FindClass(name.c_str()), false, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("NewLocalRef", Slot(offsetof(JNINativeInterface_, NewLocalRef)),
                 {P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject object = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &object, e)) return false;
          uint32_t token = 0;
          if (!Adopt(real->NewLocalRef(object), false, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("NewGlobalRef", Slot(offsetof(JNINativeInterface_, NewGlobalRef)),
                 {P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject object = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &object, e)) return false;
          uint32_t token = 0;
          if (!Adopt(real->NewGlobalRef(object), true, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("DeleteLocalRef", Slot(offsetof(JNINativeInterface_, DeleteLocalRef)),
                 {P, P}, V, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e) || !Delete(static_cast<uint32_t>(a[1].bits), false, e))
            return false;
          *out = {V, 0};
          return true;
        }, error) &&
        Register("DeleteGlobalRef", Slot(offsetof(JNINativeInterface_, DeleteGlobalRef)),
                 {P, P}, V, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e) || !Delete(static_cast<uint32_t>(a[1].bits), true, e))
            return false;
          *out = {V, 0};
          return true;
        }, error) &&
        Register("GetObjectClass", Slot(offsetof(JNINativeInterface_, GetObjectClass)),
                 {P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject object = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &object, e)) return false;
          uint32_t token = 0;
          if (!Adopt(real->GetObjectClass(object), false, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("IsSameObject", Slot(offsetof(JNINativeInterface_, IsSameObject)),
                 {P, P, P}, U, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 3, e)) return false;
          jobject left = nullptr, right = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &left, e) ||
              !Lookup(static_cast<uint32_t>(a[2].bits), &right, e)) return false;
          *out = Value::Unsigned32(real->IsSameObject(left, right) ? 1 : 0);
          return true;
        }, error) &&
        Register("NewStringUTF", Slot(offsetof(JNINativeInterface_, NewStringUTF)),
                 {P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          std::string str;
          if (!ReadGuestString(program.Memory(), static_cast<uint32_t>(a[1].bits), &str, e))
            return false;
          uint32_t token = 0;
          if (!Adopt(real->NewStringUTF(str.c_str()), false, &token, e)) return false;
          *out = Value::GuestPointer(token);
          return true;
        }, error) &&
        Register("GetStringUTFLength", Slot(offsetof(JNINativeInterface_, GetStringUTFLength)),
                 {P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject str = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &str, e) || !str)
            return Fail(e, "JNI32 UTF string reference is null or invalid");
          *out = Value::Signed32(real->GetStringUTFLength(static_cast<jstring>(str)));
          return true;
        }, error) &&
        Register("GetStringLength", Slot(offsetof(JNINativeInterface_, GetStringLength)),
                 {P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject str = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &str, e) || !str)
            return Fail(e, "JNI32 string reference is null or invalid");
          *out = Value::Signed32(real->GetStringLength(static_cast<jstring>(str)));
          return true;
        }, error) &&
        Register("GetStringUTFChars", Slot(offsetof(JNINativeInterface_, GetStringUTFChars)),
                 {P, P, P}, P, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 3, e)) return false;
          jobject str = nullptr;
          const uint32_t owner = static_cast<uint32_t>(a[1].bits);
          if (!Lookup(owner, &str, e) || !str)
            return Fail(e, "JNI32 UTF string reference is null or invalid");
          uint32_t pointer = 0;
          if (!UtfBytes(static_cast<jstring>(str), owner,
                        static_cast<uint32_t>(a[2].bits), &pointer, e)) return false;
          *out = Value::GuestPointer(pointer);
          return true;
        }, error) &&
        Register("ReleaseStringUTFChars", Slot(offsetof(JNINativeInterface_, ReleaseStringUTFChars)),
                 {P, P, P}, V, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 3, e) ||
              !ReleaseUtf(static_cast<uint32_t>(a[1].bits),
                          static_cast<uint32_t>(a[2].bits), e)) return false;
          *out = {V, 0};
          return true;
        }, error) &&
        Register("GetArrayLength", Slot(offsetof(JNINativeInterface_, GetArrayLength)),
                 {P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          jobject array = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &array, e) || !array)
            return Fail(e, "JNI32 array reference is null or invalid");
          *out = Value::Signed32(real->GetArrayLength(static_cast<jarray>(array)));
          return true;
        }, error) &&
        Register("GetJavaVM", Slot(offsetof(JNINativeInterface_, GetJavaVM)),
                 {P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 2, e)) return false;
          if (a[1].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[1].bits), kGuestVm, e))
            return Fail(e, "JNI32 GetJavaVM output pointer is not writable guest memory");
          *out = Value::Signed32(JNI_OK);
          return true;
        }, error) &&
        Register("ThrowNew", Slot(offsetof(JNINativeInterface_, ThrowNew)),
                 {P, P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!Args(a, 3, e)) return false;
          jobject klass = nullptr;
          if (!Lookup(static_cast<uint32_t>(a[1].bits), &klass, e) || !klass)
            return Fail(e, "JNI32 exception class reference is null or invalid");
          std::string text;
          if (!ReadGuestString(program.Memory(), static_cast<uint32_t>(a[2].bits),
                               &text, e)) return false;
          *out = Value::Signed32(real->ThrowNew(static_cast<jclass>(klass), text.c_str()));
          return true;
        }, error);

    const bool vm_ok = ok &&
        RegisterVm("GetEnv", Slot(offsetof(JNIInvokeInterface_, GetEnv)),
                   {P, P, I}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!VmArgs(a, 3, e)) return false;
          const int32_t requested = static_cast<int32_t>(a[2].bits);
          const int rc = requested >= JNI_VERSION_1_1 &&
                         requested <= real->GetVersion() ? JNI_OK : JNI_EVERSION;
          if (rc == JNI_OK &&
              (a[1].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[1].bits), kGuestEnv, e)))
            return Fail(e, "JNI32 JavaVM GetEnv output is invalid");
          *out = Value::Signed32(rc);
          return true;
        }, error) &&
        RegisterVm("AttachCurrentThread", Slot(offsetof(JNIInvokeInterface_, AttachCurrentThread)),
                   {P, P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!VmArgs(a, 3, e)) return false;
          if (a[2].bits != 0) {
            *out = Value::Signed32(JNI_ERR);
            return true;
          }
          if (a[1].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[1].bits), kGuestEnv, e))
            return Fail(e, "JNI32 JavaVM AttachCurrentThread output is invalid");
          *out = Value::Signed32(JNI_OK);
          return true;
        }, error) &&
        RegisterVm("AttachCurrentThreadAsDaemon", Slot(offsetof(JNIInvokeInterface_, AttachCurrentThreadAsDaemon)),
                   {P, P, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!VmArgs(a, 3, e)) return false;
          if (a[2].bits != 0) {
            *out = Value::Signed32(JNI_ERR);
            return true;
          }
          if (a[1].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[1].bits), kGuestEnv, e))
            return Fail(e, "JNI32 JavaVM daemon attach output is invalid");
          *out = Value::Signed32(JNI_OK);
          return true;
        }, error) &&
        RegisterVm("DetachCurrentThread", Slot(offsetof(JNIInvokeInterface_, DetachCurrentThread)),
                   {P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!VmArgs(a, 1, e)) return false;
          // Never detach ART's actual Java thread while running guest code.
          *out = Value::Signed32(JNI_ERR);
          return true;
        }, error) &&
        RegisterVm("DestroyJavaVM", Slot(offsetof(JNIInvokeInterface_, DestroyJavaVM)),
                   {P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!VmArgs(a, 1, e)) return false;
          *out = Value::Signed32(JNI_ERR);
          return true;
        }, error);

    // Android native libraries may query the already running VM without
    // using JNIEnv::GetJavaVM. Never return a host JavaVM address here.
    const bool created_vms_ok = vm_ok &&
        program.BindHostSymbol("JNI_GetCreatedJavaVMs",
            {P, I, P}, I, [this](const auto& a, Value* out, std::string* e) {
          if (!CheckThread(e) || a.size() != 3) return false;
          const int32_t capacity = static_cast<int32_t>(a[1].bits);
          if (capacity < 0)
            return Fail(e, "JNI32 VM enumeration has negative capacity");
          if (a[2].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[2].bits), 1, e))
            return Fail(e, "JNI32 VM count output is not writable guest memory");
          if (capacity > 0 && (a[0].bits == 0 || !program.Memory().Write32(
                  static_cast<uint32_t>(a[0].bits), kGuestVm, e)))
            return Fail(e, "JNI32 guest JavaVM array is not writable");
          *out = Value::Signed32(JNI_OK);
          return true;
        }, error);

    if (!created_vms_ok ||
        !memory.Protect(kGuestVm, kPage, MemoryPermission::kRead, error) ||
        !memory.Protect(kGuestVmTable, kPage, MemoryPermission::kRead, error) ||
        !memory.Protect(kGuestEnv, kPage, MemoryPermission::kRead, error) ||
        !memory.Protect(kGuestTable, kPage, MemoryPermission::kRead, error)) {
      active = false;
      // BindHostSymbol has no unbind. Inert callbacks remain registered but
      // cannot access a host JNIEnv; the installed guest pages are retired.
      memory.Unmap(kGuestEnv, kPage);
      memory.Unmap(kGuestTable, kPage);
      memory.Unmap(kGuestVm, kPage);
      memory.Unmap(kGuestVmTable, kPage);
      return false;
    }
    if (error) error->clear();
    return true;
  }
};

Jni32::Jni32(Program& program, JNIEnv* real_env)
    : state_(std::make_shared<State>(program, real_env)) {}
Jni32::~Jni32() {
  if (!state_) return;
  const bool owns_guest_tables = state_->active;
  state_->active = false;
  if (state_->real && std::this_thread::get_id() == state_->owner) {
    for (const auto& [address, copy] : state_->utf_copies) {
      (void)state_->program.Memory().Unmap(address, copy.mapped_size);
    }
    state_->utf_copies.clear();
    state_->ClearReferences(true);
  }
  // After shutdown, retire only pages of a successfully installed bridge.
  // Failed installations may have collided with someone else's guest pages;
  // never unmap those merely because they occupy our reserved addresses.
  if (owns_guest_tables) {
    (void)state_->program.Memory().Unmap(kGuestEnv, kPage);
    (void)state_->program.Memory().Unmap(kGuestTable, kPage);
    (void)state_->program.Memory().Unmap(kGuestVm, kPage);
    (void)state_->program.Memory().Unmap(kGuestVmTable, kPage);
  }
}
bool Jni32::Install(std::string* error) { return state_->Install(error); }
uint32_t Jni32::GuestEnvironment() const {
  return state_ && state_->active ? kGuestEnv : 0;
}
uint32_t Jni32::GuestJavaVM() const {
  return state_ && state_->active ? kGuestVm : 0;
}
bool Jni32::CallOnLoad(std::string_view library, jint* version,
                       std::string* error, uint64_t instruction_budget) {
  if (!state_ || !state_->CheckThread(error) || !version)
    return Fail(error, "JNI32 JNI_OnLoad requires an owning Java thread and output");
  Value returned;
  std::string failure;
  const bool called = state_->program.CallExport(
      library, "JNI_OnLoad",
      {Value::GuestPointer(GuestJavaVM()), Value::GuestPointer(0)},
      ValueKind::I32, &returned, &failure, instruction_budget);
  std::string cleanup;
  const bool cleaned = ReleaseLocals(&cleanup);
  if (!called || !cleaned) {
    if (error) *error = !failure.empty() ? failure : cleanup;
    return false;
  }
  const jint requested = static_cast<jint>(returned.bits);
  if (requested < JNI_VERSION_1_1 || requested > state_->real->GetVersion())
    return Fail(error, "JNI32 guest JNI_OnLoad returned unsupported JNI version");
  *version = requested;
  if (error) error->clear();
  return true;
}
bool Jni32::CallOnUnload(std::string_view library,
                         std::string* error, uint64_t instruction_budget) {
  if (!state_ || !state_->CheckThread(error))
    return Fail(error, "JNI32 JNI_OnUnload requires an attached owning Java thread");
  Value returned;
  std::string failure;
  const bool called = state_->program.CallExport(
      library, "JNI_OnUnload",
      {Value::GuestPointer(GuestJavaVM()), Value::GuestPointer(0)},
      ValueKind::Void, &returned, &failure, instruction_budget);
  std::string cleanup;
  const bool cleaned = ReleaseLocals(&cleanup);
  if (!called || !cleaned) {
    if (error) *error = !failure.empty() ? failure : cleanup;
    return false;
  }
  if (error) error->clear();
  return true;
}

bool Jni32::ReleaseLocals(std::string* error) {
  if (!state_ || !state_->CheckThread(error)) return false;
  const bool leaked = !state_->utf_copies.empty();
  // Even a misbehaving native is forbidden from leaving guest UTF mappings
  // alive across ART calls. Report the contract error while reclaiming them.
  for (const auto& [address, copy] : state_->utf_copies)
    (void)state_->program.Memory().Unmap(address, copy.mapped_size);
  state_->utf_copies.clear();
  state_->ClearReferences(false);
  if (leaked) return Fail(error, "JNI32 unreleased GetStringUTFChars leases at frame exit");
  if (error) error->clear();
  return true;
}

bool Jni32::CallNative(std::string_view library, std::string_view export_name,
                       std::string_view descriptor, JniCallMode mode,
                       jobject receiver, const jvalue* arguments,
                       size_t argument_count, jvalue* result,
                       std::string* error, uint64_t instruction_budget) {
  if (!state_ || !state_->CheckThread(error) || !result ||
      (argument_count && !arguments))
    return Fail(error, "JNI32 native invocation has invalid inputs or Java thread");
  if (mode != JniCallMode::CriticalNative && receiver == nullptr)
    return Fail(error, "JNI32 native method has no owning object or class");

  JniMethodSignature signature;
  if (!ParseJniMethodDescriptor(descriptor, &signature, error)) return false;
  if (argument_count != signature.parameters.size())
    return Fail(error, "JNI32 native argument count differs from descriptor");
  std::vector<char> raw_types;
  if (!RawParameterTypes(descriptor, &raw_types) ||
      raw_types.size() != argument_count)
    return Fail(error, "JNI32 raw argument types differ from descriptor");

  bool called = false;
  Value returned;
  jvalue host_result{};
  bool has_result_local = false;
  std::string failure;
  try {
    std::vector<Value> guest_arguments;
    guest_arguments.reserve(argument_count);
    for (size_t i = 0; i < argument_count; ++i) {
      const jvalue& input = arguments[i];
      switch (raw_types[i]) {
        case 'Z': guest_arguments.push_back(Value::Unsigned32(input.z != 0)); break;
        case 'B': guest_arguments.push_back(Value::Signed32(input.b)); break;
        case 'C': guest_arguments.push_back(Value::Unsigned32(input.c)); break;
        case 'S': guest_arguments.push_back(Value::Signed32(input.s)); break;
        case 'I': guest_arguments.push_back(Value::Signed32(input.i)); break;
        case 'J': guest_arguments.push_back(Value::Signed64(input.j)); break;
        case 'F': guest_arguments.push_back(Value::Float32(input.f)); break;
        case 'D': guest_arguments.push_back(Value::Float64(input.d)); break;
        case 'L': case '[': {
          if (mode == JniCallMode::CriticalNative) {
            failure = "CriticalNative cannot contain Java object parameters";
            break;
          }
          uint32_t token = 0;
          if (!state_->Adopt(state_->real->NewLocalRef(input.l),
                             false, &token, &failure)) break;
          guest_arguments.push_back(Value::GuestPointer(token));
          break;
        }
        default: failure = "unknown JNI32 argument type"; break;
      }
      if (!failure.empty()) break;
    }
    uint32_t receiver_token = 0;
    if (failure.empty() && mode != JniCallMode::CriticalNative &&
        !state_->Adopt(state_->real->NewLocalRef(receiver), false,
                       &receiver_token, &failure)) {
      // Adopt releases the new ref when token allocation fails.
    }
    // A host allocation failure while duplicating an object/class reference
    // must propagate its pending ART exception, never call native code with
    // an invented null token. Null Java arguments remain valid when no
    // exception was raised.
    if (failure.empty() && state_->real->ExceptionCheck())
      failure = "JNI32 host failed to create a local reference before guest entry";
    CallFrame frame;
    if (failure.empty() &&
        !PackJniCall(signature, mode,
                     mode == JniCallMode::CriticalNative ? 0 : GuestEnvironment(),
                     receiver_token, guest_arguments, &frame, &failure)) {
      // Reject invalid guest argument types before calling any native code.
    }
    if (failure.empty()) {
      std::vector<Value> full;
      full.reserve(guest_arguments.size() +
                   (mode == JniCallMode::CriticalNative ? 0 : 2));
      if (mode != JniCallMode::CriticalNative) {
        full.push_back(Value::GuestPointer(GuestEnvironment()));
        full.push_back(Value::GuestPointer(receiver_token));
      }
      full.insert(full.end(), guest_arguments.begin(), guest_arguments.end());
      called = state_->program.CallExport(library, export_name, full,
                                           signature.result, &returned,
                                           &failure, instruction_budget);
    }

    if (called && failure.empty()) {
      const uint32_t word = static_cast<uint32_t>(returned.bits);
      switch (ReturnTypeCode(descriptor)) {
        case 'V': break;
        case 'Z': host_result.z = static_cast<jboolean>(word); break;
        case 'B': host_result.b = static_cast<jbyte>(word); break;
        case 'C': host_result.c = static_cast<jchar>(word); break;
        case 'S': host_result.s = static_cast<jshort>(word); break;
        case 'I': host_result.i = static_cast<jint>(word); break;
        case 'J': host_result.j = static_cast<jlong>(returned.bits); break;
        case 'F': {
          const uint32_t bits = word;
          std::memcpy(&host_result.f, &bits, sizeof(bits));
          break;
        }
        case 'D': std::memcpy(&host_result.d, &returned.bits, sizeof(double)); break;
        case 'L': case '[': {
          jobject object = nullptr;
          if (!state_->Lookup(word, &object, &failure)) { called = false; break; }
          host_result.l = state_->real->NewLocalRef(object);
          has_result_local = host_result.l != nullptr;
          if (object != nullptr && host_result.l == nullptr &&
              !state_->real->ExceptionCheck()) {
            called = false;
            failure = "JNI32 could not duplicate guest return reference";
          }
          break;
        }
        default: called = false; failure = "invalid JNI32 return descriptor"; break;
      }
    }
  } catch (const std::exception& ex) {
    failure = std::string("JNI32 native argument marshal exception: ") + ex.what();
    called = false;
  } catch (...) {
    failure = "JNI32 native argument marshal exception";
    called = false;
  }

  std::string cleanup_error;
  const bool cleaned = ReleaseLocals(&cleanup_error);
  if (!cleaned && failure.empty()) failure = cleanup_error;
  if (!called || !cleaned) {
    if (has_result_local && state_->real)
      state_->real->DeleteLocalRef(host_result.l);
    if (error) *error = failure.empty() ? "JNI32 guest native call failed" : failure;
    return false;
  }
  // Do not publish any partial JNI return value until native execution,
  // reference duplication and frame cleanup all succeed.
  *result = host_result;
  if (error) error->clear();
  return true;
}
}  // namespace gesher::arm32
