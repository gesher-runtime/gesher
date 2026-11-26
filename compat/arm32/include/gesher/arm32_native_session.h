// gesher: narrowly scoped ARM32 native-library session for a supplied
// extracted APK native-library directory. Not yet wired to ART/dalvikvm.
#pragma once

#include <gesher/arm32_bionic.h>
#include <gesher/arm32_jni.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace gesher::arm32 {

// Loads an ordinary ARM EABI5 ELF32 native DSO from a caller-supplied
// directory. DT_NEEDED only resolves file basenames *within that directory*;
// no absolute paths, traversal, host pointer casts or implicit host libc.
// This is the staged loader used to test JNI_OnLoad and library lifecycle;
// full Android linker namespaces, app packaging and real ART registration
// remain a separate integration gate.
class NativeLibrarySession {
 public:
  NativeLibrarySession(std::filesystem::path directory, JNIEnv* host_env,
                       size_t memory_budget = GuestMemory::kDefaultBudget);
  ~NativeLibrarySession();
  NativeLibrarySession(const NativeLibrarySession&) = delete;
  NativeLibrarySession& operator=(const NativeLibrarySession&) = delete;

  // Reads all requested guest objects with a bounded, read-only provider;
  // installs an explicit Bionic32/JNI32 domain and runs guest constructors.
  // If require_jni_onload is true, a JNI_OnLoad export must exist, return a
  // supported version and run successfully before Open returns.
  // A real host ART RegisterNatives handler must be supplied before
  // JNI_OnLoad for libraries that register their natives at startup. Never
  // acknowledge registration without installing callable ART trampolines.
  bool Open(std::string_view soname, bool require_jni_onload,
            std::string* error = nullptr,
            NativeRegistrationHandler registration_handler = {});

  // After Open, invoke a named ARM32 Java_* native with exact JVM descriptor.
  // This still uses an injected host JNIEnv. It does not register a method
  // with ART or expose a host callable native function pointer.
  bool CallNative(std::string_view symbol, std::string_view descriptor,
                  JniCallMode mode, jobject receiver, const jvalue* args,
                  size_t argument_count, jvalue* result,
                  std::string* error = nullptr);

  // JNI_OnUnload, guest DT_FINI_ARRAY and all guest mappings close in order.
  // The session is single-use. Repeated Close is idempotent.
  bool Close(std::string* error = nullptr);
  bool IsOpen() const { return state_ == State::Open; }
  Jni32& Bridge() { return jni_; }
  Program& GuestProgram() { return program_; }

 private:
  enum class State { Fresh, Open, Closed };
  bool Supply(std::string_view name, std::vector<uint8_t>* bytes,
              std::string* error);
  static bool SafeSoname(std::string_view name);

  std::filesystem::path directory_;
  Program program_;
  Bionic32 bionic_;
  Jni32 jni_;
  std::string root_;
  State state_ = State::Fresh;
  bool onload_invoked_ = false;
  static constexpr uintmax_t kMaxLibraryFile = 256ull * 1024 * 1024;
};

}  // namespace gesher::arm32
