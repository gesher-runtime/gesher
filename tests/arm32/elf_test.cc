// gesher: production ELF32 loader contracts, including hostile input and rollback.
#include <gesher/arm32_elf.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef GESHER_ELF_NO_ALLOCATION_INJECTION
namespace {
long allocations_before_failure = -1;
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void* operator new(std::size_t size) {
  if (allocations_before_failure == 0) throw std::bad_alloc();
  if (allocations_before_failure > 0) --allocations_before_failure;
  if (void* allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
  throw std::bad_alloc();
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* allocation) noexcept { std::free(allocation); }
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
#endif

namespace {
using gesher::arm32::ElfImage;
using gesher::arm32::ElfLoadOptions;
using gesher::arm32::LoadElf32;

[[noreturn]] void Die(const char* expression, int line) {
  std::fprintf(stderr, "ELF32 contract failed at %d: %s\n", line, expression);
  std::exit(1);
}
#define CHECK(expression) do { if (!(expression)) Die(#expression, __LINE__); } while (0)

void W16(std::vector<uint8_t>& f, size_t at, uint16_t value) {
  f.at(at) = static_cast<uint8_t>(value);
  f.at(at + 1) = static_cast<uint8_t>(value >> 8);
}
void W32(std::vector<uint8_t>& f, size_t at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) f.at(at + i) = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t R32(const std::vector<uint8_t>& f, size_t at) {
  return uint32_t{f.at(at)} | (uint32_t{f.at(at + 1)} << 8) |
         (uint32_t{f.at(at + 2)} << 16) | (uint32_t{f.at(at + 3)} << 24);
}

// A section-header-independent DSO with two PT_LOAD segments, real ELF32
// layouts, a writable BSS, Thumb exports, host/weak imports and ten RELs.
// Test code never executes the fixture or substitutes a runtime implementation.
struct Fixture {
  std::vector<uint8_t> file = std::vector<uint8_t>(0x800);
  size_t next_tag = 0x100;

  void Tag(uint32_t tag, uint32_t value) {
    for (size_t at = 0x100; at < next_tag; at += 8) {
      if (R32(file, at) == tag) { W32(file, at + 4, value); return; }
    }
    CHECK(next_tag + 8 < 0x200);
    W32(file, next_tag, tag);
    W32(file, next_tag + 4, value);
    next_tag += 8;
  }
  void RemoveTag(uint32_t tag) {
    for (size_t at = 0x100; at < next_tag; at += 8) {
      if (R32(file, at) != tag) continue;
      for (size_t next = at; next < next_tag; next += 8) {
        W32(file, next, R32(file, next + 8));
        W32(file, next + 4, R32(file, next + 12));
      }
      next_tag -= 8;
      return;
    }
    Die("tag found for removal", __LINE__);
  }
  void Phdr(size_t at, uint32_t type, uint32_t offset, uint32_t vaddr,
            uint32_t filesz, uint32_t memsz, uint32_t flags, uint32_t align) {
    W32(file, at, type); W32(file, at + 4, offset); W32(file, at + 8, vaddr);
    W32(file, at + 16, filesz); W32(file, at + 20, memsz);
    W32(file, at + 24, flags); W32(file, at + 28, align);
  }
  void Symbol(size_t index, uint32_t name, uint32_t value, uint32_t size,
              uint8_t info, uint16_t section) {
    const size_t at = 0x340 + index * 16;
    W32(file, at, name); W32(file, at + 4, value); W32(file, at + 8, size);
    file[at + 12] = info; W16(file, at + 14, section);
  }
  void Rel(size_t index, uint32_t target, uint32_t symbol, uint32_t type) {
    W32(file, 0x400 + index * 8, target);
    W32(file, 0x404 + index * 8, symbol * 256 + type);
  }
  Fixture() {
    std::memcpy(file.data(), "\177ELF", 4);
    file[4] = 1; file[5] = 1; file[6] = 1;
    W16(file, 16, 3); W16(file, 18, 40); W32(file, 20, 1);
    W32(file, 24, 0x201); W32(file, 28, 52); W32(file, 36, 0x05000200);
    W16(file, 40, 52); W16(file, 42, 32); W16(file, 44, 3);
    Phdr(52, 1, 0, 0, 0x500, 0x500, 5, 0x100);
    Phdr(84, 1, 0x600, 0x600, 0x200, 0x280, 6, 0x100);
    Phdr(116, 2, 0x100, 0x100, 0x100, 0x100, 4, 4);
    // movs r0,#42; bx lr; and movs r0,#7; bx lr.
    W32(file, 0x200, 0x4770202a); W32(file, 0x204, 0x47702007);
    static constexpr char strings[] = "\0answer\0host_add\0weak\0thumb\0value\0fixture.so\0";
    std::memcpy(file.data() + 0x300, strings, sizeof(strings));
    Symbol(1, 1, 0x201, 4, 0x12, 1);
    Symbol(2, 8, 0, 0, 0x12, 0);
    Symbol(3, 17, 0, 0, 0x20, 0);
    Symbol(4, 22, 0x205, 4, 0x12, 1);
    Symbol(5, 28, 0x640, 4, 0x11, 2);
    W32(file, 0x3a0, 1); W32(file, 0x3a4, 6); W32(file, 0x3a8, 1);
    for (size_t i = 1; i < 5; ++i) W32(file, 0x3ac + i * 4, static_cast<uint32_t>(i + 1));
    Rel(0, 0x600, 0, 23); W32(file, 0x600, 0x640);
    Rel(1, 0x604, 1, 2);
    Rel(2, 0x608, 4, 3); W32(file, 0x608, 4);
    Rel(3, 0x60c, 2, 21); W32(file, 0x60c, 0xdeadbeef);
    Rel(4, 0x610, 2, 22); W32(file, 0x610, 0xdeadbeef);
    Rel(5, 0x614, 3, 2);
    Rel(6, 0x618, 3, 3); W32(file, 0x618, 8);
    Rel(7, 0x61c, 5, 2);
    Rel(8, 0x620, 0, 23); W32(file, 0x620, 0x201);
    Rel(9, 0x624, 0, 23); W32(file, 0x624, 0x205);
    W32(file, 0x640, 42);
    Tag(4, 0x3a0); Tag(5, 0x300); Tag(6, 0x340); Tag(10, sizeof(strings)); Tag(11, 16);
    Tag(14, 34); Tag(17, 0x400); Tag(18, 80); Tag(19, 8);
    Tag(12, 0x201); Tag(13, 0x205); Tag(25, 0x620); Tag(27, 4);
    Tag(26, 0x624); Tag(28, 4);
  }
  void GnuHash() {
    RemoveTag(4);
    Tag(0x6ffffef5, 0x460);
    W32(file, 0x460, 1); W32(file, 0x464, 1); W32(file, 0x468, 1); W32(file, 0x46c, 5);
    W32(file, 0x470, 0xffffffff); W32(file, 0x474, 1);
    for (size_t i = 0; i < 5; ++i) W32(file, 0x478 + i * 4, i == 4 ? 1 : 2);
  }
};

// An APS2 stream using real Android SLEB128 grouping (ELF32 REL records).
// All encoded relocations target previously unrelocated writable words.
void PackedAps2(Fixture& f, bool grouped) {
  std::vector<uint8_t> encoded{'A', 'P', 'S', '2'};
  // total count = 3; previous offset = 0x624 (signed LEB128).
  encoded.insert(encoded.end(), {3, 0xa4, 0x0c, 3});
  if (grouped) {
    // grouped by offset delta and r_info; relative relocation at each step.
    encoded.insert(encoded.end(), {3, 4, 23});
  } else {
    // Three individual (delta, r_info) pairs.
    encoded.insert(encoded.end(), {0, 4, 23, 4, 23, 4, 23});
  }
  CHECK(0x4a0 + encoded.size() < 0x500);
  std::copy(encoded.begin(), encoded.end(), f.file.begin() + 0x4a0);
  for (uint32_t at : {0x628u, 0x62cu, 0x630u}) W32(f.file, at, at - 0x628 + 0x640);
  f.Tag(0x6000000f, 0x4a0);
  f.Tag(0x60000010, static_cast<uint32_t>(encoded.size()));
}

ElfLoadOptions Options(int* imports = nullptr) {
  ElfLoadOptions options;
  options.resolve_import = [imports](std::string_view name, uint32_t* address) {
    if (imports != nullptr) ++*imports;
    if (name == "host_add") { *address = 0x70001; return true; }
    return false;
  };
  return options;
}

void Rejected(Fixture fixture, std::string_view message,
              const ElfLoadOptions& options = Options()) {
  ElfImage image;
  image.guest_base = 123;
  image.soname = "unchanged";
  image.bytes = {1, 9, 3};
  image.constructors = {456};
  const auto before = fixture.file;
  std::string error;
  CHECK(!LoadElf32(fixture.file.data(), fixture.file.size(), options, &image, &error));
  if (error.find(message) == std::string::npos) {
    std::fprintf(stderr, "Expected error containing '%.*s', got '%s'\n",
                 static_cast<int>(message.size()), message.data(), error.c_str());
    std::exit(1);
  }
  CHECK(image.guest_base == 123 && image.soname == "unchanged");
  CHECK((image.bytes == std::vector<uint8_t>{1, 9, 3}));
  CHECK((image.constructors == std::vector<uint32_t>{456}));
  CHECK(fixture.file == before);
}

void ValidImages() {
  for (bool gnu : {false, true}) {
    Fixture f;
    if (gnu) f.GnuHash();
    const auto source = f.file;
    ElfImage image;
    std::string error = "old";
    int imports = 0;
    CHECK(LoadElf32(f.file.data(), f.file.size(), Options(&imports), &image, &error));
    CHECK(error.empty() && f.file == source);
    CHECK(image.guest_base == 0x10000 && image.load_bias == 0x10000 && image.entry == 0x10201);
    CHECK(image.soname == "fixture.so");
    CHECK(image.bytes.size() == 0x880 && image.segments.size() == 2);
    CHECK(image.segments[0].address == 0x10000 && image.segments[0].size == 0x500);
    CHECK(image.segments[1].address == 0x10600 && image.segments[1].flags == 6);
    CHECK(std::all_of(image.bytes.begin() + 0x800, image.bytes.end(), [](uint8_t b) { return b == 0; }));
    CHECK(R32(image.bytes, 0x600) == 0x10640);
    CHECK(R32(image.bytes, 0x604) == 0x10201);
    CHECK(R32(image.bytes, 0x608) == uint32_t(0x10209 - 0x10608));
    CHECK(R32(image.bytes, 0x60c) == 0x70001 && R32(image.bytes, 0x610) == 0x70001);
    CHECK(R32(image.bytes, 0x614) == 0 && R32(image.bytes, 0x618) == 8);
    CHECK(R32(image.bytes, 0x61c) == 0x10640);
    CHECK((image.constructors == std::vector<uint32_t>{0x10201, 0x10201}));
    CHECK((image.finalizers == std::vector<uint32_t>{0x10205, 0x10205}));
    CHECK(imports == 2);  // host_add once, weak once, independent of REL count.
    uint32_t address = 0;
    CHECK(image.Lookup("answer", &address) && address == 0x10201);
    CHECK(image.Lookup("value", &address) && address == 0x10640);
    CHECK(!image.Lookup("host_add", &address) && !image.Lookup("missing", &address));
  }
  Fixture f;
  f.Tag(18, 72);  // Move the last REL into its own PLT table instead.
  f.Rel(9, 0x610, 2, 22); f.Rel(4, 0x624, 0, 23);
  W32(f.file, 0x624, 0x205);
  f.Tag(23, 0x448); f.Tag(2, 8); f.Tag(20, 17);
  ElfImage image;
  std::string error;
  CHECK(LoadElf32(f.file.data(), f.file.size(), Options(), &image, &error));
  CHECK(R32(image.bytes, 0x610) == 0x70001);

  // A minimal ET_DYN with no dynamic table is also a valid executable image
  // for the isolated executor; it does not pretend to have Android imports.
  Fixture plain;
  W16(plain.file, 44, 2);
  CHECK(LoadElf32(plain.file.data(), plain.file.size(), {}, &image, &error));
  CHECK(image.symbols.empty() && image.constructors.empty());

  // File parsing is independent of host alignment, and load bias is distinct
  // from the first PT_LOAD address (the latter need not be zero).
  Fixture shifted;
  for (size_t phdr : {size_t{52}, size_t{84}, size_t{116}}) {
    W32(shifted.file, phdr + 8, R32(shifted.file, phdr + 8) + 0x1000);
  }
  W32(shifted.file, 24, 0x1201);
  for (uint32_t tag : {4u, 5u, 6u, 12u, 13u, 17u, 25u, 26u}) {
    for (size_t at = 0x100; at < shifted.next_tag; at += 8) {
      if (R32(shifted.file, at) == tag) {
        W32(shifted.file, at + 4, R32(shifted.file, at + 4) + 0x1000);
      }
    }
  }
  for (size_t symbol : {size_t{1}, size_t{4}, size_t{5}}) {
    const size_t value = 0x344 + symbol * 16;
    W32(shifted.file, value, R32(shifted.file, value) + 0x1000);
  }
  for (size_t i = 0; i < 10; ++i) {
    W32(shifted.file, 0x400 + i * 8, R32(shifted.file, 0x400 + i * 8) + 0x1000);
  }
  for (size_t target : {size_t{0x600}, size_t{0x620}, size_t{0x624}}) {
    W32(shifted.file, target, R32(shifted.file, target) + 0x1000);
  }
  std::vector<uint8_t> unaligned(shifted.file.size() + 1);
  std::copy(shifted.file.begin(), shifted.file.end(), unaligned.begin() + 1);
  CHECK(LoadElf32(unaligned.data() + 1, shifted.file.size(), Options(), &image, &error));
  CHECK(image.guest_base == 0x11000 && image.load_bias == 0x10000);
  CHECK(R32(image.bytes, 0x600) == 0x11640 && R32(image.bytes, 0x604) == 0x11201);
  CHECK((image.constructors == std::vector<uint32_t>{0x11201, 0x11201}));

  Fixture protected_image;
  W16(protected_image.file, 44, 4);
  protected_image.Phdr(148, 0x6474e552, 0x600, 0x600, 0x200, 0x280, 4, 1);
  CHECK(LoadElf32(protected_image.file.data(), protected_image.file.size(), Options(), &image, &error));
  CHECK(image.relro.size() == 1 && image.relro[0].address == 0x10600 && image.relro[0].size == 0x280);
  CHECK(R32(image.bytes, 0x600) == 0x10640);  // Relocated before protection is published.

  // Standard and legacy Android RELR share one four-byte bitmap encoding.
  // Three positions have no DT_REL records, so this checks actual expansion.
  for (bool android : {false, true}) {
    Fixture packed;
    const uint32_t base = android ? 0x6fffe000 : 36;
    const uint32_t size = android ? 0x6fffe001 : 35;
    const uint32_t entry = android ? 0x6fffe003 : 37;
    W32(packed.file, 0x628, 0x640);
    W32(packed.file, 0x62c, 0x201);
    W32(packed.file, 0x630, 0x205);
    W32(packed.file, 0x4a0, 0x628);  // direct relocation
    W32(packed.file, 0x4a4, 7);      // 0x62c and 0x630
    packed.Tag(base, 0x4a0);
    packed.Tag(size, 8);
    packed.Tag(entry, 4);
    if (android) packed.Tag(0x6fffe005, 3);  // historical optional hint
    CHECK(LoadElf32(packed.file.data(), packed.file.size(), Options(), &image, &error));
    CHECK(R32(image.bytes, 0x628) == 0x10640);
    CHECK(R32(image.bytes, 0x62c) == 0x10201);
    CHECK(R32(image.bytes, 0x630) == 0x10205);
    CHECK(R32(image.bytes, 0x600) == 0x10640);  // Existing RELs still apply.
  }

  for (bool grouped : {false, true}) {
    Fixture packed;
    PackedAps2(packed, grouped);
    CHECK(LoadElf32(packed.file.data(), packed.file.size(), Options(), &image, &error));
    CHECK(R32(image.bytes, 0x628) == 0x10640);
    CHECK(R32(image.bytes, 0x62c) == 0x10644);
    CHECK(R32(image.bytes, 0x630) == 0x10648);
    CHECK(R32(image.bytes, 0x600) == 0x10640);  // ordinary REL table coexists.
  }

  Fixture linux;
  linux.file[7] = 3;
  CHECK(LoadElf32(linux.file.data(), linux.file.size(), Options(), &image, &error));
}

void HostileImages() {
  auto bad = [](auto modify, std::string_view message) {
    Fixture f; modify(f); Rejected(std::move(f), message);
  };
  bad([](auto& f) { f.file.resize(51); }, "truncated ELF32 header");
  bad([](auto& f) { f.file[4] = 2; }, "ELF32 ARM ET_DYN");
  bad([](auto& f) { f.file[7] = 65; }, "unsupported OSABI");
  bad([](auto& f) { f.file[7] = 255; }, "unsupported OSABI");
  bad([](auto& f) { f.file[8] = 1; }, "ABI version");
  bad([](auto& f) { W16(f.file, 18, 3); }, "ELF32 ARM ET_DYN");
  bad([](auto& f) { W32(f.file, 36, 0x05000400); }, "softfp");
  bad([](auto& f) { W32(f.file, 36, 0x04000200); }, "EABI5");
  bad([](auto& f) { W32(f.file, 28, 0xfffffff0); }, "program-header");
  bad([](auto& f) { W16(f.file, 44, 0xffff); }, "program-header");
  bad([](auto& f) { W32(f.file, 84 + 16, 0x300); }, "program segment");
  bad([](auto& f) { W32(f.file, 84 + 8, 0xfffffe00); }, "program segment");
  bad([](auto& f) { W32(f.file, 84 + 8, 0x400); }, "overlapping PT_LOAD");
  bad([](auto& f) { W32(f.file, 84 + 28, 3); }, "alignment");
  bad([](auto& f) { W32(f.file, 24, 0x601); }, "entry point");
  bad([](auto& f) { W32(f.file, 116, 7); }, "PT_TLS");
  bad([](auto& f) { W32(f.file, 116, 3); }, "PT_INTERP");
  bad([](auto& f) {
    W16(f.file, 44, 4); f.Phdr(148, 0x6474e552, 0, 0x550, 0, 0x100, 4, 1);
  }, "PT_GNU_RELRO lies outside");
  bad([](auto& f) { W32(f.file, 120, 0x108); }, "PT_DYNAMIC does not match");
  bad([](auto& f) { W32(f.file, 116 + 16, 0xff); }, "PT_DYNAMIC range");
  bad([](auto& f) { W32(f.file, 116 + 16, static_cast<uint32_t>(f.next_tag - 0x100)); }, "DT_NULL");
  bad([](auto& f) { W32(f.file, f.next_tag, 4); W32(f.file, f.next_tag + 4, 0x3a0); }, "duplicate dynamic tag");
  bad([](auto& f) { f.Tag(5, 0x550); }, "string-table range");
  bad([](auto& f) { W32(f.file, 0x350, 999); }, "string-table offset");
  bad([](auto& f) { std::fill(f.file.begin() + 0x300, f.file.begin() + 0x340, 'x'); }, "begin and end with NUL");
  bad([](auto& f) { f.Tag(6, 0x7f0); }, "symbol table exceeds");
  bad([](auto& f) { f.Tag(11, 24); }, "entry size");
  bad([](auto& f) { W32(f.file, 0x3a4, 0xffffffff); }, "DT_HASH size");
  bad([](auto& f) { W32(f.file, 0x3a8, 6); }, "DT_HASH symbol index");
  bad([](auto& f) { f.file[0x350 + 12] = 0x1a; }, "STT_GNU_IFUNC");
  bad([](auto& f) { f.file[0x350 + 12] = 0x16; }, "STT_TLS");
  bad([](auto& f) { W32(f.file, 0x354, 0xfffffff1); }, "defined dynamic symbol");
  bad([](auto& f) { W16(f.file, 0x350 + 14, 0xfff2); }, "special symbol section");
  bad([](auto& f) { f.Tag(18, 79); }, "REL table size");
  bad([](auto& f) { f.Tag(17, 0x7f0); }, "REL table exceeds");
  bad([](auto& f) { f.Tag(19, 12); }, "DT_RELENT");
  bad([](auto& f) { f.Rel(1, 0x604, 6, 2); }, "symbol index");
  bad([](auto& f) { f.Rel(0, 0x600, 1, 23); }, "must not reference");
  bad([](auto& f) { f.Rel(1, 0xfffffffc, 1, 2); }, "target is unaligned or outside");
  bad([](auto& f) { f.Rel(1, 0x602, 1, 2); }, "target is unaligned or outside");
  bad([](auto& f) { f.Rel(1, 0x504, 1, 2); }, "target is unaligned or outside");
  bad([](auto& f) { f.Rel(1, 0x204, 1, 2); }, "not in a writable");
  bad([](auto& f) { f.Rel(1, 0x600, 1, 2); }, "duplicate relocation");
  bad([](auto& f) { f.Rel(9, 0x624, 0, 160); }, "R_ARM_IRELATIVE");
  bad([](auto& f) { f.Rel(9, 0x624, 0, 17); }, "R_ARM_TLS_DTPMOD32");
  bad([](auto& f) { f.Tag(27, 3); }, "init/fini array");
  bad([](auto& f) { W32(f.file, 0x620, 0x600); }, "constructor/finalizer");
  bad([](auto& f) { f.Tag(1, 34); }, "DT_NEEDED 'fixture.so'");
  bad([](auto& f) { f.Tag(7, 0x400); }, "DT_RELA");
  bad([](auto& f) { f.Tag(36, 0x400); }, "incomplete RELR");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 4); f.Tag(37, 8);
  }, "RELR entry size");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 3); f.Tag(37, 4);
  }, "RELR table size");
  bad([](auto& f) {
    f.Tag(36, 0x5f0); f.Tag(35, 4); f.Tag(37, 4);
  }, "RELR table exceeds");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 4); f.Tag(37, 4);
    W32(f.file, 0x4a0, 3);
  }, "RELR bitmap has no preceding address");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 4); f.Tag(37, 4);
    W32(f.file, 0x4a0, 0x200);
  }, "RELR target is not in a writable");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 4); f.Tag(37, 4);
    W32(f.file, 0x4a0, 0x600);
  }, "duplicate relocation");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 4); f.Tag(37, 4);
    f.Tag(0x6fffe000, 0x4a0);
  }, "mixed RELR");
  bad([](auto& f) { f.Tag(0x6fffe005, 2); }, "Android RELR count");
  bad([](auto& f) {
    f.Tag(36, 0x4a0); f.Tag(35, 8); f.Tag(37, 4);
    W32(f.file, 0x4a0, 0xfffffff8);
    W32(f.file, 0x4a4, 3);
  }, "RELR target is unaligned or outside");

  bad([](auto& f) { f.Tag(0x6000000f, 0x400); }, "incomplete Android packed REL");
  bad([](auto& f) {
    PackedAps2(f, true); f.file[0x4a0] = 'X';
  }, "invalid Android APS2 relocation header");
  bad([](auto& f) {
    PackedAps2(f, true); f.Tag(0x60000010, 5); f.file[0x4a4] = 0x80;
  }, "invalid Android APS2 relocation count");
  bad([](auto& f) {
    PackedAps2(f, true); f.file[0x4a7] = 0;
  }, "invalid Android APS2 group size");
  bad([](auto& f) {
    PackedAps2(f, true); f.file[0x4a8] = 8;
  }, "invalid flags or addends");
  bad([](auto& f) {
    PackedAps2(f, true); f.file[0x4a5] = 0xa0; // first offset 0x620, first relocation 0x624 duplicates REL.
  }, "duplicate relocation");
  bad([](auto& f) {
    PackedAps2(f, true); f.Tag(0x60000010, 12);
  }, "trailing Android APS2 relocation data");
  bad([](auto& f) {
    PackedAps2(f, true); f.file[0x4aa] = 0x7f;
  }, "truncated Android APS2 group info");
  bad([](auto& f) { f.Tag(0x60000011, 0x400); }, "ELF32 Android packed RELA");

  bad([](auto& f) { f.Tag(0x6ffffff0, 0x400); }, "versioned");
  bad([](auto& f) { f.Tag(0x12345678, 0); }, "unsupported dynamic tag");
  bad([](auto& f) { f.Tag(30, 0x10); }, "dynamic flags");
  bad([](auto& f) { f.Tag(0x6ffffffa, 2); }, "DT_RELCOUNT");
  bad([](auto& f) { f.GnuHash(); W32(f.file, 0x468, 0); }, "DT_GNU_HASH layout");
  bad([](auto& f) { f.GnuHash(); W32(f.file, 0x474, 0xfffffffe); }, "bucket index");
  bad([](auto& f) {
    f.GnuHash();
    std::fill(f.file.begin() + 0x478, f.file.begin() + 0x500, 0);
  }, "unterminated DT_GNU_HASH");

  ElfLoadOptions options = Options();
  options.load_bias = 0xffffff00;
  Rejected({}, "address overflow", options);
  options = Options(); options.max_image_size = 2048;
  Rejected({}, "memory limit", options);
  options = Options(); options.max_symbols = 5;
  Rejected({}, "DT_HASH size", options);
  options = Options(); options.max_relocations = 9;
  Rejected({}, "REL table size", options);
  options = {};  // No importer: an actual unresolved required import is an error.
  Rejected({}, "unresolved import 'host_add'", options);
  int imports = 0;
  Fixture invalid;
  invalid.Rel(9, 0x624, 0, 160);
  Rejected(invalid, "R_ARM_IRELATIVE", Options(&imports));
  CHECK(imports == 0);  // Full relocation validation precedes importer callbacks.

  options = Options();
  options.resolve_import = [](std::string_view, uint32_t*) -> bool { throw std::runtime_error("test importer"); };
  Rejected({}, "import resolver threw", options);
  options.resolve_import = [](std::string_view, uint32_t*) -> bool { throw 17; };
  Rejected({}, "import resolver threw", options);
}

