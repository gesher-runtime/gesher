// gesher: the stand-in libart.so (compat/bionic/art_shim.cc, ADR 0086) as a
// packer's shell finds and hooks it, with tests/native/art_hook_test.so doing
// what Ijiami's shell does.
//
//   art_shim_test <art_hook_test.so> <scratch directory>
//
// Checks that the stand-in is written once under the directory and loaded,
// that /proc/self/maps names it at its load address (readable, ELF header
// there), that its LoadClass resolves to the same address through DT_GNU_HASH,
// DT_HASH and the file's .symtab, that an inline hook over its first
// instructions takes effect, and that a call with ART's LoadClass arguments
// (as patches/art/0800 makes it) reaches the hook with DexFile::begin_ and
// ClassDef::class_data_off_ where the shell reads them, then returns through
// the hook's trampoline.
//
// First, the arm64 stand-ins for NetEase Yidun's libnesec.so (libart.so and
// libdexfile.so), as images: their ELF headers and segments, sonames, the
// symbols each exports through DT_GNU_HASH and DT_HASH (and which it does
// not), the R_AARCH64_GLOB_DAT GOT slots of their imports and the code that
// reaches them, and the OpenCommon entry's call.

#include <gesher/art_shim.h>
#include <gesher/bionic_shim.h>
#include <gesher/elf_loader.h>

#include <elf.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define SYSV __attribute__((sysv_abi))

namespace {

int g_failures = 0;

void Check(const char* what, bool ok, const std::string& detail = "") {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ",
              detail.c_str());
  if (!ok) ++g_failures;
}

std::string Hex(const void* p) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%p", p);
  return buf;
}

// The device's art::DexFile up to begin_, and a dex::ClassDef.
struct FakeDexFile {
  void* vtable;
  const uint8_t* begin;
};

// --- the arm64 stand-ins (NetEase Yidun's libnesec.so) ----------------------
// Their bytes as the ELF loader and the shell read them: checking the images
// themselves needs no translator.

#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
#ifndef R_AARCH64_GLOB_DAT
#define R_AARCH64_GLOB_DAT 1025
#endif

struct Image {
  std::string bytes;
  const Elf64_Ehdr* eh = nullptr;
  std::vector<Elf64_Phdr> load;  // PT_LOAD
  const Elf64_Dyn* dynamic = nullptr;
  const Elf64_Sym* syms = nullptr;
  size_t nsyms = 0;
  const char* strtab = nullptr;
  const uint32_t* gnu_hash = nullptr;
  const uint32_t* sysv_hash = nullptr;
  const Elf64_Rela* rela = nullptr;
  size_t nrela = 0;
  const char* soname = "";

  // Addresses are file offsets: the images are linked at 0.
  template <typename T>
  const T* At(uint64_t offset, size_t count = 1) const {
    if (offset > bytes.size() || sizeof(T) * count > bytes.size() - offset) return nullptr;
    return reinterpret_cast<const T*>(bytes.data() + offset);
  }
  uint32_t Word(uint64_t offset) const {
    uint32_t w = 0;
    if (offset + 4 <= bytes.size()) std::memcpy(&w, bytes.data() + offset, 4);
    return w;
  }
  uint64_t Quad(uint64_t offset) const {
    uint64_t q = ~uint64_t{0};
    if (offset + 8 <= bytes.size()) std::memcpy(&q, bytes.data() + offset, 8);
    return q;
  }
  // Is [address, address + size) inside a PT_LOAD with exactly these flags?
  bool InSegment(uint64_t address, uint64_t size, uint32_t flags) const {
    for (const Elf64_Phdr& p : load) {
      if (address >= p.p_vaddr && address + size <= p.p_vaddr + p.p_memsz) return p.p_flags == flags;
    }
    return false;
  }
};

