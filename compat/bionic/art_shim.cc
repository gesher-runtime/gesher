// gesher: the stand-in libart.so for packers that hook ART by symbol
// (gesher/art_shim.h, docs/decisions/0086-packer-art-hooks.md).

#include <gesher/art_shim.h>

#include <gesher/elf_loader.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif

namespace gesher::bionic {

const char kArtLoadClassSymbol[] =
    "_ZN3art11ClassLinker9LoadClassEPNS_6ThreadERKNS_7DexFileERKNS_3dex8ClassDefENS_"
    "6HandleINS_6mirror5ClassEEE";

namespace {

// The hookable function: an ordinary System V prologue and epilogue around
// two long NOPs, and nothing else. An inline hook replaces its first bytes
// with a jump (5 or 14 bytes) and runs the instructions it displaced from a
// trampoline before jumping back; every instruction here is position
// independent, so displaced ones run anywhere, and the prologue ends on an
// instruction boundary at 13 bytes, the NOPs past it at 21 and 29.
constexpr uint8_t kLoadClassCode[] = {
    0x55,                                            // push rbp
    0x48, 0x89, 0xe5,                                // mov rbp, rsp
    0x53,                                            // push rbx
    0x41, 0x54,                                      // push r12
    0x41, 0x55,                                      // push r13
    0x41, 0x56,                                      // push r14
    0x41, 0x57,                                      // push r15
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // nop dword [rax+rax*1+0]
    0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,  // nop dword [rax+rax*1+0]
    0x41, 0x5f,                                      // pop r15
    0x41, 0x5e,                                      // pop r14
    0x41, 0x5d,                                      // pop r13
    0x41, 0x5c,                                      // pop r12
    0x5b,                                            // pop rbx
    0x5d,                                            // pop rbp
    0xc3,                                            // ret
};

constexpr char kSoname[] = "libart.so";
constexpr char kDexFileSoname[] = "libdexfile.so";
constexpr size_t kPage = 4096;
// A stand-in directory untouched this long is no longer being written or
// loaded by a process starting up; a longer-running one keeps its files mapped.
constexpr std::chrono::seconds kArtShimPruneAge{24 * 60 * 60};

uint32_t GnuHash(const char* name) {
  uint32_t h = 5381;
  for (const auto* p = reinterpret_cast<const uint8_t*>(name); *p != 0; ++p) h += (h << 5) + *p;
  return h;
}

// Appends `size` bytes at an `align`ed offset of `out`; returns the offset.
size_t Append(std::vector<uint8_t>* out, const void* data, size_t size, size_t align) {
  while (out->size() % align != 0) out->push_back(0);
  const size_t offset = out->size();
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
  return offset;
}

template <typename T>
void Put(std::vector<uint8_t>* out, size_t offset, const T& value) {
  std::memcpy(out->data() + offset, &value, sizeof(T));
}

std::string Base(const char* path) {
  std::string s(path);
  const size_t slash = s.find_last_of("/\\");
  return slash == std::string::npos ? s : s.substr(slash + 1);
}

bool ReadFile(const std::string& path, std::string* data) {
  const int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) return false;
  data->clear();
  char buf[4096];
  for (;;) {
    const int n = static_cast<int>(read(fd, buf, sizeof(buf)));
    if (n < 0) {
      close(fd);
      return false;
    }
    if (n == 0) break;
    data->append(buf, static_cast<size_t>(n));
  }
  close(fd);
  return true;
}

// A directory per image set, under the stand-ins' directory: its machine and a
// 64-bit FNV-1a of its contents, so a process that has an older stand-in
// mapped keeps it and a file is never rewritten while mapped.
std::string ImageDirectory(const char* kind, const std::string& image,
                           const std::string& more = std::string()) {
  uint64_t fnv = 14695981039346656037ull;
  for (unsigned char c : image) fnv = (fnv ^ c) * 1099511628211ull;
  for (unsigned char c : more) fnv = (fnv ^ c) * 1099511628211ull;
  char hash[17];
  std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(fnv));
  return std::string(kind) + "-" + hash;
}

bool IsHexName(const std::string& name, size_t from, size_t digits) {
  if (name.size() != from + digits) return false;
  for (size_t i = from; i < name.size(); ++i) {
    if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
  }
  return true;
}


// mkdir -p.
void MakeDirectories(const std::string& dir) {
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
      const std::string part = dir.substr(0, i);
      if (!part.empty() && part.back() != ':') mkdir(part.c_str(), 0755);
    }
  }
}

// The stand-in directory `dir` is in use: its last-write time becomes now, so
// that another Gesher's PruneArtShimDirectories, which goes by that time,
// does not take it for an abandoned one while this process loads from it
// (reusing a directory writes nothing into it). A failure is ignored.
void MarkDirectoryUsed(const std::string& dir) {
  std::error_code ignored;
  std::filesystem::last_write_time(std::filesystem::path(dir),
                                   std::filesystem::file_time_type::clock::now(), ignored);
}

