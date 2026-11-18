/* Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0 */
// Usage: native_activity_guest_test <fixture-directory> [--arm64]
#include "native_activity_dispatch.h"
#include "native_activity_guest_contract.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace elf = gesher::elf;
namespace activity = gesher::bionic::native_activity;
namespace {
int checks;
void Check(bool value, const char* reason) {
  if (!value) { std::fprintf(stderr, "FAIL NativeActivity dispatch: %s\n", reason); std::exit(1); }
  ++checks;
}
void BIONIC_SYSV HostCallback(ActivityFixtureState* state) { ++state->starts; }
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) return 2;
  const bool arm = argc == 3 && std::strcmp(argv[2], "--arm64") == 0;
  gesher::bionic::InstallBionicShim();
  elf::NamespaceOptions config;
  config.name = "native-activity-dispatch-fixture";
  config.default_library_paths = {argv[1]};
  config.permitted_paths = {argv[1]};
  std::string error;
  elf::Namespace* ns = elf::CreateNamespace(config, &error);
  Check(ns != nullptr, "fixture gets an isolated actual namespace");
  elf::LoadOptions options;
  options.context.name_space = ns;
  elf::Library* library = elf::Load((std::string(argv[1]) + "/libnativeactivity_fixture.so").c_str(), &error, options);
  if (library == nullptr) std::fprintf(stderr, "%s\n", error.c_str());
  Check(library != nullptr && elf::IsAarch64(library) == arm, "actual compiler-built ELF has requested ISA");
  void* create = elf::Lookup(library, "NativeActivityFixture_create");
  Check(create != nullptr && elf::FindCaller(create) == library, "onCreate-shaped export has real ELF ownership");
  void* entry = activity::Entry(create);
  Check(entry != nullptr && (arm ? entry != create : entry == create),
        "production entry dispatch translates guest PC and preserves native x86 address");
  ActivityFixtureState state{};
  uint64_t saved = UINT64_C(0x1234abcdef567890);
  using Create = void(BIONIC_SYSV*)(ActivityFixtureState*, void*, size_t);
  reinterpret_cast<Create>(entry)(&state, &saved, sizeof(saved));
  Check(state.marker == saved && state.input_size == sizeof(saved), "onCreate-shaped saved-state pointer/size reach guest");
  ActivityFixtureCallbacks callbacks{};
  using Fill = void(*)(ActivityFixtureCallbacks*);
  activity::Call(reinterpret_cast<Fill>(elf::Lookup(library, "NativeActivityFixture_callbacks")), &callbacks);
  Check(elf::FindCaller(reinterpret_cast<void*>(callbacks.start)) == library,
        "callbacks written by guest retain their actual library ownership");
  activity::Call(callbacks.start, &state);
  activity::Call(callbacks.start, &state);
  Check(state.starts == 2, "typed start callbacks execute actual guest code twice");
  activity::Call(callbacks.focus, &state, -17);
  Check(state.focused == -17, "typed focus callback preserves signed int argument");
  activity::Call(callbacks.window, &state, static_cast<void*>(&saved));
  Check(state.window == &saved, "typed window callback preserves host/guest shared pointer");
  ActivityFixtureRect bounds{-3, 5, 401, 223};
  activity::Call(callbacks.rect, &state, &bounds);
  Check(state.rect[0] == -3 && state.rect[1] == 5 && state.rect[2] == 401 && state.rect[3] == 223,
        "typed content-rectangle callback reads actual pointed-to fields");
  size_t bytes = 0;
  void* saved_pointer = activity::Call(callbacks.save, &state, &bytes);
  Check(saved_pointer == &state.marker && bytes == sizeof(saved),
        "saved-state callback preserves pointer return and size_t output");
  Check(activity::Entry(reinterpret_cast<void*>(&HostCallback)) == reinterpret_cast<void*>(&HostCallback),
        "an actual host callback is not reclassified as guest code");
  using Start = void(*)(ActivityFixtureState*);
  activity::Call(reinterpret_cast<Start>(&HostCallback), &state);
  Check(state.starts == 3, "typed host callback continues to execute normally");
  std::printf("NativeActivity dispatch contracts: %d checks; architecture=%s\n", checks,
               arm ? "AArch64 through real Dynarmic" : "x86-64 native ELF");
  std::puts("BOUNDARY dispatch helper + real ELF/guest execution; no Windows ART NativeActivity/UI lifecycle assertion");
  return 0;
}
