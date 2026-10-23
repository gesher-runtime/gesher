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
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "tls.h"

// DT_RELR's entry type; this elf.h predates it.
using Elf64_Relr = uint64_t;

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

// The address of a defined symbol, resolving IFUNCs.
void* SymbolAddress(const Library* lib, const Elf64_Sym* sym) {
  void* addr = lib->load_bias + sym->st_value;
  if (ELF64_ST_TYPE(sym->st_info) == STT_GNU_IFUNC) addr = ResolveIfunc(addr);
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

// After relocation, set each segment's final protection.
bool ProtectSegments(Library* lib, const uint8_t* data, std::string* error) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
  for (int i = 0; i < ehdr->e_phnum; ++i) {
    if (phdr[i].p_type != PT_LOAD) continue;
    uint8_t* start = lib->load_bias + PageDown(phdr[i].p_vaddr);
    size_t len = PageUp(phdr[i].p_vaddr + phdr[i].p_memsz) - PageDown(phdr[i].p_vaddr);
    if (mprotect(start, len, ProtFor(phdr[i].p_flags)) != 0) {
      Fail(error, "mprotect of a segment failed");
      return false;
    }
  }
  return true;
}

// Parses PT_DYNAMIC and PT_TLS.
void ParseDynamic(Library* lib, const uint8_t* data) {
  const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(data);
  const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(data + ehdr->e_phoff);
  const Elf64_Dyn* dyn = nullptr;

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
  if (lib->relr) ApplyRelr(lib);
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
      ehdr->e_machine != EM_X86_64 || ehdr->e_type != ET_DYN) {
    Fail(error, std::string(name) + ": not an ELF64 little-endian x86-64 shared object");
    return nullptr;
  }

  auto* lib = new Library();
  lib->name = name;
  if (!MapSegments(lib, data, error)) {
    delete lib;
    return nullptr;
  }
  ParseDynamic(lib, data);

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (lib->tls.present) TlsAssignModule(&lib->tls);
    Loaded().push_back(lib);
  }

  if (!ResolveNeeded(lib, options, error)) {
    return nullptr;
  }
  if (!Relocate(lib, error)) {
    return nullptr;
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

}  // namespace gesher::elf