// Makes `path` hold exactly `image`: an existing file with these bytes is used
// as it is (another process may have it mapped); otherwise the bytes go to a
// file of this process and are renamed into place.
bool WriteOnce(const std::string& path, const std::string& image, std::string* error) {
  std::string existing;
  if (ReadFile(path, &existing) && existing == image) return true;
  const std::string temp = path + "." + std::to_string(getpid()) + ".tmp";
  const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    *error = "cannot create " + temp + ": " + std::strerror(errno);
    return false;
  }
  const bool written = write(fd, image.data(), static_cast<unsigned>(image.size())) ==
                       static_cast<int>(image.size());
  close(fd);
  if (!written) {
    unlink(temp.c_str());
    *error = "cannot write " + temp;
    return false;
  }
  if (rename(temp.c_str(), path.c_str()) != 0) {
    // Another process put it there first (or holds an old one mapped): use it
    // if it is this image.
    unlink(temp.c_str());
    if (ReadFile(path, &existing) && existing == image) return true;
    *error = "cannot create " + path + ": " + std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace

uint16_t ArtHookingPackerMachine(const char* path) {
  if (path == nullptr) return 0;
  const std::string base = Base(path);
  if (base == "libexec.so" || base == "libexecmain.so" || base == "libexec_x86.so" ||
      base == "libexecmain_x86.so") {
    return EM_X86_64;
  }
  // x86-64 NetEase Yidun is not covered: it needs OpenCommon and the Runtime
  // functions, which only the arm64 stand-in has.
  if (base == "libnesec.so") return EM_AARCH64;
  return 0;
}

bool IsArtHookingPacker(const char* path) { return ArtHookingPackerMachine(path) != 0; }

const char kArtShimKindX86_64[] = "x86_64";
const char kArtShimKindArm64[] = "arm64";

void PruneArtShimDirectories(const std::string& dir, const std::string& keep,
                             std::chrono::seconds min_age) {
  namespace fs = std::filesystem;
  const size_t dash = keep.find('-');
  if (dash == std::string::npos) return;
  const std::string kind = keep.substr(0, dash + 1);
  std::error_code ec;
  const auto now = fs::file_time_type::clock::now();
  std::vector<fs::path> stale;
  for (fs::directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    // This kind's other images, and the 32-bit names every kind used before.
    if (name == keep || !(IsHexName(name, 0, 8) ||
                          (name.compare(0, kind.size(), kind) == 0 &&
                           IsHexName(name, kind.size(), 16)))) {
      continue;
    }
    std::error_code type_error;
    if (!it->is_directory(type_error) || type_error) continue;
    std::error_code time_error;
    const auto written = fs::last_write_time(it->path(), time_error);
    if (time_error || now - written < min_age) continue;
    stale.push_back(it->path());
  }
  for (const fs::path& sub : stale) {
    // A file another process has mapped cannot be deleted (Windows): it and
    // its directory stay, to be pruned by a later process.
    std::error_code ignored;
    for (fs::directory_iterator it(sub, ignored), end; !ignored && it != end; it.increment(ignored)) {
      std::error_code each;
      fs::remove(it->path(), each);
    }
    fs::remove(sub, ignored);
  }
}

std::string ArtShimImage() {
  // Layout, in one read-execute PT_LOAD from file offset 0 at address 0, so
  // that each file offset is also the address relative to the load base:
  // headers, .text, .dynsym, .dynstr, .hash, .gnu.hash, .dynamic, .symtab,
  // .strtab, .shstrtab, then the section headers.
  constexpr int kPhnum = 3;
  std::vector<uint8_t> out(sizeof(Elf64_Ehdr) + kPhnum * sizeof(Elf64_Phdr), 0);

  const size_t text = Append(&out, kLoadClassCode, sizeof(kLoadClassCode), 64);

  // String table: "", soname, symbol.
  std::string strtab(1, '\0');
  const auto soname_name = static_cast<uint32_t>(strtab.size());
  strtab += kSoname;
  strtab += '\0';
  const auto symbol_name = static_cast<uint32_t>(strtab.size());
  strtab += kArtLoadClassSymbol;
  strtab += '\0';

  Elf64_Sym syms[2] = {};
  syms[1].st_name = symbol_name;
  syms[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
  syms[1].st_other = STV_DEFAULT;
  syms[1].st_shndx = 1;  // .text
  syms[1].st_value = text;
  syms[1].st_size = sizeof(kLoadClassCode);
  const size_t dynsym = Append(&out, syms, sizeof(syms), 8);
  const size_t dynstr = Append(&out, strtab.data(), strtab.size(), 1);

  // DT_HASH: one bucket (every hash modulo 1), holding the symbol; no chain.
  const uint32_t hash[] = {1, 2, 1, 0, 0};  // nbucket, nchain, bucket[0], chain[0..1]
  const size_t sysv_hash = Append(&out, hash, sizeof(hash), 8);

  // DT_GNU_HASH: one bucket, symbols from index 1, a one-word bloom filter.
  const uint32_t h = GnuHash(kArtLoadClassSymbol);
  constexpr uint32_t kBloomShift = 6;
  struct {
    uint32_t nbuckets = 1, symoffset = 1, bloom_size = 1, bloom_shift = kBloomShift;
    uint64_t bloom;
    uint32_t bucket = 1;
    uint32_t chain;
  } gnu;
  gnu.bloom = (uint64_t{1} << (h % 64)) | (uint64_t{1} << ((h >> kBloomShift) % 64));
  gnu.chain = h | 1;  // the last (only) symbol of its chain
  const size_t gnu_hash = Append(&out, &gnu, sizeof(gnu), 8);

  const Elf64_Dyn dyn[] = {
      {DT_HASH, {sysv_hash}},     {DT_GNU_HASH, {gnu_hash}},
      {DT_STRTAB, {dynstr}},      {DT_SYMTAB, {dynsym}},
      {DT_STRSZ, {strtab.size()}}, {DT_SYMENT, {sizeof(Elf64_Sym)}},
      {DT_SONAME, {soname_name}}, {DT_NULL, {0}},
  };
  const size_t dynamic = Append(&out, dyn, sizeof(dyn), 8);

  // .symtab/.strtab: the same symbol, for parsers that read the file's
  // section headers instead of the dynamic segment.
  const size_t symtab = Append(&out, syms, sizeof(syms), 8);
  const size_t symstr = Append(&out, strtab.data(), strtab.size(), 1);

  const char shstr[] =
      "\0.text\0.dynsym\0.dynstr\0.hash\0.gnu.hash\0.dynamic\0.symtab\0.strtab\0.shstrtab";
  const size_t shstrtab = Append(&out, shstr, sizeof(shstr), 1);
  auto name_of = [&](const char* section) {
    for (size_t i = 0; i < sizeof(shstr); ++i) {
      if (std::strcmp(shstr + i, section) == 0) return static_cast<uint32_t>(i);
    }
    return 0u;
  };

  Elf64_Shdr sh[10] = {};
  auto section = [&](int i, const char* name, uint32_t type, uint64_t flags, size_t offset,
                     size_t size, uint32_t link, uint32_t info, uint64_t align, uint64_t entsize) {
    sh[i].sh_name = name_of(name);
    sh[i].sh_type = type;
    sh[i].sh_flags = flags;
    sh[i].sh_addr = (flags & SHF_ALLOC) != 0 ? offset : 0;
    sh[i].sh_offset = offset;
    sh[i].sh_size = size;
    sh[i].sh_link = link;
    sh[i].sh_info = info;
    sh[i].sh_addralign = align;
    sh[i].sh_entsize = entsize;
  };
  section(1, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text, sizeof(kLoadClassCode), 0, 0,
          64, 0);
  section(2, ".dynsym", SHT_DYNSYM, SHF_ALLOC, dynsym, sizeof(syms), 3, 1, 8, sizeof(Elf64_Sym));
  section(3, ".dynstr", SHT_STRTAB, SHF_ALLOC, dynstr, strtab.size(), 0, 0, 1, 0);
  section(4, ".hash", SHT_HASH, SHF_ALLOC, sysv_hash, sizeof(hash), 2, 0, 8, 4);
  section(5, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, gnu_hash, sizeof(gnu), 2, 0, 8, 0);
  section(6, ".dynamic", SHT_DYNAMIC, SHF_ALLOC, dynamic, sizeof(dyn), 3, 0, 8,
          sizeof(Elf64_Dyn));
  section(7, ".symtab", SHT_SYMTAB, 0, symtab, sizeof(syms), 8, 1, 8, sizeof(Elf64_Sym));
  section(8, ".strtab", SHT_STRTAB, 0, symstr, strtab.size(), 0, 0, 1, 0);
  section(9, ".shstrtab", SHT_STRTAB, 0, shstrtab, sizeof(shstr), 0, 0, 1, 0);
  const size_t shoff = Append(&out, sh, sizeof(sh), 8);
  // Whole pages: gesher-base maps whole pages of a file as a view and copies a
  // final partial page into anonymous memory, which /proc/self/maps cannot name
  // (the packer looks for the line naming libart.so).
  out.resize((out.size() + kPage - 1) / kPage * kPage, 0);

  Elf64_Ehdr eh = {};
  eh.e_ident[EI_MAG0] = ELFMAG0;
  eh.e_ident[EI_MAG1] = ELFMAG1;
  eh.e_ident[EI_MAG2] = ELFMAG2;
  eh.e_ident[EI_MAG3] = ELFMAG3;
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
  eh.e_type = ET_DYN;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_shoff = shoff;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = kPhnum;
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = 10;
  eh.e_shstrndx = 9;
  Put(&out, 0, eh);

  Elf64_Phdr ph[kPhnum] = {};
  ph[0].p_type = PT_LOAD;
  ph[0].p_flags = PF_R | PF_X;
  ph[0].p_offset = 0;
  ph[0].p_vaddr = ph[0].p_paddr = 0;
  ph[0].p_filesz = ph[0].p_memsz = out.size();
  ph[0].p_align = kPage;
  ph[1].p_type = PT_DYNAMIC;
  ph[1].p_flags = PF_R;
  ph[1].p_offset = ph[1].p_vaddr = ph[1].p_paddr = dynamic;
  ph[1].p_filesz = ph[1].p_memsz = sizeof(dyn);
  ph[1].p_align = 8;
  ph[2].p_type = PT_GNU_STACK;
  ph[2].p_flags = PF_R | PF_W;
  for (int i = 0; i < kPhnum; ++i) {
    Put(&out, sizeof(Elf64_Ehdr) + i * sizeof(Elf64_Phdr), ph[i]);
  }
  return std::string(out.begin(), out.end());
}

void* LoadArtShim(const std::string& dir, std::string* path, std::string* error) {
  static std::mutex mutex;
  static void* entry = nullptr;
  static std::string loaded_path;
  std::lock_guard<std::mutex> lock(mutex);
  if (entry != nullptr) {
    *path = loaded_path;
    return entry;
  }
  const std::string image = ArtShimImage();
  const std::string name = ImageDirectory(kArtShimKindX86_64, image);
  const std::string sub = dir + "/" + name;
  MakeDirectories(sub);
  MarkDirectoryUsed(sub);
  *path = sub + "/" + kSoname;
  if (!WriteOnce(*path, image, error)) return nullptr;
  elf::Library* lib = elf::Load(path->c_str(), error);
  if (lib == nullptr) return nullptr;
  void* fn = elf::Lookup(lib, kArtLoadClassSymbol);
  if (fn == nullptr) {
    *error = *path + " has no " + kArtLoadClassSymbol;
    return nullptr;
  }
  entry = fn;
  loaded_path = *path;
  PruneArtShimDirectories(dir, name, kArtShimPruneAge);
  return entry;
}

// --- the arm64 stand-in -------------------------------------------------------

const char kArtDisableVerifierSymbol[] = "_ZN3art7Runtime15DisableVerifierEv";
const char kArtIsVerificationEnabledSymbol[] = "_ZNK3art7Runtime21IsVerificationEnabledEv";
const char kArtIsVerificationSoftFailSymbol[] = "_ZNK3art7Runtime22IsVerificationSoftFailEv";
const char kArtOatVersionSymbol[] = "_ZN3art9OatHeader11kOatVersionE";
const char kArtRuntimeInstanceSymbol[] = "_ZN3art7Runtime9instance_E";
const char kArtOpenCommonSymbol[] =
    "_ZN3art13DexFileLoader10OpenCommonENSt3__110shared_ptrINS_16DexFileContainerEEEPKhmRKNS1_"
    "12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEENS1_8optionalIjEEPKNS_10OatDexFileE"
    "bbPSC_PNS_22DexFileLoaderErrorCodeE";
const char kArtOpenCommonEntrySymbol[] = "__gesher_art_open_common_entry";

const char kArtShimDisableVerifierImport[] = "__gesher_art_runtime_disable_verifier";
const char kArtShimIsVerificationEnabledImport[] = "__gesher_art_runtime_is_verification_enabled";
const char kArtShimIsVerificationSoftFailImport[] = "__gesher_art_runtime_is_verification_soft_fail";
const char kArtShimOpenCommonImport[] = "__gesher_art_dex_file_loader_open_common";

namespace {

constexpr uint32_t kA64Nop = 0xd503201f;

// LoadClass: a frame record around four NOPs. An inline hook replaces the first
// four or five instructions with a branch (LDR X17, #8; BR X17; .quad target)
// and runs the displaced ones from a trampoline; none is PC-relative.
constexpr uint32_t kA64LoadClass[] = {
    0xa9bf7bfd,                          // stp x29, x30, [sp, #-16]!
    0x910003fd,                          // mov x29, sp
    kA64Nop, kA64Nop, kA64Nop, kA64Nop,  // nop x4
    0xa8c17bfd,                          // ldp x29, x30, [sp], #16
    0xd65f03c0,                          // ret
};

// A Runtime member function: six NOPs a hook can displace, then a tail call
// through the function's GOT slot (LDR X16, <slot>; BR X16) to the host
// function the ELF loader bound it to. Its arguments and X30 are untouched.

uint32_t A64LdrLiteralX16(int64_t delta) {
  return 0x58000010u | ((static_cast<uint32_t>(delta / 4) & 0x7ffffu) << 5);
}

uint32_t A64Bl(int64_t delta) { return 0x94000000u | (static_cast<uint32_t>(delta / 4) & 0x3ffffffu); }

// DexFileLoader::OpenCommon (AAPCS64: the unique_ptr result's storage in X8,
// eight arguments in X0-X7, error_msg and error_code on the stack): six NOPs a
// hook can displace, then a call to the host function through the GOT with
// the storage first (X0) and the rest shifted up one, the ninth argument on
// as a stack argument, as the translator's host call passes them on.
constexpr uint32_t kA64OpenCommon[] = {
    kA64Nop, kA64Nop, kA64Nop, kA64Nop, kA64Nop, kA64Nop,
    0xa9bf7bfd,  // stp x29, x30, [sp, #-16]!
    0x910003fd,  // mov x29, sp
    0xd10083ff,  // sub sp, sp, #32
    0xa9412ba9,  // ldp x9, x10, [x29, #16]   ; error_msg, error_code
    0xf90003e7,  // str x7, [sp]              ; verify_checksum
    0xa900abe9,  // stp x9, x10, [sp, #8]
    0xaa0603e7,  // mov x7, x6
    0xaa0503e6,  // mov x6, x5
    0xaa0403e5,  // mov x5, x4
    0xaa0303e4,  // mov x4, x3
    0xaa0203e3,  // mov x3, x2
    0xaa0103e2,  // mov x2, x1
    0xaa0003e1,  // mov x1, x0
    0xaa0803e0,  // mov x0, x8
    0,           // ldr x16, <GOT slot>
    0xd63f0200,  // blr x16
    0x910003bf,  // mov sp, x29
    0xa8c17bfd,  // ldp x29, x30, [sp], #16
    0xd65f03c0,  // ret
};
constexpr size_t kA64OpenCommonLdr = 20;

// The entry ART calls (through a host entry, System V order): the storage
// first, then OpenCommon's arguments. It makes the device's call to
// OpenCommon, so a hook placed there runs as on a device.
constexpr uint32_t kA64OpenCommonEntry[] = {
    0xa9bf7bfd,  // stp x29, x30, [sp, #-16]!
    0x910003fd,  // mov x29, sp
    0xd10043ff,  // sub sp, sp, #16
    0xa941aba9,  // ldp x9, x10, [x29, #24]   ; error_msg, error_code
    0xa9002be9,  // stp x9, x10, [sp]
    0xaa0003e8,  // mov x8, x0
    0xaa0103e0,  // mov x0, x1
    0xaa0203e1,  // mov x1, x2
    0xaa0303e2,  // mov x2, x3
    0xaa0403e3,  // mov x3, x4
    0xaa0503e4,  // mov x4, x5
    0xaa0603e5,  // mov x5, x6
    0xaa0703e6,  // mov x6, x7
    0xf9400ba7,  // ldr x7, [x29, #16]        ; verify_checksum
    0,           // bl OpenCommon
    0x910003bf,  // mov sp, x29
    0xa8c17bfd,  // ldp x29, x30, [sp], #16
    0xd65f03c0,  // ret
};
constexpr size_t kA64OpenCommonEntryBl = 14;

}  // namespace

namespace {

// One arm64 stand-in object. A device's ART is two libraries: libart.so
// (ClassLinker, Runtime, OatHeader) and libdexfile.so (DexFileLoader, which
// libart.so imports); a shell looks for each symbol in the library that
// defines it there, so each stand-in exports what its library does.
std::string A64ShimImage(bool dexfile, const std::string& oat_version) {
  struct Function {
    const char* name;
    std::vector<uint32_t> words;
    int ldr = -1;   // the word loading a GOT slot
    int slot = -1;  // which
    int bl = -1;    // the word calling another function
    int callee = -1;
  };
  const std::vector<uint32_t> forwarder = {kA64Nop, kA64Nop, kA64Nop, kA64Nop,
                                           kA64Nop, kA64Nop, 0,       0xd61f0200 /* br x16 */};
  // libart.so: LoadClass, the three Runtime functions (each forwarding through
  // the import of the same index), then OatHeader::kOatVersion and
  // Runtime::instance_. libdexfile.so: OpenCommon (through its one import) and
  // its entry.
  const std::vector<const char*> imports =
      dexfile ? std::vector<const char*>{kArtShimOpenCommonImport}
              : std::vector<const char*>{kArtShimDisableVerifierImport,
                                         kArtShimIsVerificationEnabledImport,
                                         kArtShimIsVerificationSoftFailImport};
  const std::vector<Function> functions =
      dexfile ? std::vector<Function>{{kArtOpenCommonSymbol,
                                       {std::begin(kA64OpenCommon), std::end(kA64OpenCommon)},
                                       static_cast<int>(kA64OpenCommonLdr),
                                       0},
                                      {kArtOpenCommonEntrySymbol,
                                       {std::begin(kA64OpenCommonEntry), std::end(kA64OpenCommonEntry)},
                                       -1,
                                       -1,
                                       static_cast<int>(kA64OpenCommonEntryBl),
                                       0}}
              : std::vector<Function>{
                    {kArtLoadClassSymbol, {std::begin(kA64LoadClass), std::end(kA64LoadClass)}},
                    {kArtDisableVerifierSymbol, forwarder, 6, 0},
                    {kArtIsVerificationEnabledSymbol, forwarder, 6, 1},
                    {kArtIsVerificationSoftFailSymbol, forwarder, 6, 2}};
  const size_t kImports = imports.size();
  const size_t kFunctions = functions.size();
  const size_t objects = dexfile ? 0 : 2;  // kOatVersion, instance_
  const char* soname = dexfile ? kDexFileSoname : kSoname;
  constexpr int kPhnum = 4;
  constexpr int kShnum = 14;

  std::vector<uint8_t> out(sizeof(Elf64_Ehdr) + kPhnum * sizeof(Elf64_Phdr), 0);
  // .text: each function 32 bytes; the forwarders' LDR literals are patched
  // once the GOT's address is known.
  std::vector<size_t> fn_offset(kFunctions);
  for (size_t i = 0; i < kFunctions; ++i) {
    fn_offset[i] = Append(&out, functions[i].words.data(), functions[i].words.size() * 4,
                          i == 0 ? 64 : 16);
  }
  const size_t text = fn_offset[0];
  for (size_t i = 0; i < kFunctions; ++i) {
    if (functions[i].bl < 0) continue;
    const size_t at = fn_offset[i] + functions[i].bl * 4;
    Put(&out, at,
        A64Bl(static_cast<int64_t>(fn_offset[functions[i].callee]) - static_cast<int64_t>(at)));
  }
  const size_t text_size = out.size() - text;
  std::string version = oat_version;
  version.resize(4, '\0');
  const size_t rodata = Append(&out, version.data(), 4, 8);

  // Strings: "", soname, imports, exports.
  std::string strtab(1, '\0');
  auto add_string = [&strtab](const char* s) {
    const auto offset = static_cast<uint32_t>(strtab.size());
    strtab += s;
    strtab += '\0';
    return offset;
  };
  const uint32_t soname_name = add_string(soname);
  // Symbols: null, the imports (undefined), then the exports, which DT_GNU_HASH
  // covers from index symoffset on.
  std::vector<Elf64_Sym> syms(1 + kImports + kFunctions + objects);
  for (size_t i = 0; i < kImports; ++i) {
    Elf64_Sym& s = syms[1 + i];
    s.st_name = add_string(imports[i]);
    s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    s.st_shndx = SHN_UNDEF;
  }
  const size_t first_export = 1 + kImports;
  for (size_t i = 0; i < kFunctions; ++i) {
    Elf64_Sym& s = syms[first_export + i];
    s.st_name = add_string(functions[i].name);
    s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    s.st_shndx = 1;  // .text
    s.st_value = fn_offset[i];
    s.st_size = functions[i].words.size() * 4;
  }
  // Runtime::instance_ (its value set after loading, LoadArtShimArm64) lives
  // in .data, after the GOT in the read-write page; its address below.
  const size_t instance_index = syms.size() - 1;
  if (!dexfile) {
    Elf64_Sym& s = syms[syms.size() - 2];
    s.st_name = add_string(kArtOatVersionSymbol);
    s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    s.st_shndx = 2;  // .rodata
    s.st_value = rodata;
    s.st_size = 4;
    syms[instance_index].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT);
    syms[instance_index].st_shndx = 13;  // .data
    syms[instance_index].st_size = 8;
    syms[instance_index].st_name = add_string(kArtRuntimeInstanceSymbol);
  }
  const size_t nsyms = syms.size();
  const size_t dynsym = Append(&out, syms.data(), nsyms * sizeof(Elf64_Sym), 8);
  const size_t dynstr = Append(&out, strtab.data(), strtab.size(), 1);

  // DT_HASH: one bucket whose chain runs from the last symbol down to index 1.
  std::vector<uint32_t> hash = {1, static_cast<uint32_t>(nsyms), static_cast<uint32_t>(nsyms - 1)};
  for (size_t i = 0; i < nsyms; ++i) hash.push_back(i == 0 ? 0 : static_cast<uint32_t>(i - 1));
  const size_t sysv_hash = Append(&out, hash.data(), hash.size() * 4, 8);

  // DT_GNU_HASH: one bucket from the first export, a one-word bloom filter.
  constexpr uint32_t kBloomShift = 6;
  std::vector<uint8_t> gnu;
  {
    const uint32_t header[] = {1, static_cast<uint32_t>(first_export), 1, kBloomShift};
    uint64_t bloom = 0;
    std::vector<uint32_t> chain;
    for (size_t i = first_export; i < nsyms; ++i) {
      const uint32_t h = GnuHash(strtab.c_str() + syms[i].st_name);
      bloom |= (uint64_t{1} << (h % 64)) | (uint64_t{1} << ((h >> kBloomShift) % 64));
      chain.push_back((h & ~1u) | (i + 1 == nsyms ? 1u : 0u));
    }
    const uint32_t bucket = static_cast<uint32_t>(first_export);
    gnu.insert(gnu.end(), reinterpret_cast<const uint8_t*>(header),
               reinterpret_cast<const uint8_t*>(header) + sizeof(header));
    gnu.insert(gnu.end(), reinterpret_cast<const uint8_t*>(&bloom),
               reinterpret_cast<const uint8_t*>(&bloom) + sizeof(bloom));
    gnu.insert(gnu.end(), reinterpret_cast<const uint8_t*>(&bucket),
               reinterpret_cast<const uint8_t*>(&bucket) + sizeof(bucket));
    gnu.insert(gnu.end(), reinterpret_cast<const uint8_t*>(chain.data()),
               reinterpret_cast<const uint8_t*>(chain.data()) + chain.size() * 4);
  }
  const size_t gnu_hash = Append(&out, gnu.data(), gnu.size(), 8);

  // .rela.dyn: R_AARCH64_GLOB_DAT for each GOT slot; the GOT is the first
  // bytes of the read-write segment, whose address is known only below.
  const size_t rela = Append(&out, std::vector<uint8_t>(kImports * sizeof(Elf64_Rela)).data(),
                             kImports * sizeof(Elf64_Rela), 8);
  const size_t dyn_count = 12;
  const size_t dynamic = Append(&out, std::vector<uint8_t>(dyn_count * sizeof(Elf64_Dyn)).data(),
                                dyn_count * sizeof(Elf64_Dyn), 8);
  const size_t symtab = Append(&out, syms.data(), nsyms * sizeof(Elf64_Sym), 8);
  const size_t symstr = Append(&out, strtab.data(), strtab.size(), 1);
  const char shstr[] =
      "\0.text\0.rodata\0.dynsym\0.dynstr\0.hash\0.gnu.hash\0.rela.dyn\0.dynamic\0.symtab\0"
      ".strtab\0.shstrtab\0.got\0.data";
  const size_t shstrtab = Append(&out, shstr, sizeof(shstr), 1);
  const size_t shoff = Append(&out, std::vector<uint8_t>(kShnum * sizeof(Elf64_Shdr)).data(),
                              kShnum * sizeof(Elf64_Shdr), 8);
  // Whole pages for the read-execute segment (see ArtShimImage), then one
  // read-write page holding the GOT.
  out.resize((out.size() + kPage - 1) / kPage * kPage, 0);
  const size_t rx_size = out.size();
  const size_t got = rx_size;
  const size_t data = got + 64;
  out.resize(rx_size + kPage, 0);
  if (!dexfile) {
    syms[instance_index].st_value = data;
    std::memcpy(out.data() + dynsym + instance_index * sizeof(Elf64_Sym), &syms[instance_index],
                sizeof(Elf64_Sym));
    std::memcpy(out.data() + symtab + instance_index * sizeof(Elf64_Sym), &syms[instance_index],
                sizeof(Elf64_Sym));
  }

  for (size_t i = 0; i < kFunctions; ++i) {
    if (functions[i].ldr < 0) continue;
    const size_t ldr = fn_offset[i] + functions[i].ldr * 4;
    const size_t slot = got + functions[i].slot * 8;
    Put(&out, ldr, A64LdrLiteralX16(static_cast<int64_t>(slot) - static_cast<int64_t>(ldr)));
  }
  for (size_t i = 0; i < kImports; ++i) {
    Elf64_Rela r = {};
    r.r_offset = got + i * 8;
    r.r_info = ELF64_R_INFO(1 + i, R_AARCH64_GLOB_DAT);
    Put(&out, rela + i * sizeof(Elf64_Rela), r);
  }
  const Elf64_Dyn dyn[dyn_count] = {
      {DT_HASH, {sysv_hash}},
      {DT_GNU_HASH, {gnu_hash}},
      {DT_STRTAB, {dynstr}},
      {DT_SYMTAB, {dynsym}},
      {DT_STRSZ, {strtab.size()}},
      {DT_SYMENT, {sizeof(Elf64_Sym)}},
      {DT_RELA, {rela}},
      {DT_RELASZ, {kImports * sizeof(Elf64_Rela)}},
      {DT_RELAENT, {sizeof(Elf64_Rela)}},
      {DT_SONAME, {soname_name}},
      {DT_FLAGS, {DF_BIND_NOW}},
      {DT_NULL, {0}},
  };
  std::memcpy(out.data() + dynamic, dyn, sizeof(dyn));

  auto name_of = [&](const char* section) {
    for (size_t i = 0; i < sizeof(shstr); ++i) {
      if (std::strcmp(shstr + i, section) == 0) return static_cast<uint32_t>(i);
    }
    return 0u;
  };
  auto section = [&](int i, const char* name, uint32_t type, uint64_t flags, size_t offset,
                     size_t size, uint32_t link, uint32_t info, uint64_t align, uint64_t entsize) {
    Elf64_Shdr sh = {};
    sh.sh_name = name_of(name);
    sh.sh_type = type;
    sh.sh_flags = flags;
    sh.sh_addr = (flags & SHF_ALLOC) != 0 ? offset : 0;
    sh.sh_offset = offset;
    sh.sh_size = size;
    sh.sh_link = link;
    sh.sh_info = info;
    sh.sh_addralign = align;
    sh.sh_entsize = entsize;
    Put(&out, shoff + i * sizeof(Elf64_Shdr), sh);
  };
  const auto first_global = static_cast<uint32_t>(1);
  section(1, ".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text, text_size, 0, 0, 64, 0);
  section(2, ".rodata", SHT_PROGBITS, SHF_ALLOC, rodata, 4, 0, 0, 8, 0);
  section(3, ".dynsym", SHT_DYNSYM, SHF_ALLOC, dynsym, nsyms * sizeof(Elf64_Sym), 4, first_global, 8,
          sizeof(Elf64_Sym));
  section(4, ".dynstr", SHT_STRTAB, SHF_ALLOC, dynstr, strtab.size(), 0, 0, 1, 0);
  section(5, ".hash", SHT_HASH, SHF_ALLOC, sysv_hash, hash.size() * 4, 3, 0, 8, 4);
  section(6, ".gnu.hash", SHT_GNU_HASH, SHF_ALLOC, gnu_hash, gnu.size(), 3, 0, 8, 0);
  section(7, ".rela.dyn", SHT_RELA, SHF_ALLOC, rela, kImports * sizeof(Elf64_Rela), 3, 0, 8,
          sizeof(Elf64_Rela));
  section(8, ".dynamic", SHT_DYNAMIC, SHF_ALLOC, dynamic, sizeof(dyn), 4, 0, 8, sizeof(Elf64_Dyn));
  section(9, ".symtab", SHT_SYMTAB, 0, symtab, nsyms * sizeof(Elf64_Sym), 10, first_global, 8,
          sizeof(Elf64_Sym));
  section(10, ".strtab", SHT_STRTAB, 0, symstr, strtab.size(), 0, 0, 1, 0);
  section(11, ".shstrtab", SHT_STRTAB, 0, shstrtab, sizeof(shstr), 0, 0, 1, 0);
  section(12, ".got", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, got, kImports * 8, 0, 0, 8, 8);
  section(13, ".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, data, 8, 0, 0, 8, 0);

  Elf64_Ehdr eh = {};
  std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
  eh.e_type = ET_DYN;
  eh.e_machine = EM_AARCH64;
  eh.e_version = EV_CURRENT;
  eh.e_phoff = sizeof(Elf64_Ehdr);
  eh.e_shoff = shoff;
  eh.e_ehsize = sizeof(Elf64_Ehdr);
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = kPhnum;
  eh.e_shentsize = sizeof(Elf64_Shdr);
  eh.e_shnum = kShnum;
  eh.e_shstrndx = 11;
  Put(&out, 0, eh);

  Elf64_Phdr ph[kPhnum] = {};
  ph[0].p_type = PT_LOAD;
  ph[0].p_flags = PF_R | PF_X;
  ph[0].p_filesz = ph[0].p_memsz = rx_size;
  ph[0].p_align = kPage;
  ph[1].p_type = PT_LOAD;
  ph[1].p_flags = PF_R | PF_W;
  ph[1].p_offset = ph[1].p_vaddr = ph[1].p_paddr = got;
  ph[1].p_filesz = ph[1].p_memsz = kPage;
  ph[1].p_align = kPage;
  ph[2].p_type = PT_DYNAMIC;
  ph[2].p_flags = PF_R;
  ph[2].p_offset = ph[2].p_vaddr = ph[2].p_paddr = dynamic;
  ph[2].p_filesz = ph[2].p_memsz = sizeof(dyn);
  ph[2].p_align = 8;
  ph[3].p_type = PT_GNU_STACK;
  ph[3].p_flags = PF_R | PF_W;
  for (int i = 0; i < kPhnum; ++i) Put(&out, sizeof(Elf64_Ehdr) + i * sizeof(Elf64_Phdr), ph[i]);
  return std::string(out.begin(), out.end());
}

}  // namespace

std::string ArtShimImageArm64(const std::string& oat_version) {
  return A64ShimImage(false, oat_version);
}

std::string DexFileShimImageArm64() { return A64ShimImage(true, std::string()); }

void* LoadArtShimArm64(const std::string& dir, const std::string& oat_version, void* runtime,
                       void** open_common_entry, std::string* path, std::string* error) {
  static std::mutex mutex;
  static void* entry = nullptr;
  static void* open_common = nullptr;
  static std::string loaded_path;
  std::lock_guard<std::mutex> lock(mutex);
  if (entry != nullptr) {
    *path = loaded_path;
    *open_common_entry = open_common;
    return entry;
  }
  // Both objects in one directory named after their contents, as a device's
  // are in its ART module's lib64.
  const std::string image = ArtShimImageArm64(oat_version);
  const std::string dexfile_image = DexFileShimImageArm64();
  const std::string name = ImageDirectory(kArtShimKindArm64, image, dexfile_image);
  const std::string sub = dir + "/" + name;
  MakeDirectories(sub);
  MarkDirectoryUsed(sub);
  const std::string dexfile_path = sub + "/" + kDexFileSoname;
  *path = sub + "/" + kSoname;
  if (!WriteOnce(dexfile_path, dexfile_image, error) || !WriteOnce(*path, image, error)) {
    return nullptr;
  }
  elf::Library* dexfile = elf::Load(dexfile_path.c_str(), error);
  if (dexfile == nullptr) return nullptr;
  elf::Library* lib = elf::Load(path->c_str(), error);
  if (lib == nullptr) return nullptr;
  if (!elf::IsAarch64(lib) || !elf::IsAarch64(dexfile)) {
    *error = sub + " did not load as arm64 code";
    return nullptr;
  }
  void* fn = elf::Lookup(lib, kArtLoadClassSymbol);
  void* instance = elf::Lookup(lib, kArtRuntimeInstanceSymbol);
  void* open_common_fn = elf::Lookup(dexfile, kArtOpenCommonEntrySymbol);
  if (fn == nullptr || instance == nullptr || open_common_fn == nullptr) {
    *error = *path + " lacks " +
             (fn == nullptr ? kArtLoadClassSymbol
                            : instance == nullptr ? kArtRuntimeInstanceSymbol : kArtOpenCommonEntrySymbol);
    return nullptr;
  }
  std::memcpy(instance, &runtime, sizeof(runtime));
  entry = fn;
  open_common = open_common_fn;
  *open_common_entry = open_common;
  loaded_path = *path;
  PruneArtShimDirectories(dir, name, kArtShimPruneAge);
  return entry;
}

}  // namespace gesher::bionic
