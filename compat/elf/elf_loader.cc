// gesher: the ELF loader (gesher/elf_loader.h). See docs/native-libs.md.

#include <gesher/elf_loader.h>

#include <elf.h>
#include <link.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <gesher/arm64_runtime.h>

#include "fs_rewrite.h"
#include "tls.h"

// DT_RELR's entry type; this elf.h predates it.
using Elf64_Relr = uint64_t;

// AArch64 e_machine and relocations (this elf.h may lack the aarch64 set).
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_ABS64
#define R_AARCH64_ABS64 257
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL64 1030
#define R_AARCH64_TLSDESC 1031
#define R_AARCH64_IRELATIVE 1032
#endif

// Loaded Android code is System V ABI (docs/decisions/0006-managed-abi.md), so
// every call into it (init, ifunc resolvers) goes through a sysv_abi pointer.
#define SYSV __attribute__((sysv_abi))

namespace gesher::elf {

namespace {

constexpr size_t kPageSize = 0x1000;

size_t PageDown(size_t v) { return v & ~(kPageSize - 1); }
size_t PageUp(size_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

// The GNU hash of a symbol name (DT_GNU_HASH).
uint32_t GnuHash(const char* name) {
  uint32_t h = 5381;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(name); *p != 0; ++p) {
    h += (h << 5) + *p;
  }
  return h;
}

// The System V hash of a symbol name (DT_HASH).
uint32_t SysvHash(const char* name) {
  uint32_t h = 0, g;
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(name); *p != 0; ++p) {
    h = (h << 4) + *p;
    g = h & 0xf0000000;
    if (g != 0) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

}  // namespace

// A loaded object. One per .so.
struct Library {
  std::string name;       // file base name or given name
  std::string soname;     // DT_SONAME, else name
  uint8_t* base = nullptr;   // where the first PT_LOAD landed
  uint8_t* load_bias = nullptr;  // base - first PT_LOAD p_vaddr
  size_t map_size = 0;       // total reserved span
  bool is_closed = false;
  bool is_aarch64 = false;   // EM_AARCH64: translated guest code (ADR 0047)

  // Dynamic section data (pointers into the mapping).
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  size_t strtab_size = 0;

  // GNU hash.
  bool has_gnu_hash = false;
  uint32_t gnu_nbucket = 0;
  uint32_t gnu_symndx = 0;     // first symbol covered by the hash
  uint32_t gnu_maskwords = 0;
  uint32_t gnu_shift2 = 0;
  const uint64_t* gnu_bloom = nullptr;
  const uint32_t* gnu_buckets = nullptr;
  const uint32_t* gnu_chain = nullptr;

  // SysV hash (fallback, and it gives the symbol count).
  bool has_sysv_hash = false;
  uint32_t sysv_nbucket = 0;
  uint32_t sysv_nchain = 0;     // == number of symbols
  const uint32_t* sysv_buckets = nullptr;
  const uint32_t* sysv_chain = nullptr;

  // Relocations.
  const Elf64_Rela* rela = nullptr;
  size_t rela_count = 0;
  const Elf64_Rela* jmprel = nullptr;
  size_t jmprel_count = 0;
  const Elf64_Relr* relr = nullptr;
  size_t relr_count = 0;

  // Init/fini. System V, like all loaded code.
  void (SYSV* init_func)() = nullptr;
  void (SYSV* fini_func)() = nullptr;
  void (SYSV** init_array)() = nullptr;
  size_t init_array_count = 0;
  void (SYSV** fini_array)() = nullptr;
  size_t fini_array_count = 0;
  bool init_done = false;

  // TLS (PT_TLS).
  TlsModule tls{};

  // What the %fs rewrite did (fs_rewrite.h).
  FsRewriteStats fs{};

  // The program headers inside the mapping, for dl_iterate_phdr.
  const Elf64_Phdr* phdr = nullptr;
  uint16_t phnum = 0;

  // LoadOptions::stub_unresolved_functions for this load.
  bool stub_unresolved = true;

