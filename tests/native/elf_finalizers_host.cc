// Copyright (C) 2026 The Gesher Authors; SPDX-License-Identifier: Apache-2.0
// Complete production Close and lookup, with explicit owned fixture metadata.
// Only ARM Call is observed; this does not execute the translator.
#include "elf_loader.cc"
#include <atomic>
#include <chrono>
#include <thread>
using Finalizer = void (SYSV*)();
static std::vector<const void*> translated;
namespace gesher::arm64 {
uint64_t Call(const void* function, const uint64_t* arguments, size_t count) {
  if (arguments != nullptr || count != 0) std::abort();
  translated.push_back(function);
  return 0;
}
}
static gesher::elf::Library* Owner(bool arm, Finalizer* array, size_t count, Finalizer fini) {
  namespace elf = gesher::elf;
  auto* lib = new elf::Library();
  lib->is_aarch64 = arm; lib->fini_array = array; lib->fini_array_count = count; lib->fini_func = fini;
#if GESHER_HAS_NAMESPACES
  lib->name_space = elf::DefaultNamespace(); lib->local_group_root = lib;
  lib->target_sdk_version = 22;  // Local visibility before SDK23, without GLOBAL retention.
  lib->name_space->libraries.push_back(lib);
#endif
  elf::Loaded().push_back(lib);
  return lib;
}
using gesher::elf::Library;
using gesher::elf::Close;
static int order;
static Library* reentrant;
static bool entered;
static bool visible_during_finalization;
static std::atomic<int> concurrent_calls{0};
static void SYSV One() { order = order * 10 + 1; }
static void SYSV Two() { order = order * 10 + 2; }
static void SYSV Three() { order = order * 10 + 3; }
static void SYSV Reentrant() {
  order++;
  visible_during_finalization = gesher::elf::LookupGlobal("finalizer_visible") == &order;
  if (!entered) { entered = true; Close(reentrant); }
}
static void SYSV Concurrent() {
  ++concurrent_calls;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
}
static int checks;
static void Check(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
  ++checks;
}
int main() {
  Close(nullptr);
  Finalizer callbacks[] = {One, Two};
  Library& native = *Owner(false, callbacks, 2, Three);
  Close(&native);
  Check(order == 213, "reverse native array then DT_FINI");
  Close(&native);
  Check(order == 213, "native finalizers run once");
  order = 0;
  Library& guest = *Owner(true, callbacks, 2, Three);
  Close(&guest);
  Check(order == 0, "ARM64 callbacks do not execute as host instructions");
  Check(translated == std::vector<const void*>{reinterpret_cast<void*>(Two),
        reinterpret_cast<void*>(One), reinterpret_cast<void*>(Three)}, "translated order and routing");
  Close(&guest);
  Check(translated.size() == 3, "translated finalizers run once");
  Finalizer sentinels[] = {nullptr, reinterpret_cast<Finalizer>(-1), One};
  Library& empty = *Owner(false, sentinels, 3, reinterpret_cast<Finalizer>(-1));
  Close(&empty);
  Check(order == 1, "null and minus-one finalizer entries are skipped");
  Library& recursive = *Owner(false, nullptr, 0, Reentrant);
  static Elf64_Sym symbols[2]{};
  static const uint32_t bucket[] = {1}, chain[] = {0, 0};
  symbols[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
  symbols[1].st_shndx = 1;
  symbols[1].st_name = 1;
  symbols[1].st_size = sizeof(order);
  recursive.symtab = symbols;
  recursive.strtab = "\0finalizer_visible";
  recursive.load_bias = reinterpret_cast<uint8_t*>(&order);
  recursive.has_sysv_hash = true;
  recursive.sysv_nbucket = 1; recursive.sysv_nchain = 2;
  recursive.sysv_buckets = bucket; recursive.sysv_chain = chain;
  reentrant = &recursive; order = 0;
  Check(gesher::elf::LookupGlobal("finalizer_visible") == &order, "symbol visible before Close");
  Close(&recursive);
  Check(order == 1, "recursive close does not repeat finalization");
  Check(visible_during_finalization, "symbol remains visible while finalizers run");
  Check(gesher::elf::LookupGlobal("finalizer_visible") == nullptr, "symbol hidden after Close");
  Library& shared = *Owner(false, nullptr, 0, Concurrent);
  std::vector<std::thread> threads;
  for (int i = 0; i < 12; ++i) threads.emplace_back([&] { Close(&shared); });
  for (auto& thread : threads) thread.join();
  Check(concurrent_calls.load() == 1, "concurrent close claims finalizers once");
  std::printf("ELF finalizer contracts all passed: %d checks\n", checks);
}
