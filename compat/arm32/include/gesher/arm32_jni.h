// gesher: the first real, pointer-width-safe ARM32 -> 64-bit ART JNI boundary.
// Every guest JNIEnv/JavaVM-visible address is a 32-bit guest address or a
// generation-tagged opaque reference. Never narrow a host jobject or JNIEnv*.
// Partial JNI coverage: unimplemented table slots are null and fault closed.
#pragma once

#include <gesher/arm32_program.h>

#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>

namespace gesher::arm32 {

// A host JNIEnv is valid on its owning Java thread only. Jni32 is bound to
// that thread and its Program; the Program must outlive Jni32. The caller
// must end the guest JNI frame using ReleaseLocals() before returning to ART.
// This component does NOT install native methods in ART, translate callbacks,
// provide JavaVM attach/detach, or constitute full JNI32 support.
class Jni32 {
 public:
  Jni32(Program& program, JNIEnv* real_env);
  ~Jni32();
  Jni32(const Jni32&) = delete;
  Jni32& operator=(const Jni32&) = delete;

  // Maps a little-endian JNIEnv pointer and its function table into guest
  // pages and binds the implemented slots to typed guest SVC thunks. All
  // unimplemented slots stay null. Failed setup is not retryable.
  bool Install(std::string* error = nullptr);

  // Return the guest address of JNIEnv's first word (function-table pointer).
  // Must be called after Install(); no host pointer is returned.
  uint32_t GuestEnvironment() const;

  // Guest JavaVM* has its own 32-bit invoke-interface table. GetEnv and
  // AttachCurrentThread on the existing owning ART thread are implemented;
  // new guest threads and detachment are not supported.
  uint32_t GuestJavaVM() const;

  // Enter the DSO's JNI_OnLoad(JavaVM*, void*) through AAPCS32 after its
  // constructors have been run. A missing JNI_OnLoad export is an error here:
  // the caller decides whether the library declares one. No ART registration
  // is performed by this helper.
  bool CallOnLoad(std::string_view library, jint* version,
                  std::string* error = nullptr,
                  uint64_t instruction_budget = 1000000);

  // Execute an exported JNI_OnUnload(JavaVM*, void*) while the guest DSO is
  // still mapped. The caller must check whether JNI_OnUnload is present and
  // call this before Program::Shutdown; no actual ART unload is hooked here.
  bool CallOnUnload(std::string_view library,
                    std::string* error = nullptr,
                    uint64_t instruction_budget = 1000000);

  // Enter a named ARM32 JNI method from the owning ART thread. The descriptor
  // governs every argument and the result; jobjects are copied into
  // generation-checked 32-bit guest tokens, never narrowed from 64-bit host
  // addresses. CriticalNative omits JNIEnv and receiver entirely and accepts
  // primitives only. Calling this does not register the method in ART.
  // On success a returned jobject is a new host local reference for ART.
  // Every temporary guest local reference is released at the return boundary.
  bool CallNative(std::string_view library, std::string_view export_name,
                  std::string_view descriptor, JniCallMode mode,
                  jobject receiver_or_class, const jvalue* arguments,
                  size_t argument_count, jvalue* result,
                  std::string* error = nullptr,
                  uint64_t instruction_budget = 1000000);

  // Clear all local reference tokens (and their real JNI local references)
  // at the native method's return boundary. Global references survive.
  // Must run on the owning thread after guest execution has stopped.
  bool ReleaseLocals(std::string* error = nullptr);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace gesher::arm32