  std::vector<std::string> needed;
  std::vector<Library*> dependencies;
};

namespace {

std::mutex g_mutex;
std::vector<Library*>& Loaded() {
  static std::vector<Library*> v;
  return v;
}
SymbolResolver g_resolver;
uint64_t g_load_count = 0;  // under g_mutex
UnresolvedHandler g_unresolved_handler = nullptr;

// --- stubs for unresolved function imports ---------------------------------

// Called by a stub, System V, with the symbol's name.
void SYSV UnresolvedCalled(const char* name) {
  if (g_unresolved_handler != nullptr) g_unresolved_handler(name);
  std::fprintf(stderr, "gesher elf: called unresolved function '%s'\n", name);
  std::fflush(stderr);
  std::abort();
}

// A stub is `mov rdi, imm64(name); mov rax, imm64(UnresolvedCalled); jmp rax`.
constexpr size_t kStubSize = 32;
std::mutex g_stub_mutex;
uint8_t* g_stub_page = nullptr;
size_t g_stub_used = 0;
constexpr size_t kStubPageSize = 0x10000;

void* MakeUnresolvedStub(const char* name) {
  std::lock_guard<std::mutex> lock(g_stub_mutex);
  if (g_stub_page == nullptr || g_stub_used + kStubSize > kStubPageSize) {
    g_stub_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kStubPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub_page == nullptr) return nullptr;
    g_stub_used = 0;
  }
  uint8_t* p = g_stub_page + g_stub_used;
  g_stub_used += kStubSize;
  // The name must outlive the object's strtab only as long as the object; it
  // lives in the mapping, which is never unmapped.
  uint64_t name_ptr = reinterpret_cast<uint64_t>(name);
  uint64_t target = reinterpret_cast<uint64_t>(&UnresolvedCalled);
  p[0] = 0x48; p[1] = 0xBF;  // mov rdi, imm64
  std::memcpy(p + 2, &name_ptr, 8);
  p[10] = 0x48; p[11] = 0xB8;  // mov rax, imm64
  std::memcpy(p + 12, &target, 8);
  p[20] = 0xFF; p[21] = 0xE0;  // jmp rax
  FlushInstructionCache(GetCurrentProcess(), p, kStubSize);
  return p;
}

// --- placeholders for unresolved data imports ---------------------------------
// A data import nothing defines (an object the shim lacks) is bound to its own
// slot in a reserved, inaccessible region instead of failing the load, so an
// object whose missing data is never touched still loads. Touching it faults;
// the handler below names the symbol and aborts.
constexpr size_t kDataSlot = 256;
constexpr size_t kDataRegionSize = size_t{1} << 20;  // 4096 slots
std::mutex g_data_mutex;
uint8_t* g_data_region = nullptr;
size_t g_data_used = 0;
const char* g_data_names[kDataRegionSize / kDataSlot];

LONG CALLBACK UnresolvedDataHandler(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* r = info->ExceptionRecord;
  if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
      g_data_region == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  auto addr = static_cast<uintptr_t>(r->ExceptionInformation[1]);
  auto base = reinterpret_cast<uintptr_t>(g_data_region);
  if (addr < base || addr >= base + kDataRegionSize) return EXCEPTION_CONTINUE_SEARCH;
  size_t slot = (addr - base) / kDataSlot;
  const char* name = slot < g_data_used / kDataSlot ? g_data_names[slot] : "?";
  std::fprintf(stderr,
               "gesher elf: loaded code accessed data symbol '%s' (+%zu), which nothing "
               "defines (the bionic shim lacks it)\n",
               name, static_cast<size_t>((addr - base) % kDataSlot));
  std::fflush(stderr);
  std::abort();
}

void* MakeUnresolvedData(const char* name) {
  std::lock_guard<std::mutex> lock(g_data_mutex);
  if (g_data_region == nullptr) {
    g_data_region = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kDataRegionSize, MEM_RESERVE, PAGE_NOACCESS));
    if (g_data_region == nullptr) return nullptr;
    AddVectoredExceptionHandler(1, UnresolvedDataHandler);
  }
  if (g_data_used + kDataSlot > kDataRegionSize) return nullptr;
  g_data_names[g_data_used / kDataSlot] = name;
  void* p = g_data_region + g_data_used;
  g_data_used += kDataSlot;
  return p;
}

bool SymbolDefined(const Elf64_Sym* sym) {
  return sym->st_shndx != SHN_UNDEF;
}

// Finds a symbol by name defined in `lib`. Returns the Elf64_Sym or nullptr.
const Elf64_Sym* FindDefinedSymbol(const Library* lib, const char* name, uint32_t gnu_hash,
                                   uint32_t sysv_hash) {
  if (lib->has_gnu_hash) {
    // Bloom filter.
    uint32_t h1 = gnu_hash;
    uint32_t word = (h1 / 64) & (lib->gnu_maskwords - 1);
    uint64_t mask = (uint64_t{1} << (h1 % 64)) |
                    (uint64_t{1} << ((h1 >> lib->gnu_shift2) % 64));
    if ((lib->gnu_bloom[word] & mask) != mask) return nullptr;

    uint32_t n = lib->gnu_buckets[h1 % lib->gnu_nbucket];
    if (n < lib->gnu_symndx) return nullptr;
    const uint32_t* chain = lib->gnu_chain;
    for (;; ++n) {
      uint32_t ch = chain[n - lib->gnu_symndx];
      if (((ch ^ h1) >> 1) == 0) {
        const Elf64_Sym* sym = &lib->symtab[n];
        if (SymbolDefined(sym) && std::strcmp(lib->strtab + sym->st_name, name) == 0) {
          return sym;
        }
      }
      if (ch & 1) break;
    }
    return nullptr;
  }
  if (lib->has_sysv_hash) {
    for (uint32_t n = lib->sysv_buckets[sysv_hash % lib->sysv_nbucket]; n != STN_UNDEF;
         n = lib->sysv_chain[n]) {
      const Elf64_Sym* sym = &lib->symtab[n];
      if (SymbolDefined(sym) && std::strcmp(lib->strtab + sym->st_name, name) == 0) {
        return sym;
      }
    }
  }
  return nullptr;
}

// Resolves an IFUNC: the symbol value is a resolver function returning the real
// address; it uses System V, like all loaded code.
void* ResolveIfunc(void* addr) {
  using Resolver = void* (SYSV*)();
  return reinterpret_cast<Resolver>(addr)();
}

// The address of a defined symbol, resolving IFUNCs. For a guest aarch64
// object the IFUNC resolver is guest code, run through the translator.
void* SymbolAddress(const Library* lib, const Elf64_Sym* sym) {
  void* addr = lib->load_bias + sym->st_value;
  if (ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC) {
    if (lib->is_aarch64) {
      addr = reinterpret_cast<void*>(gesher::arm64::Call(addr, nullptr, 0));
    } else {
      addr = ResolveIfunc(addr);
    }
  }
  return addr;
}

