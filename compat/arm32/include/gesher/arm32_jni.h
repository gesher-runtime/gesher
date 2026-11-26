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

  // Clear all local reference tokens (and their real JNI local references)
  // at the native method's return boundary. Global references survive.
  // Must run on the owning thread after guest execution has stopped.
  bool ReleaseLocals(std::string* error = nullptr);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

}  // namespace gesher::arm32
