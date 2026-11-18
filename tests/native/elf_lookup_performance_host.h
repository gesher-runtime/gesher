// The Library descriptor, namespace eligibility and IFUNC boundary are adapted.
// Hashing and symbol-table probing are extracted unchanged from elf_loader.cc.
#pragma once
#include <algorithm>
#include <mutex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "elf_types.h"

inline std::recursive_mutex g_mutex;

enum class LookupStatus { kNotFound, kResolved, kUnavailable };
struct SymbolVersion { uint32_t hash; const char* name; const char* provider; };
struct Namespace {};
struct LoaderContext {};
enum class Architecture { kAuto };
enum class LookupKind { kHandle };
struct FixtureProvider { void* (*lookup)(const char*); };
struct Library {
  std::vector<Library*> dependencies;
  Namespace* name_space = nullptr;
  const std::vector<Library*>* load_scope = nullptr;
  FixtureProvider* provider = nullptr;
  bool has_gnu_hash = false, has_sysv_hash = false;
  uint32_t gnu_maskwords = 1, gnu_shift2 = 5, gnu_nbucket = 1, gnu_symndx = 1, sysv_nbucket = 1;
  const uint64_t* gnu_bloom = nullptr;
  const uint32_t* gnu_buckets = nullptr;
  const uint32_t* gnu_chain = nullptr;
  const uint32_t* sysv_buckets = nullptr;
  const uint32_t* sysv_chain = nullptr;
  const Elf64_Sym* symtab = nullptr;
  const char* strtab = nullptr;
  const Elf64_Half* versym = nullptr;
  size_t versym_capacity = 0;
  std::vector<std::pair<Elf64_Half, SymbolVersion>> version_definitions;
  std::vector<Elf64_Sym> symbols = std::vector<Elf64_Sym>(1);
  std::string strings = std::string(1, '\0');
  std::vector<uint32_t> gnu_chains, sysv_chains;
  uint64_t bloom = ~uint64_t{0};
  uint32_t bucket = 0;
  void* (*resolver)() = nullptr;
};

// Every descriptor in this graph/resource gate is an available ELF of the
// same architecture in one eligible namespace. Providers are not modeled.
// Complete production translation-unit gates exercise real namespace policy.
inline bool Available(Library* library) { return library != nullptr; }
inline bool MatchesArchitecture(Library*, Architecture) { return true; }
inline bool Accessible(Namespace*, Library*, const std::vector<Library*>*) { return true; }
inline bool RefreshProviderGraph(Library*, Architecture, std::string*) { std::abort(); }

uint32_t GnuHash(const char* name);
uint32_t SysvHash(const char* name);
const Elf64_Sym* FindDefinedSymbol(const Library* library, const char* name, uint32_t gh, uint32_t sh
#if HAS_VERSIONS
                                  , const SymbolVersion* version = nullptr
#endif
                                  );
void* SymbolAddress(const Library* library, const Elf64_Sym* symbol);
inline void* LookupSymbolAddress(const Library* library, const Elf64_Sym* symbol) {
  return SymbolAddress(library, symbol);
}
void* Lookup(Library* library, const char* name);
#if HAS_REFERENCE
void* ReferenceLookup(Library* library, const char* name);
#endif