Library* FindLocked(const std::string& name) {
  for (Library* lib : Loaded()) {
    if (lib->soname == name || lib->name == name) return lib;
  }
  return nullptr;
}

// Looks up `name` as exported by any loaded object. Fills *out_lib/*out_sym.
void* LookupGlobalLocked(const char* name, const Library** out_lib, const Elf64_Sym** out_sym) {
  uint32_t gh = GnuHash(name), sh = SysvHash(name);
  for (Library* lib : Loaded()) {
    if (lib->is_closed) continue;
    const Elf64_Sym* sym = FindDefinedSymbol(lib, name, gh, sh);
    if (sym != nullptr && ELF64_ST_BIND(sym->st_info) != STB_LOCAL) {
      if (out_lib) *out_lib = lib;
      if (out_sym) *out_sym = sym;
      return SymbolAddress(lib, sym);
    }
  }
  return nullptr;
}

struct ResolvedSymbol {
  void* address = nullptr;
  const Library* lib = nullptr;   // nullptr for a shim symbol
  const Elf64_Sym* sym = nullptr;
  bool found = false;
};

// Resolves an undefined symbol for a relocation: first in the object itself and
// its dependency graph, then through the bionic shim resolver.
ResolvedSymbol ResolveForReloc(Library* lib, const char* name) {
  ResolvedSymbol r;
  uint32_t gh = GnuHash(name), sh = SysvHash(name);
  // Self first, then dependencies (global scope), as the dynamic linker does.
  const Elf64_Sym* sym = FindDefinedSymbol(lib, name, gh, sh);
  if (sym != nullptr && ELF64_ST_BIND(sym->st_info) != STB_LOCAL) {
    r = {SymbolAddress(lib, sym), lib, sym, true};
    return r;
  }
  for (Library* dep : lib->dependencies) {
    const Elf64_Sym* s = FindDefinedSymbol(dep, name, gh, sh);
    if (s != nullptr && ELF64_ST_BIND(s->st_info) != STB_LOCAL) {
      return {SymbolAddress(dep, s), dep, s, true};
    }
  }
  // Any other loaded object (global scope).
  const Library* gl = nullptr;
  const Elf64_Sym* gs = nullptr;
  if (void* a = LookupGlobalLocked(name, &gl, &gs)) {
    return {a, gl, gs, true};
  }
  // The bionic shim.
  if (g_resolver) {
    if (void* a = g_resolver(name)) {
      return {a, nullptr, nullptr, true};
    }
  }
  return r;
}

void Fail(std::string* error, const std::string& msg) {
  if (error) *error = msg;
}

// Reads the whole file into `buf`.
bool ReadFile(const char* path, std::vector<uint8_t>* buf, std::string* error) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    Fail(error, std::string("cannot open ") + path);
    return false;
  }
  struct stat st {};
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    Fail(error, std::string("cannot stat ") + path);
    return false;
  }
  buf->resize(static_cast<size_t>(st.st_size));
  size_t off = 0;
  while (off < buf->size()) {
    ssize_t n = pread(fd, buf->data() + off, buf->size() - off, static_cast<off_t>(off));
    if (n <= 0) {
      close(fd);
      Fail(error, std::string("short read on ") + path);
      return false;
    }
    off += static_cast<size_t>(n);
  }
  close(fd);
  return true;
}

int ProtFor(uint32_t p_flags) {
  int prot = 0;
  if (p_flags & PF_R) prot |= PROT_READ;
  if (p_flags & PF_W) prot |= PROT_WRITE;
  if (p_flags & PF_X) prot |= PROT_EXEC;
  return prot;
}

// Maps the PT_LOAD segments of `data` into a fresh reservation. Fills base,
// load_bias, map_size. Returns false on error.
bool MapSegments(Library* lib, const uint8_t* data, std::string* error) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);

  // Span of all PT_LOAD segments.
  uint64_t min_vaddr = UINT64_MAX, max_vaddr = 0;
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    min_vaddr = std::min(min_vaddr, phdr[i].p_vaddr);
    max_vaddr = std::max(max_vaddr, phdr[i].p_vaddr + phdr[i].p_memsz);
  }
  if (min_vaddr == UINT64_MAX) {
    Fail(error, "no PT_LOAD segments");
    return false;
  }
  min_vaddr = PageDown(min_vaddr);
  max_vaddr = PageUp(max_vaddr);
  lib->map_size = max_vaddr - min_vaddr;

  // Reserve the whole range as anonymous RW, then fill and protect each
  // segment. File-backed fixed mappings need placeholder mappings on Windows
  // (not implemented); copying the contents avoids that and costs a test .so
  // nothing to speak of.
  void* reservation = mmap(nullptr, lib->map_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (reservation == MAP_FAILED) {
    Fail(error, "mmap reservation failed");
    return false;
  }
  lib->base = static_cast<uint8_t*>(reservation);
  lib->load_bias = lib->base - min_vaddr;

  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    uint8_t* seg = lib->load_bias + phdr[i].p_vaddr;
    if (phdr[i].p_filesz > 0) {
      std::memcpy(seg, data + phdr[i].p_offset, phdr[i].p_filesz);
    }
    // .bss (p_memsz > p_filesz) stays zero from the anonymous mapping.
  }
  return true;
}

