// Execute the real FS rewriter's local-exec stubs without building AOSP.
#include <elf.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "fs_rewrite.h"
#include "tls.h"

namespace elf = gesher::elf;
namespace {
int failures;
void Check(const char* name, bool ok) {
  std::printf("%s  exec3 TLS: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++failures;
}
void* Rewrite(const std::vector<uint8_t>& code, size_t tls_size = 64, bool supported = true) {
  alignas(Elf64_Ehdr) std::array<uint8_t, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr)> file{};
  auto* header = reinterpret_cast<Elf64_Ehdr*>(file.data());
  header->e_phoff = sizeof(Elf64_Ehdr);
  header->e_phnum = 1;
  auto* segment = reinterpret_cast<Elf64_Phdr*>(file.data() + header->e_phoff);
  segment->p_type = PT_LOAD;
  segment->p_flags = PF_R | PF_X;
  segment->p_filesz = code.size();
  auto* mapping = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!mapping) return nullptr;
  std::memcpy(mapping, code.data(), code.size());
  elf::FsRewriteStats stats;
  std::string error;
  if (!elf::RewriteFsAccesses("exec3-local-probe", file.data(), file.size(), mapping, &stats, &error, tls_size)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return nullptr;
  }
  if (supported ? stats.trapped != 0 || stats.stubbed == 0 : stats.trapped != 1) return nullptr;
  DWORD old;
  if (!VirtualProtect(mapping, 4096, PAGE_EXECUTE_READ, &old)) return nullptr;
  FlushInstructionCache(GetCurrentProcess(), mapping, code.size());
  return mapping; // The rewriter retains diagnostics for the mapping's lifetime.
}
template<class T> void Store(uint8_t* to, T value) { std::memcpy(to, &value, sizeof(value)); }
} // namespace

int main() {
  alignas(64) static uint8_t image[64]{};
  constexpr uint64_t initial = 0x1122334455667788ULL;
  Store(image, initial);
  Store(image + 16, 3.25);
  elf::TlsModule module;
  module.present = true;
  module.init_image = image;
  module.init_size = module.total_size = sizeof(image);
  module.align = 64;
  if (!elf::TlsAssignModule(&module, true)) return 1;
  elf::TlsPublishModule(&module);
  if (!elf::TlsSetupThread()) return 1;
  auto* block = static_cast<uint8_t*>(elf::TlsGetAddr(module.module_id, 0));
  using Integer = uint64_t (*)();
  using Floating = double (*)();
  auto load = reinterpret_cast<Integer>(Rewrite({0x64,0x48,0x8B,0x04,0x25,0xC0,0xFF,0xFF,0xFF,0xC3}));
  Check("absolute integer load", load && load() == initial);
  auto store = reinterpret_cast<Integer>(Rewrite({
      0x48,0xB9,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
      0x48,0xB8,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
      0x64,0x48,0x89,0x04,0x25,0xC8,0xFF,0xFF,0xFF,
      0x48,0x89,0xC8,0xC3}));
  uint64_t stored = 0;
  if (store) stored = store();
  uint64_t memory;
  std::memcpy(&memory, block + 8, sizeof(memory));
  Check("store preserves its input and scratch registers", store && stored == initial && memory == 0x8877665544332211ULL);
  Store(block + 24, UINT64_MAX);
  auto flags = reinterpret_cast<Integer>(Rewrite({
      0xF9,0x64,0x48,0x83,0x14,0x25,0xD8,0xFF,0xFF,0xFF,0x00,
      0x0F,0x92,0xC0,0x0F,0xB6,0xC0,0xC3}));
  Check("carry input and result flags survive the stub", flags && flags() == 1);
  auto high_byte = reinterpret_cast<Integer>(Rewrite({
      0x48,0xB8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
      0x64,0x88,0x24,0x25,0xE0,0xFF,0xFF,0xFF,
      0x64,0x8A,0x04,0x25,0xE0,0xFF,0xFF,0xFF,0x0F,0xB6,0xC0,0xC3}));
  Check("legacy AH byte registers keep their encoding", high_byte && high_byte() == 0x77);
  auto scalar = reinterpret_cast<Floating>(Rewrite({0xF2,0x64,0x0F,0x10,0x04,0x25,0xD0,0xFF,0xFF,0xFF,0xC3}));
  Check("SSE scalar double load", scalar && scalar() == 3.25);
  auto vector = reinterpret_cast<Integer>(Rewrite({
      0xF3,0x64,0x0F,0x6F,0x04,0x25,0xE0,0xFF,0xFF,0xFF,
      0xF3,0x64,0x0F,0x7F,0x04,0x25,0xF0,0xFF,0xFF,0xFF,
      0x66,0x48,0x0F,0x7E,0xC0,0xC3}));
  for (int i = 0; i < 16; ++i) block[32 + i] = static_cast<uint8_t>(i + 1);
  Check("SSE vector load and store", vector && vector() == 0x0807060504030201ULL && std::memcmp(block + 32, block + 48, 16) == 0);
  Store(block, uint64_t{3});
  auto bitwise = reinterpret_cast<Integer>(Rewrite({
      0x64,0x48,0x83,0x24,0x25,0xC0,0xFF,0xFF,0xFF,0x01,
      0x64,0x48,0x8B,0x04,0x25,0xC0,0xFF,0xFF,0xFF,0xC3}));
  Check("opcode extensions are not confused with register operands", bitwise && bitwise() == 1);
  Store(block, initial);
  bool thread_ok = false;
  std::thread worker([&] {
    if (!elf::TlsSetupThread()) return;
    auto* other = static_cast<uint8_t*>(elf::TlsGetAddr(module.module_id, 0));
    thread_ok = load && load() == initial;
    Store(other, uint64_t{99});
    thread_ok = thread_ok && load() == 99;
  });
  worker.join();
  Check("rewritten loads use each thread's own initialized block", thread_ok && load && load() == initial);
  Check("negative accesses without an executable layout stay trapped", Rewrite({0x64,0x48,0x8B,0x04,0x25,0xC0,0xFF,0xFF,0xFF,0xC3}, 0, false) != nullptr);
  Check("negative accesses outside the executable block stay trapped", Rewrite({0x64,0x48,0x8B,0x04,0x25,0xBF,0xFF,0xFF,0xFF,0xC3}, 64, false) != nullptr);
  Check("accesses observing the stack pointer stay trapped", Rewrite({0x64,0x48,0x89,0x24,0x25,0xC0,0xFF,0xFF,0xFF,0xC3}, 64, false) != nullptr);
  std::printf("exec3 TLS probe: %d failed\n", failures);
  return failures ? 1 : 0;
}