#ifndef GESHER_ELF_NO_ALLOCATION_INJECTION
void AllocationFailures() {
  Fixture f;
  const auto original = f.file;
  const auto options = Options();
  bool completed = false;
  // Fail each allocation point in turn, including a persistently failing
  // allocator while formatting the diagnostic. No output can be published
  // until the entire owned image is ready.
  for (long budget = 0; budget < 256; ++budget) {
    ElfImage image;
    image.guest_base = 123;
    image.bytes = {9, 8, 7};
    image.soname = "original";
    std::string error;
    bool threw = false, loaded = false;
    allocations_before_failure = budget;
    try { loaded = LoadElf32(f.file.data(), f.file.size(), options, &image, &error); }
    catch (...) { threw = true; }
    allocations_before_failure = -1;
    CHECK(!threw);
    CHECK(f.file == original);
    if (loaded) { completed = true; break; }
    CHECK(image.guest_base == 123 && image.soname == "original");
    CHECK((image.bytes == std::vector<uint8_t>{9, 8, 7}));
  }
  CHECK(completed);
}
#endif

}  // namespace

int main() {
  ValidImages();
  HostileImages();
#ifndef GESHER_ELF_NO_ALLOCATION_INJECTION
  AllocationFailures();
#endif
  std::puts("ARM32 ELF image, symbols, REL relocations and rollback contracts passed");
  return 0;
}