// After relocation, set each segment's final protection. For a guest aarch64
// object, executable segments are made readable but NOT executable: the host
// never runs guest code, the translator reads it (ADR 0047).
bool ProtectSegments(Library* lib, const uint8_t* data, std::string* error) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    uint8_t* start = lib->load_bias + PageDown(phdr[i].p_vaddr);
    size_t len = PageUp(phdr[i].p_vaddr + phdr[i].p_memsz) - PageDown(phdr[i].p_vaddr);
    int prot = ProtFor(phdr[i].p_flags);
    if (lib->is_aarch64 && (prot & PROT_EXEC)) prot = (prot & ~PROT_EXEC) | PROT_READ;
    if (mprotect(start, len, prot) != 0) {
      Fail(error, "mprotect of a segment failed");
      return false;
    }
  }
  return true;
}

// Registers a guest aarch64 object's executable segments with the translator,
// so branches into them run as guest code.
void RegisterGuestCode(Library* lib, const uint8_t* data) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD || !(phdr[i].p_flags & PF_X)) continue;
    uint8_t* start = lib->load_bias + PageDown(phdr[i].p_vaddr);
    size_t len = PageUp(phdr[i].p_vaddr + phdr[i].p_memsz) - PageDown(phdr[i].p_vaddr);
    gesher::arm64::AddCode(start, len, lib->soname.c_str());
  }
}

// Parses PT_DYNAMIC and PT_TLS.
void ParseDynamic(Library* lib, const uint8_t* data) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
  const Elf64_Dyn* dyn = nullptr;

  // The program headers as mapped: PT_PHDR, else the PT_LOAD holding e_phoff.
  lib->phnum = ehdr->e_phnum;
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type == PT_PHDR) {
      lib->phdr = reinterpret_cast<const Elf64_Phdr*>(lib->load_bias + phdr[i].p_vaddr);
    }
  }
  for (int i = 0; i < ehdr->e_phnum && lib->phdr == nullptr; ++i) {
    if (phdr[i].p_type == PT_LOAD && phdr[i].p_offset <= ehdr->e_phoff &&
        ehdr->e_phoff + ehdr->e_phnum * sizeof(Elf64_Phdr) <= phdr[i].p_offset + phdr[i].p_filesz) {
      lib->phdr = reinterpret_cast<const Elf64_Phdr*>(lib->load_bias + phdr[i].p_vaddr +
                                                      (ehdr->e_phoff - phdr[i].p_offset));
    }
  }

  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type == PT_DYNAMIC) {
      dyn = reinterpret_cast<const Elf64_Dyn*>(lib->load_bias + phdr[i].p_vaddr);
    } else if (phdr[i].p_type == PT_TLS) {
      lib->tls.present = true;
      lib->tls.init_image = lib->load_bias + phdr[i].p_vaddr;
      lib->tls.init_size = phdr[i].p_filesz;
      lib->tls.total_size = phdr[i].p_memsz;
      lib->tls.align = phdr[i].p_align ? phdr[i].p_align : 1;
    }
  }
  if (dyn == nullptr) return;

  size_t pltrel_size = 0;
  for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
    uint64_t v = d->d_un.d_val;
    uint8_t* p = lib->load_bias + d->d_un.d_ptr;
    switch (d->d_tag) {
      case DT_SYMTAB: lib->symtab = reinterpret_cast<const Elf64_Sym*>(p); break;
      case DT_STRTAB: lib->strtab = reinterpret_cast<const char*>(p); break;
      case DT_STRSZ: lib->strtab_size = v; break;
      case DT_SONAME: /* resolved after strtab is known */ break;
      case DT_HASH: {
        const uint32_t* h = reinterpret_cast<const uint32_t*>(p);
        lib->has_sysv_hash = true;
        lib->sysv_nbucket = h[0];
        lib->sysv_nchain = h[1];
        lib->sysv_buckets = h + 2;
        lib->sysv_chain = h + 2 + lib->sysv_nbucket;
        break;
      }
      case DT_GNU_HASH: {
        const uint32_t* h = reinterpret_cast<const uint32_t*>(p);
        lib->has_gnu_hash = true;
        lib->gnu_nbucket = h[0];
        lib->gnu_symndx = h[1];
        lib->gnu_maskwords = h[2];
        lib->gnu_shift2 = h[3];
        lib->gnu_bloom = reinterpret_cast<const uint64_t*>(h + 4);
        lib->gnu_buckets = reinterpret_cast<const uint32_t*>(lib->gnu_bloom + lib->gnu_maskwords);
        lib->gnu_chain = lib->gnu_buckets + lib->gnu_nbucket;
        break;
      }
      case DT_RELA: lib->rela = reinterpret_cast<const Elf64_Rela*>(p); break;
      case DT_RELASZ: lib->rela_count = v / sizeof(Elf64_Rela); break;
      case DT_JMPREL: lib->jmprel = reinterpret_cast<const Elf64_Rela*>(p); break;
      case DT_PLTRELSZ: pltrel_size = v; break;
      case DT_RELR: lib->relr = reinterpret_cast<const Elf64_Relr*>(p); break;
      case DT_RELRSZ: lib->relr_count = v / sizeof(Elf64_Relr); break;
      case DT_INIT: lib->init_func = reinterpret_cast<void (SYSV*)()>(p); break;
      case DT_FINI: lib->fini_func = reinterpret_cast<void (SYSV*)()>(p); break;
      case DT_INIT_ARRAY: lib->init_array = reinterpret_cast<void (SYSV**)()>(p); break;
      case DT_INIT_ARRAYSZ: lib->init_array_count = v / sizeof(void*); break;
      case DT_FINI_ARRAY: lib->fini_array = reinterpret_cast<void (SYSV**)()>(p); break;
      case DT_FINI_ARRAYSZ: lib->fini_array_count = v / sizeof(void*); break;
      default: break;
    }
  }
  lib->jmprel_count = pltrel_size / sizeof(Elf64_Rela);

  // SONAME and NEEDED now that strtab is set.
  for (const Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; ++d) {
    if (d->d_tag == DT_SONAME && lib->strtab) {
      lib->soname = lib->strtab + d->d_un.d_val;
    } else if (d->d_tag == DT_NEEDED && lib->strtab) {
      lib->needed.emplace_back(lib->strtab + d->d_un.d_val);
    }
  }
  if (lib->soname.empty()) lib->soname = lib->name;
}