bool Parse(const std::string& bytes, Image* image) {
  image->bytes = bytes;
  image->eh = image->At<Elf64_Ehdr>(0);
  if (image->eh == nullptr) return false;
  const auto* ph = image->At<Elf64_Phdr>(image->eh->e_phoff, image->eh->e_phnum);
  if (ph == nullptr) return false;
  for (int i = 0; i < image->eh->e_phnum; ++i) {
    if (ph[i].p_type == PT_LOAD) image->load.push_back(ph[i]);
    if (ph[i].p_type == PT_DYNAMIC) image->dynamic = image->At<Elf64_Dyn>(ph[i].p_offset);
  }
  if (image->dynamic == nullptr) return false;
  uint64_t symtab = 0, strtab = 0, gnu = 0, sysv = 0, rela = 0, relasz = 0, soname = 0;
  for (const Elf64_Dyn* d = image->dynamic; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
      case DT_SYMTAB: symtab = d->d_un.d_ptr; break;
      case DT_STRTAB: strtab = d->d_un.d_ptr; break;
      case DT_GNU_HASH: gnu = d->d_un.d_ptr; break;
      case DT_HASH: sysv = d->d_un.d_ptr; break;
      case DT_RELA: rela = d->d_un.d_ptr; break;
      case DT_RELASZ: relasz = d->d_un.d_val; break;
      case DT_SONAME: soname = d->d_un.d_val; break;
      default: break;
    }
  }
  image->sysv_hash = image->At<uint32_t>(sysv, 2);
  image->gnu_hash = image->At<uint32_t>(gnu, 4);
  if (image->sysv_hash == nullptr || image->gnu_hash == nullptr) return false;
  image->nsyms = image->sysv_hash[1];  // nchain: the number of symbols
  image->syms = image->At<Elf64_Sym>(symtab, image->nsyms);
  image->strtab = image->At<char>(strtab);
  image->nrela = relasz / sizeof(Elf64_Rela);
  image->rela = image->At<Elf64_Rela>(rela, image->nrela);
  if (image->syms == nullptr || image->strtab == nullptr || image->rela == nullptr) return false;
  image->soname = image->strtab + soname;
  return true;
}

uint32_t ImageGnuHash(const char* name) {
  uint32_t h = 5381;
  for (const auto* p = reinterpret_cast<const uint8_t*>(name); *p != 0; ++p) h += (h << 5) + *p;
  return h;
}

