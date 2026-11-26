// gesher: a checked ELF32 ARM image for the experimental guest executor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace gesher::arm32 {

struct ElfSegment {
  uint32_t address = 0;  // Guest address, never a host pointer.
  uint32_t size = 0;
  uint32_t flags = 0;    // ELF PF_X=1, PF_W=2, PF_R=4.
};

struct ElfSymbol {
  std::string name;
  uint32_t address = 0;  // A Thumb function retains bit zero.
  uint32_t size = 0;
  uint8_t binding = 0;
  uint8_t type = 0;
};

struct ElfRelroRange {
  uint32_t address = 0;
  uint32_t size = 0;
};

// An owned, relocated image. bytes[0] belongs at guest_base, and holes between
// segments must remain inaccessible when the executor maps it. These bytes
// are never executable host memory. Loading does not run any guest code.
struct ElfImage {
  uint32_t guest_base = 0;
  uint32_t load_bias = 0;
  uint32_t entry = 0;
  std::string soname;
  // DT_NEEDED names in declared order, after the caller's dependency callback
  // has accepted each name. No dependency image is owned by this object.
  std::vector<std::string> dependencies;
  std::vector<uint8_t> bytes;
  std::vector<ElfSegment> segments;
  // Validated PT_GNU_RELRO byte ranges. The executor applies read-only
  // protection to their covering guest pages after writing relocated bytes.
  std::vector<ElfRelroRange> relro;
  std::vector<ElfSymbol> symbols;
  // Invocation order: DT_INIT then INIT_ARRAY; reversed FINI_ARRAY then DT_FINI.
  std::vector<uint32_t> constructors;
  std::vector<uint32_t> finalizers;

  bool Lookup(std::string_view name, uint32_t* address) const;
};

struct ElfLoadOptions {
  uint32_t load_bias = 0x10000;
  size_t max_image_size = 256 * 1024 * 1024;
  size_t max_symbols = 1024 * 1024;
  size_t max_relocations = 1024 * 1024;
  // An existing guest function/data address. The caller must not truncate a
  // host pointer here. false means unresolved; unresolved weak references
  // follow AAELF32's absolute/PC-relative rules. No host function is called.
  std::function<bool(std::string_view name, uint32_t* address)> resolve_import;
  // Called once for each DT_NEEDED before resolving undefined symbols.
  // The caller is responsible for dependency namespaces, recursively loading
  // libraries, cycle detection, lifetime and rollback of external side effects.
  // Without this callback, DT_NEEDED is rejected instead of pretending a
  // missing Android library is present. Callback exceptions fail the load.
  std::function<bool(std::string_view soname)> resolve_dependency;
};

// Loads a little-endian ARM EABI5 softfp ET_DYN image using checked ELF32
// reads. Supports ordinary REL relocations: NONE, RELATIVE, ABS32, REL32,
// GLOB_DAT and JUMP_SLOT, standard and legacy Android RELR tables, Android\n// APS2 packed REL groups, SysV/GNU dynamic hashes, BSS and init/fini arrays.
//
// This is the loader core for an experimental executor, not an Android app
// runtime. DT_NEEDED requires an explicit caller-managed dependency resolver;
// TLS, IFUNC, packed RELA relocations, versioned
// symbols, an interpreter and preinit arrays are explicitly unsupported.
// Only System V/Linux OSABI with ABI version zero is accepted; FDPIC and
// unknown ABI variants are rejected. Unknown dynamic tags are rejected.
// Unsupported features and every relocation are checked before relocation
// writes. A failure leaves *image unchanged and sets *error, if nonnull.
// Allocation failure and importer exceptions return false too; if even the
// diagnostic cannot be allocated, *error is cleared.
bool LoadElf32(const void* data, size_t size, const ElfLoadOptions& options,
               ElfImage* image, std::string* error);

}  // namespace gesher::arm32