// Applies one RELA relocation. Returns false and sets *error if a needed symbol
// is missing.
bool ApplyRela(Library* lib, const Elf64_Rela& r, std::string* error) {
  uint32_t type = ELF64_R_TYPE(r.r_info);
  uint32_t sym_index = ELF64_R_SYM(r.r_info);
  uint64_t* where = reinterpret_cast<uint64_t*>(lib->load_bias + r.r_offset);

  auto resolve = [&](ResolvedSymbol* rs) -> bool {
    const Elf64_Sym* sym = &lib->symtab[sym_index];
    const char* name = lib->strtab + sym->st_name;
    *rs = ResolveForReloc(lib, name);
    if (!rs->found) {
      // A weak undefined symbol resolves to 0.
      if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) {
        rs->address = nullptr;
        rs->found = true;
        return true;
      }
      // An unresolved function: a stub that reports the call, so imports the
      // object never calls do not stop it loading.
      if (lib->stub_unresolved &&
          (type == R_X86_64_JUMP_SLOT || ELF64_ST_TYPE(sym->st_info) == STT_FUNC)) {
        rs->address = MakeUnresolvedStub(name);
        rs->found = rs->address != nullptr;
        if (rs->found) {
          std::fprintf(stderr, "gesher elf: %s: '%s' is not implemented (stubbed)\n",
                       lib->name.c_str(), name);
          return true;
        }
      }
      // An unresolved data object: an inaccessible placeholder (above).
      if (lib->stub_unresolved && (type == R_X86_64_GLOB_DAT || type == R_X86_64_64)) {
        rs->address = MakeUnresolvedData(name);
        rs->found = rs->address != nullptr;
        if (rs->found) {
          std::fprintf(stderr,
                       "gesher elf: %s: data '%s' is not provided (a placeholder that faults "
                       "if used)\n",
                       lib->name.c_str(), name);
          return true;
        }
      }
      Fail(error, lib->name + ": unresolved symbol '" + name + "'");
      return false;
    }
    return true;
  };

  switch (type) {
    case R_X86_64_RELATIVE:
    case R_X86_64_RELATIVE64:
      *where = reinterpret_cast<uint64_t>(lib->load_bias) + r.r_addend;
      break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: {
      ResolvedSymbol rs;
      if (!resolve(&rs)) return false;
      *where = reinterpret_cast<uint64_t>(rs.address) + r.r_addend;
      break;
    }
    case R_X86_64_64: {
      ResolvedSymbol rs;
      if (!resolve(&rs)) return false;
      *where = reinterpret_cast<uint64_t>(rs.address) + r.r_addend;
      break;
    }
    case R_X86_64_IRELATIVE: {
      void* addr = ResolveIfunc(lib->load_bias + r.r_addend);
      *where = reinterpret_cast<uint64_t>(addr);
      break;
    }
    case R_X86_64_DTPMOD64: {
      // Dynamic TLS: module id of the defining object.
      if (sym_index == 0) {
        *where = lib->tls.module_id;
      } else {
        ResolvedSymbol rs;
        if (!resolve(&rs)) return false;
        *where = rs.lib ? const_cast<Library*>(rs.lib)->tls.module_id : lib->tls.module_id;
      }
      break;
    }
    case R_X86_64_DTPOFF64: {
      const Elf64_Sym* sym = &lib->symtab[sym_index];
      *where = (sym_index ? sym->st_value : 0) + r.r_addend;
      break;
    }
    case R_X86_64_TPOFF64: {
      // Initial-exec: offset from the thread pointer (negative, variant II).
      const Library* def = lib;
      uint64_t sym_value = 0;
      if (sym_index != 0) {
        ResolvedSymbol rs;
        if (!resolve(&rs)) return false;
        if (rs.lib) def = rs.lib;
        sym_value = rs.sym ? rs.sym->st_value : 0;
      }
      if (!def->tls.is_static) {
        Fail(error, lib->name + ": initial-exec TLS of " + def->name +
                        ", whose TLS did not fit the static TLS reserve");
        return false;
      }
      int64_t tp_off = TlsTpOffset(def->tls) + static_cast<int64_t>(sym_value) + r.r_addend;
      *where = static_cast<uint64_t>(tp_off);
      break;
    }
    default:
      Fail(error, lib->name + ": unsupported relocation type " + std::to_string(type));
      return false;
  }
  return true;
}