uint32_t ImageSysvHash(const char* name) {
  uint32_t h = 0;
  for (const auto* p = reinterpret_cast<const uint8_t*>(name); *p != 0; ++p) {
    h = (h << 4) + *p;
    const uint32_t g = h & 0xf0000000u;
    if (g != 0) h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

// The defined symbol `name` through DT_GNU_HASH (as the shell's lookup and
// bionic's linker find it), or null.
const Elf64_Sym* LookupGnu(const Image& image, const char* name) {
  const uint32_t* g = image.gnu_hash;
  const uint32_t nbuckets = g[0], symoffset = g[1], bloom_size = g[2], shift = g[3];
  if (nbuckets == 0 || bloom_size == 0) return nullptr;
  const auto* bloom = reinterpret_cast<const uint64_t*>(g + 4);
  const auto* buckets = reinterpret_cast<const uint32_t*>(bloom + bloom_size);
  const uint32_t* chain = buckets + nbuckets;
  const uint32_t h = ImageGnuHash(name);
  const uint64_t word = bloom[(h / 64) % bloom_size];
  if ((word & (uint64_t{1} << (h % 64))) == 0 || (word & (uint64_t{1} << ((h >> shift) % 64))) == 0)
    return nullptr;
  for (uint32_t i = buckets[h % nbuckets]; i >= symoffset && i < image.nsyms; ++i) {
    const uint32_t ch = chain[i - symoffset];
    if ((ch | 1) == (h | 1) && std::strcmp(image.strtab + image.syms[i].st_name, name) == 0 &&
        image.syms[i].st_shndx != SHN_UNDEF)
      return &image.syms[i];
    if ((ch & 1) != 0) break;
  }
  return nullptr;
}

// The same through DT_HASH.
const Elf64_Sym* LookupSysv(const Image& image, const char* name) {
  const uint32_t nbucket = image.sysv_hash[0];
  if (nbucket == 0) return nullptr;
  const uint32_t* bucket = image.sysv_hash + 2;
  const uint32_t* chain = bucket + nbucket;
  for (uint32_t i = bucket[ImageSysvHash(name) % nbucket]; i != 0 && i < image.nsyms; i = chain[i]) {
    if (std::strcmp(image.strtab + image.syms[i].st_name, name) == 0 &&
        image.syms[i].st_shndx != SHN_UNDEF)
      return &image.syms[i];
  }
  return nullptr;
}

// The GOT slot an R_AARCH64_GLOB_DAT binds to the import `name`, or 0.
uint64_t GotSlotOf(const Image& image, const char* name) {
  for (size_t i = 0; i < image.nrela; ++i) {
    const size_t sym = ELF64_R_SYM(image.rela[i].r_info);
    if (sym >= image.nsyms) continue;
    if (ELF64_R_TYPE(image.rela[i].r_info) == R_AARCH64_GLOB_DAT &&
        std::strcmp(image.strtab + image.syms[sym].st_name, name) == 0)
      return image.rela[i].r_offset;
  }
  return 0;
}

// Where an LDR (literal) X16 at `at` loads from; 0 if it is not one.
uint64_t LdrX16Target(const Image& image, uint64_t at) {
  const uint32_t w = image.Word(at);
  if ((w & 0xff00001fu) != 0x58000010u) return 0;
  int64_t imm = (w >> 5) & 0x7ffff;
  if ((imm & 0x40000) != 0) imm -= 0x80000;
  return static_cast<uint64_t>(static_cast<int64_t>(at) + imm * 4);
}

// Where a BL at `at` calls; 0 if it is not one.
uint64_t BlTarget(const Image& image, uint64_t at) {
  const uint32_t w = image.Word(at);
  if ((w & 0xfc000000u) != 0x94000000u) return 0;
  int64_t imm = w & 0x3ffffff;
  if ((imm & 0x2000000) != 0) imm -= 0x4000000;
  return static_cast<uint64_t>(static_cast<int64_t>(at) + imm * 4);
}

constexpr uint32_t kA64Nop = 0xd503201f;

// Is `name` a defined global of this type, the same symbol through both hashes?
bool Exported(const Image& image, const char* name, unsigned type, const Elf64_Sym** out) {
  const Elf64_Sym* gnu = LookupGnu(image, name);
  const Elf64_Sym* sysv = LookupSysv(image, name);
  *out = gnu;
  return gnu != nullptr && gnu == sysv && ELF64_ST_BIND(gnu->st_info) == STB_GLOBAL &&
         ELF64_ST_TYPE(gnu->st_info) == type;
}

bool Absent(const Image& image, const char* name) {
  return LookupGnu(image, name) == nullptr && LookupSysv(image, name) == nullptr;
}

// One R_AARCH64_GLOB_DAT per import, to an undefined global function, at a
// zeroed 8-byte slot in the read-write segment.
bool GotRelocations(const Image& image, size_t imports) {
  if (image.nrela != imports) return false;
  for (size_t i = 0; i < image.nrela; ++i) {
    const Elf64_Rela& r = image.rela[i];
    const size_t sym = ELF64_R_SYM(r.r_info);
    if (sym == 0 || sym >= image.nsyms) return false;
    const Elf64_Sym& s = image.syms[sym];
    if (ELF64_R_TYPE(r.r_info) != R_AARCH64_GLOB_DAT || s.st_shndx != SHN_UNDEF ||
        ELF64_ST_BIND(s.st_info) != STB_GLOBAL || ELF64_ST_TYPE(s.st_info) != STT_FUNC ||
        r.r_offset % 8 != 0 || !image.InSegment(r.r_offset, 8, PF_R | PF_W) || image.Quad(r.r_offset) != 0)
      return false;
  }
  return true;
}

void CheckArm64StandIns() {
  using namespace gesher::bionic;
  Check("NetEase Yidun's arm64 shell is recognized (libnesec.so)",
        IsArtHookingPacker("/data/app/x/lib/arm64/libnesec.so") &&
            !IsArtHookingPacker("/data/app/x/lib/arm64/libnesec2.so"));
  // Each shell's stand-in is for one machine: an x86-64 libnesec.so gets
  // none (ADR 0086), nor does an arm64 libexec*.so.
  Check("libnesec.so's stand-in is arm64 only, Ijiami's x86-64 only",
        ArtHookingPackerMachine("/data/app/x/lib/x86_64/libnesec.so") == EM_AARCH64 &&
            ArtHookingPackerMachine("C:\\a\\libexec_x86.so") == EM_X86_64 &&
            ArtHookingPackerMachine("/data/app/x/lib/arm64/libexec.so") == EM_X86_64 &&
            ArtHookingPackerMachine("/data/app/x/lib/arm64/libnesec2.so") == 0 &&
            ArtHookingPackerMachine(nullptr) == 0);

  const std::string version("999", 4);  // the 4 bytes of OatHeader::kOatVersion
  Image art, dexfile;
  const bool parsed = Parse(ArtShimImageArm64(version), &art) && Parse(DexFileShimImageArm64(), &dexfile);
  Check("arm64 stand-ins: both images parse (PT_DYNAMIC, DT_HASH, DT_GNU_HASH, DT_RELA)", parsed);
  if (!parsed) return;
  Check("arm64 stand-ins: the same bytes each time",
        ArtShimImageArm64(version) == art.bytes && DexFileShimImageArm64() == dexfile.bytes);
  Check("arm64 stand-ins: libart.so's bytes depend on the OAT version",
        ArtShimImageArm64(std::string("998", 4)) != art.bytes);

  for (const Image* image : {&art, &dexfile}) {
    const Elf64_Ehdr& eh = *image->eh;
    const std::string which = image == &art ? "libart.so" : "libdexfile.so";
    Check(("arm64 " + which + ": ELF64 LSB AArch64 ET_DYN").c_str(),
          std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_ident[EI_CLASS] == ELFCLASS64 &&
              eh.e_ident[EI_DATA] == ELFDATA2LSB && eh.e_type == ET_DYN && eh.e_machine == EM_AARCH64,
          std::to_string(eh.e_machine));
    Check(("arm64 " + which + ": a read-execute PT_LOAD from offset 0, then a read-write one").c_str(),
          image->load.size() == 2 && image->load[0].p_offset == 0 && image->load[0].p_vaddr == 0 &&
              image->load[0].p_flags == (PF_R | PF_X) && image->load[1].p_flags == (PF_R | PF_W) &&
              image->load[0].p_filesz <= image->load[1].p_offset &&
              image->load[1].p_offset + image->load[1].p_filesz <= image->bytes.size());
    Check(("arm64 " + which + ": DT_SONAME").c_str(), which == image->soname, image->soname);
  }

  // libart.so: LoadClass, the Runtime functions, kOatVersion and instance_.
  const Elf64_Sym* load_class = nullptr;
  bool functions = Exported(art, kArtLoadClassSymbol, STT_FUNC, &load_class) &&
                   art.InSegment(load_class->st_value, load_class->st_size, PF_R | PF_X);
  bool prologue = functions && art.Word(load_class->st_value) == 0xa9bf7bfdu &&  // stp x29, x30
                  art.Word(load_class->st_value + 4) == 0x910003fdu;               // mov x29, sp
  for (int i = 2; prologue && i < 6; ++i) prologue = art.Word(load_class->st_value + i * 4) == kA64Nop;
  const char* const runtime_functions[][2] = {
      {kArtDisableVerifierSymbol, kArtShimDisableVerifierImport},
      {kArtIsVerificationEnabledSymbol, kArtShimIsVerificationEnabledImport},
      {kArtIsVerificationSoftFailSymbol, kArtShimIsVerificationSoftFailImport}};
  bool forwarders = true;
  for (const auto& f : runtime_functions) {
    const Elf64_Sym* s = nullptr;
    if (!Exported(art, f[0], STT_FUNC, &s) || !art.InSegment(s->st_value, s->st_size, PF_R | PF_X)) {
      functions = forwarders = false;
      continue;
    }
    // Six NOPs a hook can displace, LDR X16 from this function's own GOT
    // slot, BR X16.
    bool nops = true;
    for (int i = 0; i < 6; ++i) nops = nops && art.Word(s->st_value + i * 4) == kA64Nop;
    const uint64_t slot = GotSlotOf(art, f[1]);
    forwarders = forwarders && nops && slot != 0 && LdrX16Target(art, s->st_value + 24) == slot &&
                 art.Word(s->st_value + 28) == 0xd61f0200u;  // br x16
  }
  Check("arm64 libart.so: LoadClass and the Runtime functions through DT_GNU_HASH and DT_HASH",
        functions);
  Check("arm64 libart.so: LoadClass is a frame record and NOPs (nothing PC-relative to displace)",
        prologue);
  Check("arm64 libart.so: each Runtime function tail-calls through its own import's GOT slot",
        forwarders);
  const Elf64_Sym* oat = nullptr;
  Check("arm64 libart.so: OatHeader::kOatVersion holds the given version, read-only",
        Exported(art, kArtOatVersionSymbol, STT_OBJECT, &oat) && oat->st_size == 4 &&
            art.InSegment(oat->st_value, 4, PF_R | PF_X) &&
            std::memcmp(art.bytes.data() + oat->st_value, version.data(), 4) == 0);
  const Elf64_Sym* instance = nullptr;
  Check("arm64 libart.so: Runtime::instance_ is a writable pointer, null until loaded",
        Exported(art, kArtRuntimeInstanceSymbol, STT_OBJECT, &instance) && instance->st_size == 8 &&
            art.InSegment(instance->st_value, 8, PF_R | PF_W) && art.Quad(instance->st_value) == 0);
  Check("arm64 libart.so: three R_AARCH64_GLOB_DAT GOT slots, one per Runtime import",
        GotRelocations(art, 3) && GotSlotOf(art, kArtShimDisableVerifierImport) != 0 &&
            GotSlotOf(art, kArtShimIsVerificationEnabledImport) != 0 &&
            GotSlotOf(art, kArtShimIsVerificationSoftFailImport) != 0);
  Check("arm64 libart.so: no DexFileLoader::OpenCommon (libdexfile.so's on a device)",
        Absent(art, kArtOpenCommonSymbol) && Absent(art, kArtOpenCommonEntrySymbol) &&
            GotSlotOf(art, kArtShimOpenCommonImport) == 0);

  // libdexfile.so: OpenCommon and its entry, nothing of libart.so's.
  const Elf64_Sym* open_common = nullptr;
  const Elf64_Sym* entry = nullptr;
  const bool dex_exports = Exported(dexfile, kArtOpenCommonSymbol, STT_FUNC, &open_common) &&
                           Exported(dexfile, kArtOpenCommonEntrySymbol, STT_FUNC, &entry) &&
                           dexfile.InSegment(open_common->st_value, open_common->st_size, PF_R | PF_X) &&
                           dexfile.InSegment(entry->st_value, entry->st_size, PF_R | PF_X);
  Check("arm64 libdexfile.so: OpenCommon and its entry through DT_GNU_HASH and DT_HASH", dex_exports);
  bool libart_absent = true;
  for (const char* name : {kArtLoadClassSymbol, kArtDisableVerifierSymbol, kArtIsVerificationEnabledSymbol,
                           kArtIsVerificationSoftFailSymbol, kArtOatVersionSymbol, kArtRuntimeInstanceSymbol}) {
    libart_absent = libart_absent && Absent(dexfile, name);
  }
  Check("arm64 libdexfile.so: none of libart.so's symbols", libart_absent);
  const uint64_t oc_slot = GotSlotOf(dexfile, kArtShimOpenCommonImport);
  Check("arm64 libdexfile.so: one R_AARCH64_GLOB_DAT GOT slot, for ART's OpenCommon",
        GotRelocations(dexfile, 1) && oc_slot != 0);
  if (!dex_exports) return;
  bool calls_art = true;
  for (int i = 0; i < 6; ++i) calls_art = calls_art && dexfile.Word(open_common->st_value + i * 4) == kA64Nop;
  bool through_got = false;
  for (uint64_t at = open_common->st_value; at + 4 < open_common->st_value + open_common->st_size; at += 4) {
    if (LdrX16Target(dexfile, at) == oc_slot && dexfile.Word(at + 4) == 0xd63f0200u) through_got = true;  // blr x16
  }
  Check("arm64 libdexfile.so: OpenCommon is six NOPs, then a call to ART's through its GOT slot",
        calls_art && through_got);
  bool entry_calls = false;
  for (uint64_t at = entry->st_value; at < entry->st_value + entry->st_size; at += 4) {
    if (BlTarget(dexfile, at) == open_common->st_value) entry_calls = true;
  }
  bool result_in_x8 = false;
  for (uint64_t at = entry->st_value; at < entry->st_value + entry->st_size; at += 4) {
    if (dexfile.Word(at) == 0xaa0003e8u) result_in_x8 = true;  // mov x8, x0
  }
  Check("arm64 libdexfile.so: the entry ART calls makes the device's call (result in X8, BL OpenCommon)",
        entry_calls && result_in_x8);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <art_hook_test.so> <scratch directory>\n", argv[0]);
    return 2;
  }
  gesher::bionic::InstallBionicShim();
  CheckArm64StandIns();

  Check("a packer's shell is recognized (libexec_x86.so, libexecmain.so)",
        gesher::bionic::IsArtHookingPacker("/data/app/x/lib/x86_64/libexec_x86.so") &&
            gesher::bionic::IsArtHookingPacker("C:\\a\\libexecmain.so") &&
            !gesher::bionic::IsArtHookingPacker("/data/app/x/lib/x86_64/libexecutor.so"));

  // Stand-in directories an earlier Gesher left: loading prunes this kind's
  // other images and the old 8-hex-digit names once they are a day old.
  namespace fs = std::filesystem;
  const fs::path scratch(argv[2]);
  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(48);
  struct Leftover { const char* name; bool old; bool pruned; };
  const Leftover leftovers[] = {
      {"0123abcd", true, true},                  // a 32-bit name from before kinds
      {"x86_64-0000000000000000", true, true},   // another x86-64 image
      {"x86_64-1111111111111111", false, false}, // recent: may be starting up
      {"arm64-0000000000000000", true, false},   // the other kind's
      {"notes", true, false},                    // not a stand-in directory
  };
  for (const Leftover& leftover : leftovers) {
    const fs::path sub = scratch / leftover.name;
    fs::create_directories(sub);
    std::ofstream(sub / "libart.so") << "stale";
    std::error_code ec;
    if (leftover.old) fs::last_write_time(sub, old_time, ec);
    Check(("leftover " + std::string(leftover.name) + " is set up").c_str(), !ec, ec.message());
  }

  std::string path, error;
  void* entry = gesher::bionic::LoadArtShim(argv[2], &path, &error);
  Check("the stand-in libart.so loads", entry != nullptr, path + error);
  if (entry == nullptr) return 1;
  const std::string loaded_dir = fs::path(path).parent_path().filename().string();
  Check("its directory is named x86_64-<64-bit hash>",
        loaded_dir.size() == 23 && loaded_dir.compare(0, 7, "x86_64-") == 0, loaded_dir);
  for (const Leftover& leftover : leftovers) {
    Check(("leftover " + std::string(leftover.name) + (leftover.pruned ? " is pruned" : " stays"))
              .c_str(),
          fs::exists(scratch / leftover.name) != leftover.pruned);
  }
  const size_t slash = path.find_last_of('/');
  Check("its file is named libart.so",
        slash != std::string::npos && path.substr(slash) == "/libart.so", path);
  std::string again_path, again_error;
  Check("a second load returns the same entry",
        gesher::bionic::LoadArtShim(argv[2], &again_path, &again_error) == entry);

  std::string hook_error;
  gesher::elf::Library* hook = gesher::elf::Load(argv[1], &hook_error);
  Check("art_hook_test.so loads", hook != nullptr, hook_error);
  if (hook == nullptr) return 1;
  auto install = reinterpret_cast<int(SYSV*)(void**, void**, void**)>(
      gesher::elf::Lookup(hook, "gesher_art_hook_install"));
  auto calls = reinterpret_cast<int(SYSV*)()>(gesher::elf::Lookup(hook, "gesher_art_hook_calls"));
  auto returned =
      reinterpret_cast<int(SYSV*)()>(gesher::elf::Lookup(hook, "gesher_art_hook_original_returned"));
  auto last_begin =
      reinterpret_cast<uintptr_t(SYSV*)()>(gesher::elf::Lookup(hook, "gesher_art_hook_last_begin"));
  auto last_off = reinterpret_cast<uint32_t(SYSV*)()>(
      gesher::elf::Lookup(hook, "gesher_art_hook_last_class_data_off"));
  auto last_klass =
      reinterpret_cast<uintptr_t(SYSV*)()>(gesher::elf::Lookup(hook, "gesher_art_hook_last_klass"));
  auto hooked_path =
      reinterpret_cast<const char*(SYSV*)()>(gesher::elf::Lookup(hook, "gesher_art_hook_path"));
  if (!install || !calls || !returned || !last_begin || !last_off || !last_klass || !hooked_path) {
    Check("art_hook_test.so's exports", false);
    return 1;
  }

  void* gnu = nullptr;
  void* sysv = nullptr;
  void* file = nullptr;
  const int installed = install(&gnu, &sysv, &file);
  Check("/proc/self/maps names the stand-in at its base (readable, ELF header)",
        installed != -1, hooked_path());
  Check("the maps line is the stand-in's file", path == hooked_path(), hooked_path());
  Check("LoadClass through DT_GNU_HASH", gnu == entry, Hex(gnu) + " vs " + Hex(entry));
  Check("LoadClass through DT_HASH", sysv == entry, Hex(sysv));
  Check("LoadClass through the file's .symtab", file == entry, Hex(file));
  Check("the inline hook is installed (prologue as expected, writable, restored)", installed == 0,
        std::to_string(installed));

  // ART's call (patches/art/0800): this, Thread*, &dex_file, &class_def, Handle.
  uint8_t dex_bytes[0x70] = {'d', 'e', 'x', '\n', '0', '3', '9', 0};
  FakeDexFile dex = {nullptr, dex_bytes};
  uint8_t class_def[0x20] = {};
  const uint32_t class_data_off = 0x1234;
  std::memcpy(class_def + 0x18, &class_data_off, sizeof(class_data_off));
  using LoadClass = void(SYSV*)(void*, void*, const void*, const void*, uintptr_t);
  for (uintptr_t klass = 0x100; klass < 0x103; ++klass) {
    reinterpret_cast<LoadClass>(entry)(reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20),
                                       &dex, class_def, klass);
  }
  Check("ART's calls reach the hook", calls() == 3, std::to_string(calls()));
  Check("the hook's original (trampoline) returns", returned() == 3, std::to_string(returned()));
  Check("the hook reads DexFile::begin_ at offset 8",
        last_begin() == reinterpret_cast<uintptr_t>(dex_bytes));
  Check("the hook reads ClassDef::class_data_off_ at 0x18", last_off() == class_data_off);
  Check("the Handle arrives by value in the fifth argument", last_klass() == 0x102);

  std::printf(g_failures == 0 ? "art_shim_test all passed\n" : "art_shim_test: %d failed\n",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
