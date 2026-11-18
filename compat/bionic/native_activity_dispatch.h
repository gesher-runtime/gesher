/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "abi_types.h"
#include <gesher/arm64_runtime.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <cstdlib>

namespace gesher::bionic::native_activity {

// NDK headers describe host function-pointer types, while callbacks written
// by an Android ELF object have that object's ISA/ABI. Use actual loader/guest
// ownership, never a filename or the process's most recent load, to choose the
// existing System V entry thunk for an AArch64 address.
inline void* Entry(void* address) {
  if (address == nullptr) return nullptr;
  elf::Library* owner = elf::FindCaller(address);
  const bool guest = owner != nullptr ? elf::IsAarch64(owner) : arm64::IsGuestCode(address);
  return guest ? arm64::HostEntry(address) : address;
}

template <typename F> struct SysvOf;
template <typename R, typename... A> struct SysvOf<R (*)(A...)> {
  using type = R(BIONIC_SYSV*)(A...);
};

template <typename F, typename... Args>
auto Call(F callback, Args... args) {
  EnterLoadedCode();
  void* entry = Entry(reinterpret_cast<void*>(callback));
  // There is no Android callback error return for an inability to prepare the
  // native entry. Never execute a raw guest PC or fabricate a callback result.
  if (entry == nullptr) std::abort();
  return reinterpret_cast<typename SysvOf<F>::type>(entry)(args...);
}

}  // namespace gesher::bionic::native_activity