// Applies one RELA relocation for a guest aarch64 object. A function symbol
// that resolves to a host (shim) address is bound to a guest trampoline so the
// guest's call crosses the ABI boundary (ADR 0047); data and guest-internal
// symbols keep their real (shared-address-space) address.
bool ApplyRelaAarch64(Library* lib, const Elf64_Rela& r, std::string* error) {
  uint32_t type = ELF64_R_TYPE(r.r_info);
  uint32_t sym_index = ELF64_R_SYM(r.r_info);
  uint64_t* where = reinterpret_cast<uint64_t*>(lib->load_bias + r.r_offset);

  auto resolve_value = [&](bool as_function, uint64_t* out) -> bool {
    const Elf64_Sym* sym = &lib->symtab[sym_index];
    const char* name = lib->strtab + sym->st_name;
    ResolvedSymbol rs = ResolveForReloc(lib, name);
    if (!rs.found) {
      if (ELF64_ST_BIND(sym->st_info) == STB_WEAK) { *out = 0; return true; }
      if (lib->stub_unresolved) {
        // An unresolved import: a host stub that reports the call, bound to a
        // guest trampoline so the guest can call it.
        void* stub = MakeUnresolvedStub(name);
        *out = reinterpret_cast<uint64_t>(gesher::arm64::BindHostFunction(stub));
        std::fprintf(stderr, "gesher elf: %s: '%s' is not implemented (stubbed)\n",
                     lib->name.c_str(), name);
        return stub != nullptr;
      }
      Fail(error, lib->name + ": unresolved symbol '" + name + "'");
      return false;
    }
    bool is_func = as_function || ELF64_ST_TYPE(sym->st_info) == STT_FUNC ||
                   ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC;
    if (is_func && rs.lib == nullptr) {
      // A host function: bind a guest trampoline the guest calls (SVC bridge).
      *out = reinterpret_cast<uint64_t>(gesher::arm64::BindHostFunction(rs.address));
    } else {
      // A guest function, or any data object (shared address space).
      *out = reinterpret_cast<uint64_t>(rs.address);
    }
    return true;
  };

  switch (type) {
    case R_AARCH64_RELATIVE:
      *where = reinterpret_cast<uint64_t>(lib->load_bias) + r.r_addend;
      break;
    case R_AARCH64_JUMP_SLOT: {
      uint64_t v;
      if (!resolve_value(true, &v)) return false;
      *where = v;
      break;
    }
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_ABS64: {
      uint64_t v;
      if (!resolve_value(false, &v)) return false;
      *where = v + r.r_addend;
      break;
    }
    case R_AARCH64_IRELATIVE: {
      // The resolver is guest code; it returns the resolved guest address.
      uint64_t resolved = gesher::arm64::Call(lib->load_bias + r.r_addend, nullptr, 0);
      *where = resolved;
      break;
    }
    case R_AARCH64_TLS_TPREL64: {
      // Local/initial-exec: offset from the thread pointer (TPIDR_EL0).
      if (!lib->tls.is_static) {
        Fail(error, lib->name + ": TLS_TPREL against a module that did not fit the static area");
        return false;
      }
      uint64_t sym_value = 0;
      if (sym_index != 0) sym_value = lib->symtab[sym_index].st_value;
      *where = static_cast<uint64_t>(lib->tls.tp_offset) + sym_value + r.r_addend;
      break;
    }
    default:
      Fail(error, lib->name + ": unsupported aarch64 relocation type " + std::to_string(type));
      return false;
  }
  return true;
}

bool ApplyRelr(Library* lib) {
  // DT_RELR: a compact encoding of R_X86_64_RELATIVE relocations.
  uint64_t* where = nullptr;
  for (size_t i = 0; i < lib->relr_count; ++i) {
    Elf64_Relr entry = lib->relr[i];
    if ((entry & 1) == 0) {
      where = reinterpret_cast<uint64_t*>(lib->load_bias + entry);
      *where += reinterpret_cast<uint64_t>(lib->load_bias);
      ++where;
    } else {
      // A bitmap of the next 63 words.
      uint64_t bits = entry >> 1;
      for (uint64_t* w = where; bits != 0; bits >>= 1, ++w) {
        if (bits & 1) *w += reinterpret_cast<uint64_t>(lib->load_bias);
      }
      where += 63;
    }
  }
  return true;
}

bool Relocate(Library* lib, std::string* error) {
  if (lib->relr) ApplyRelr(lib);  // DT_RELR is arch-neutral (load-bias add)
  if (lib->is_aarch64) {
    for (size_t i = 0; i < lib->rela_count; ++i) {
      if (!ApplyRelaAarch64(lib, lib->rela[i], error)) return false;
    }
    for (size_t i = 0; i < lib->jmprel_count; ++i) {
      if (!ApplyRelaAarch64(lib, lib->jmprel[i], error)) return false;
    }
    return true;
  }
  for (size_t i = 0; i < lib->rela_count; ++i) {
    if (!ApplyRela(lib, lib->rela[i], error)) return false;
  }
  for (size_t i = 0; i < lib->jmprel_count; ++i) {
    if (!ApplyRela(lib, lib->jmprel[i], error)) return false;
  }
  return true;
}

