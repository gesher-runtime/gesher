// gesher-base: <link.h>. Structures follow musl/glibc. dl_iterate_phdr
// reports nothing until Gesher's ELF loader exists (Windows DLLs have no
// program headers).
#pragma once

#include <elf.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ElfW(type) Elf64_##type

struct dl_phdr_info {
  ElfW(Addr) dlpi_addr;
  const char* dlpi_name;
  const ElfW(Phdr) * dlpi_phdr;
  ElfW(Half) dlpi_phnum;
  unsigned long long int dlpi_adds;
  unsigned long long int dlpi_subs;
  size_t dlpi_tls_modid;
  void* dlpi_tls_data;
};

struct link_map {
  ElfW(Addr) l_addr;
  char* l_name;
  ElfW(Dyn) * l_ld;
  struct link_map *l_next, *l_prev;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data);

#ifdef __cplusplus
}
#endif