void RunInit(Library* lib) {
  if (lib->init_done) return;
  lib->init_done = true;
  if (lib->is_aarch64) {
    // Init functions are guest code: run them through the translator.
    if (lib->init_func) gesher::arm64::Call(reinterpret_cast<void*>(lib->init_func), nullptr, 0);
    for (size_t i = 0; i < lib->init_array_count; ++i) {
      auto f = lib->init_array[i];
      if (f && f != reinterpret_cast<void (SYSV*)()>(-1)) {
        gesher::arm64::Call(reinterpret_cast<void*>(f), nullptr, 0);
      }
    }
    return;
  }
  if (lib->init_func) lib->init_func();
  for (size_t i = 0; i < lib->init_array_count; ++i) {
    if (lib->init_array[i] && lib->init_array[i] != reinterpret_cast<void (SYSV*)()>(-1)) {
      lib->init_array[i]();
    }
  }
}

// Resolves DT_NEEDED: loads a dependency from the search paths, or treats it as
// shimmed. Returns false on a hard error.
bool ResolveNeeded(Library* lib, const LoadOptions& options, std::string* error);

Library* LoadCommon(const char* name, const uint8_t* data, size_t size, std::string* error,
                    const LoadOptions& options) {
  if (size < sizeof(Elf64_Ehdr)) {
    Fail(error, std::string(name) + ": too small to be ELF");
    return nullptr;
  }
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_ident[EI_DATA] != ELFDATA2LSB ||
      (ehdr->e_machine != EM_X86_64 && ehdr->e_machine != EM_AARCH64) ||
      ehdr->e_type != ET_DYN) {
    Fail(error, std::string(name) + ": not an ELF64 little-endian x86-64 or aarch64 shared object");
    return nullptr;
  }
  const bool aarch64 = ehdr->e_machine == EM_AARCH64;
  if (aarch64 && !gesher::arm64::EnsureLoaded()) {
    Fail(error, std::string(name) + ": an arm64-v8a object, but the arm64 translator (" +
                    "gesher_arm64.dll) is not available");
    return nullptr;
  }

  auto* lib = new Library();
  lib->name = name;
  lib->is_aarch64 = aarch64;
  lib->stub_unresolved = options.stub_unresolved_functions;
  if (!MapSegments(lib, data, error)) {
    delete lib;
    return nullptr;
  }
  ParseDynamic(lib, data);

  if (!aarch64) {
    // x86-64 only: %fs accesses become %gs ones before any of the object's code
    // runs (IFUNC resolvers run during relocation). Guest arm64 code reaches
    // TLS through TPIDR_EL0, which the translator supplies (ADR 0047).
    if (!RewriteFsAccesses(lib->name.c_str(), data, size, lib->load_bias, &lib->fs, error)) {
      delete lib;
      return nullptr;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lib->tls.present) {
      if (aarch64) {
        lib->tls.tp_offset = gesher::arm64::TlsRegister(lib->tls.total_size, lib->tls.align);
        lib->tls.is_static = lib->tls.tp_offset >= 0;
        lib->tls.module_id = 0;  // dynamic TLS via the translator, not compat/elf
      } else {
        TlsAssignModule(&lib->tls);
      }
    }
    Loaded().push_back(lib);
    ++g_load_count;
  }

  // Register the guest executable segments with the translator before any guest
  // code runs (init arrays, IFUNC resolvers).
  if (aarch64) RegisterGuestCode(lib, data);

  if (!ResolveNeeded(lib, options, error)) {
    return nullptr;
  }
  if (!Relocate(lib, error)) {
    return nullptr;
  }
  if (lib->tls.present) {
    if (aarch64) {
      if (lib->tls.is_static) {
        gesher::arm64::TlsPublish(lib->tls.tp_offset, lib->tls.init_image, lib->tls.init_size,
                                  lib->tls.total_size);
      }
    } else {
      // The relocated TLS image goes into threads that already have a block.
      TlsPublishModule(&lib->tls);
    }
  }
  if (!ProtectSegments(lib, data, error)) {
    return nullptr;
  }
  RunInit(lib);
  return lib;
}

bool ResolveNeeded(Library* lib, const LoadOptions& options, std::string* error) {
  for (const std::string& soname : lib->needed) {
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (Library* existing = FindLocked(soname)) {
        lib->dependencies.push_back(existing);
        continue;
      }
    }
    // Try to load it from a search path.
    std::string found;
    for (const std::string& dir : options.search_paths) {
      std::string candidate = dir + "/" + soname;
      struct stat st {};
      if (stat(candidate.c_str(), &st) == 0) {
        found = candidate;
        break;
      }
    }
    if (!found.empty()) {
      std::vector<uint8_t> buf;
      if (!ReadFile(found.c_str(), &buf, error)) return false;
      Library* dep = LoadCommon(soname.c_str(), buf.data(), buf.size(), error, options);
      if (dep == nullptr) return false;
      lib->dependencies.push_back(dep);
      continue;
    }
    // Otherwise the bionic shim provides its symbols (libc.so, libm.so, ...).
    if (!options.allow_shimmed_needed) {
      Fail(error, lib->name + ": cannot find needed library " + soname);
      return false;
    }
    // Shimmed: its symbols come through g_resolver during relocation.
  }
  return true;
}

}  // namespace

void SetUnresolvedHandler(UnresolvedHandler handler) { g_unresolved_handler = handler; }

int ForEachLoaded(const std::function<int(const PhdrInfo&)>& fn) {
  std::vector<PhdrInfo> infos;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Library* lib : Loaded()) {
      if (lib->is_closed || lib->phdr == nullptr) continue;
      PhdrInfo info;
      info.addr = reinterpret_cast<uintptr_t>(lib->load_bias);
      info.name = lib->soname.c_str();
      info.phdr = lib->phdr;
      info.phnum = lib->phnum;
      info.tls_modid = lib->tls.present ? lib->tls.module_id : 0;
      info.adds = g_load_count;
      infos.push_back(info);
    }
  }
  // Called without the lock: the callback may itself look symbols up.
  for (const PhdrInfo& info : infos) {
    if (int r = fn(info)) return r;
  }
  return 0;
}

bool FindAddress(const void* addr, AddressInfo* info) {
  auto a = reinterpret_cast<const uint8_t*>(addr);
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Library* lib : Loaded()) {
    if (a < lib->base || a >= lib->base + lib->map_size) continue;
    info->soname = lib->soname.c_str();
    info->base = lib->base;
    info->symbol = nullptr;
    info->symbol_addr = nullptr;
    // The symbol count: SysV hash's nchain, else scan the GNU hash chains.
    size_t count = lib->has_sysv_hash ? lib->sysv_nchain : 0;
    if (count == 0 && lib->has_gnu_hash) {
      uint32_t last = 0;
      for (uint32_t b = 0; b < lib->gnu_nbucket; ++b) last = std::max(last, lib->gnu_buckets[b]);
      if (last >= lib->gnu_symndx) {
        while ((lib->gnu_chain[last - lib->gnu_symndx] & 1) == 0) ++last;
      }
      count = last + 1;
    }
    const uint8_t* best = nullptr;
    for (size_t i = 1; i < count; ++i) {
      const Elf64_Sym* sym = &lib->symtab[i];
      if (sym->st_shndx == SHN_UNDEF || sym->st_value == 0) continue;
      int t = ELF64_ST_TYPE(sym->st_info);
      if (t != STT_FUNC && t != STT_OBJECT && t != STT_GNU_IFUNC) continue;
      const uint8_t* s = lib->load_bias + sym->st_value;
      if (s <= a && (a < s + std::max<uint64_t>(sym->st_size, 1) || sym->st_size == 0) &&
          (best == nullptr || s > best)) {
        best = s;
        info->symbol = lib->strtab + sym->st_name;
        info->symbol_addr = const_cast<uint8_t*>(s);
      }
    }
    return true;
  }
  return false;
}

void SetBionicResolver(SymbolResolver resolver) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_resolver = std::move(resolver);
}

Library* Load(const char* path, std::string* error, const LoadOptions& options) {
  std::vector<uint8_t> buf;
  if (!ReadFile(path, &buf, error)) return nullptr;
  // Name the object by its file base name.
  const char* slash = std::strrchr(path, '/');
  const char* bslash = std::strrchr(path, '\\');
  const char* base = slash > bslash ? slash : bslash;
  std::string name = base ? base + 1 : path;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Library* existing = FindLocked(name)) return existing;
  }
  return LoadCommon(name.c_str(), buf.data(), buf.size(), error, options);
}

Library* LoadFromMemory(const char* name, const void* data, size_t size, std::string* error,
                        const LoadOptions& options) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Library* existing = FindLocked(name)) return existing;
  }
  return LoadCommon(name, static_cast<const uint8_t*>(data), size, error, options);
}

void* Lookup(Library* library, const char* name) {
  if (library == nullptr) return nullptr;
  uint32_t gh = GnuHash(name), sh = SysvHash(name);
  const Elf64_Sym* sym = FindDefinedSymbol(library, name, gh, sh);
  if (sym != nullptr) return SymbolAddress(library, sym);
  // Fall back to the dependency graph.
  for (Library* dep : library->dependencies) {
    const Elf64_Sym* s = FindDefinedSymbol(dep, name, gh, sh);
    if (s != nullptr) return SymbolAddress(dep, s);
  }
  return nullptr;
}

void* LookupGlobal(const char* name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return LookupGlobalLocked(name, nullptr, nullptr);
}

Library* Find(const char* name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return FindLocked(name);
}

const char* Soname(Library* library) { return library ? library->soname.c_str() : ""; }

void Close(Library* library) {
  if (library == nullptr || library->is_closed) return;
  for (size_t i = library->fini_array_count; i-- > 0;) {
    if (library->fini_array[i]) library->fini_array[i]();
  }
  if (library->fini_func) library->fini_func();
  library->is_closed = true;
}

bool SetupThreadTls() { return TlsSetupThread(); }

FsRewriteCounts FsRewrites(Library* library) {
  FsRewriteCounts c;
  if (library == nullptr) return c;
  c.stack_guard = library->fs.stack_guard;
  c.thread_pointer = library->fs.thread_ptr;
  c.stubbed = library->fs.stubbed;
  c.trapped = library->fs.trapped;
  c.decode_errors = library->fs.decode_errors;
  c.bytes_decoded = library->fs.bytes_decoded;
  c.used_unwind_info = library->fs.used_fdes;
  return c;
}

}  // namespace gesher::elf
