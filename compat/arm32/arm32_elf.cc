// gesher: independent ELF32 ARM parsing and relocation in owned guest memory.
// Format/relocation contracts: Arm AAELF32 and Android Bionic's
// linker/linker_relocate.cpp. This is an independent implementation.
#include <gesher/arm32_elf.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace gesher::arm32 {
namespace {

constexpr uint64_t kAddressLimit = uint64_t{1} << 32;
constexpr size_t kMaxNameLength = 4096;
constexpr uint32_t kPtLoad = 1, kPtDynamic = 2, kPtInterp = 3, kPtTls = 7;
constexpr uint32_t kPtGnuRelro = 0x6474e552;
constexpr uint32_t kDtNeeded = 1, kDtPltRelSz = 2, kDtHash = 4, kDtStrtab = 5,
                   kDtSymtab = 6, kDtStrSz = 10, kDtSymEnt = 11, kDtInit = 12,
                   kDtFini = 13, kDtSoname = 14, kDtRel = 17, kDtRelSz = 18,
                   kDtRelEnt = 19, kDtPltRel = 20, kDtJmpRel = 23,
                   kDtInitArray = 25, kDtFiniArray = 26, kDtInitArraySz = 27,
                   kDtFiniArraySz = 28, kDtFlags = 30,
                   kDtGnuHash = 0x6ffffef5, kDtRelCount = 0x6ffffffa,
                   kDtRelr = 36, kDtRelrSz = 35, kDtRelrEnt = 37,
                   kDtAndroidRelr = 0x6fffe000, kDtAndroidRelrSz = 0x6fffe001,
                   kDtAndroidRelrEnt = 0x6fffe003, kDtAndroidRelrCount = 0x6fffe005,
                   kDtAndroidRel = 0x6000000f, kDtAndroidRelSz = 0x60000010,
                   kDtAndroidRela = 0x60000011, kDtAndroidRelaSz = 0x60000012,
                   kDtFlags1 = 0x6ffffffb;
constexpr uint8_t kStbGlobal = 1, kStbWeak = 2, kSttFunc = 2,
                  kSttTls = 6, kSttIfunc = 10;
constexpr uint16_t kShnAbs = 0xfff1;

uint16_t U16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (uint16_t{p[1]} << 8));
}
uint32_t U32(const uint8_t* p) {
  return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) |
         (uint32_t{p[3]} << 24);
}
void Put32(uint8_t* p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
bool Fits(uint64_t offset, uint64_t count, uint64_t limit) {
  return offset <= limit && count <= limit - offset;
}
bool PowerOfTwo(uint32_t n) { return n != 0 && (n & (n - 1)) == 0; }

struct ProgramHeader {
  uint32_t type, offset, vaddr, filesz, memsz, flags, align;
};
struct Symbol {
  uint32_t name, value, size;
  uint8_t binding, type, visibility;
  uint16_t section;
};
struct Relocation {
  uint32_t offset, symbol, type;
};

const char* RelocationName(uint32_t type) {
  switch (type) {
    case 0: return "R_ARM_NONE";
    case 2: return "R_ARM_ABS32";
    case 3: return "R_ARM_REL32";
    case 13: return "R_ARM_TLS_DESC";
    case 17: return "R_ARM_TLS_DTPMOD32";
    case 18: return "R_ARM_TLS_DTPOFF32";
    case 19: return "R_ARM_TLS_TPOFF32";
    case 20: return "R_ARM_COPY";
    case 21: return "R_ARM_GLOB_DAT";
    case 22: return "R_ARM_JUMP_SLOT";
    case 23: return "R_ARM_RELATIVE";
    case 160: return "R_ARM_IRELATIVE";
    default: return "unknown ARM relocation";
  }
}

class Loader {
 public:
  Loader(const void* data, size_t size, const ElfLoadOptions& options, std::string* error)
      : file_(static_cast<const uint8_t*>(data)), file_size_(size), options_(options),
        error_(error) {}

  bool Run(ElfImage* result) {
    if (!Headers() || !MapImage() || !Dynamic() || !Symbols() || !Relocations() ||
        !Relocate() || !InitFini()) return false;
    *result = std::move(image_);
    if (error_ != nullptr) error_->clear();
    return true;
  }

 private:
  const uint8_t* file_;
  size_t file_size_;
  const ElfLoadOptions& options_;
  std::string* error_;
  ElfImage image_;
  std::vector<ProgramHeader> loads_;
  std::vector<ProgramHeader> relro_;
  ProgramHeader dynamic_{};
  bool has_dynamic_ = false;
  uint32_t minimum_ = 0;
  std::map<uint32_t, uint32_t> tags_;
  std::vector<uint32_t> needed_;
  std::vector<Symbol> symbols_;
  std::vector<Relocation> relocations_;
  std::map<uint32_t, std::string> names_;
  size_t name_bytes_ = 0;

  bool Fail(std::string message) {
    if (error_ != nullptr) *error_ = "ARM32 ELF: " + message;
    return false;
  }
  bool Has(uint32_t tag) const { return tags_.find(tag) != tags_.end(); }
  uint32_t Tag(uint32_t tag) const {
    const auto it = tags_.find(tag);
    return it == tags_.end() ? 0 : it->second;
  }
  bool Guest(uint32_t vaddr, uint32_t* address) {
    const uint64_t value = uint64_t{vaddr} + options_.load_bias;
    if (value >= kAddressLimit) return Fail("guest address overflow");
    *address = static_cast<uint32_t>(value);
    return true;
  }
  const ProgramHeader* Covering(uint64_t vaddr, uint64_t length) const {
    for (const auto& ph : loads_) {
      if (vaddr >= ph.vaddr && Fits(vaddr - ph.vaddr, length, ph.memsz)) return &ph;
    }
    return nullptr;
  }
  const uint8_t* At(uint64_t vaddr, uint64_t length) const {
    if (Covering(vaddr, length) == nullptr) return nullptr;
    return image_.bytes.data() + static_cast<size_t>(vaddr - minimum_);
  }
  const uint8_t* MetadataAt(uint64_t vaddr, uint64_t length) const {
    const auto* ph = Covering(vaddr, length);
    if (ph == nullptr || !Fits(vaddr - ph->vaddr, length, ph->filesz)) return nullptr;
    return image_.bytes.data() + static_cast<size_t>(vaddr - minimum_);
  }
  bool Executable(uint32_t address) const {
    const uint32_t instruction = address & ~uint32_t{1};
    if (instruction < options_.load_bias) return false;
    const auto* ph = Covering(instruction - options_.load_bias, (address & 1) ? 2 : 4);
    return ph != nullptr && (ph->flags & 1) != 0 &&
           ((address & 1) != 0 || (address & 3) == 0);
  }

  bool Headers() {
    if (file_ == nullptr || file_size_ < 52) return Fail("truncated ELF32 header");
    if (std::memcmp(file_, "\177ELF", 4) != 0 || file_[4] != 1 || file_[5] != 1 ||
        file_[6] != 1 || U16(file_ + 16) != 3 || U16(file_ + 18) != 40 ||
        U32(file_ + 20) != 1 || U16(file_ + 40) != 52) {
      return Fail("expected a little-endian ELF32 ARM ET_DYN image");
    }
    if ((file_[7] != 0 && file_[7] != 3) || file_[8] != 0) {
      return Fail("unsupported OSABI or ABI version (FDPIC is not supported)");
    }
    const uint32_t flags = U32(file_ + 36);
    if ((flags & 0xff000000) != 0x05000000 || (flags & 0x00800400) != 0) {
      return Fail("requires ARM EABI5 softfp; hard-float and BE8 are unsupported");
    }
    const uint32_t phoff = U32(file_ + 28);
    const uint16_t phnum = U16(file_ + 44);
    if (U16(file_ + 42) != 32 || phnum == 0 || phnum == 0xffff ||
        !Fits(phoff, uint64_t{phnum} * 32, file_size_)) {
      return Fail("invalid or truncated program-header table");
    }
    for (size_t i = 0; i < phnum; ++i) {
      const uint8_t* p = file_ + phoff + i * 32;
      ProgramHeader ph{U32(p), U32(p + 4), U32(p + 8), U32(p + 16), U32(p + 20),
                       U32(p + 24), U32(p + 28)};
      if (ph.type == kPtTls) return Fail("PT_TLS requires the unimplemented ARM32 TLS runtime");
      if (ph.type == kPtInterp) return Fail("PT_INTERP executables are unsupported");
      if (ph.type != kPtLoad && ph.type != kPtDynamic && ph.type != kPtGnuRelro) continue;
      if (ph.filesz > ph.memsz || !Fits(ph.offset, ph.filesz, file_size_) ||
          !Fits(ph.vaddr, ph.memsz, kAddressLimit)) {
        return Fail("program segment exceeds its file or guest address range");
      }
      if (ph.type == kPtDynamic) {
        if (has_dynamic_) return Fail("multiple PT_DYNAMIC segments");
        dynamic_ = ph;
        has_dynamic_ = true;
        continue;
      }
      if (ph.type == kPtGnuRelro) {
        if (ph.memsz != 0) relro_.push_back(ph);
        continue;
      }
      if (ph.align > 1 && (!PowerOfTwo(ph.align) ||
          ((ph.vaddr - ph.offset) & (ph.align - 1)) != 0 ||
          (options_.load_bias & (ph.align - 1)) != 0)) {
        return Fail("invalid PT_LOAD alignment or incompatible load bias");
      }
      if ((ph.flags & ~uint32_t{7}) != 0) return Fail("unsupported PT_LOAD flags");
      if (ph.memsz != 0) loads_.push_back(ph);
    }
    if (loads_.empty()) return Fail("no nonempty PT_LOAD segment");
    std::sort(loads_.begin(), loads_.end(), [](const auto& a, const auto& b) {
      return a.vaddr < b.vaddr;
    });
    minimum_ = loads_.front().vaddr;
    uint64_t end = minimum_;
    for (const auto& ph : loads_) {
      if (ph.vaddr < end) return Fail("overlapping PT_LOAD byte ranges");
      end = uint64_t{ph.vaddr} + ph.memsz;
      if (end + options_.load_bias > kAddressLimit) return Fail("loaded segment address overflow");
    }
    if (end - minimum_ > options_.max_image_size || end - minimum_ > SIZE_MAX) {
      return Fail("image span exceeds configured memory limit");
    }
    for (const auto& ph : relro_) {
      uint32_t address;
      if (Covering(ph.vaddr, ph.memsz) == nullptr || !Guest(ph.vaddr, &address)) {
        return Fail("PT_GNU_RELRO lies outside PT_LOAD");
      }
      image_.relro.push_back({address, ph.memsz});
    }
    image_.load_bias = options_.load_bias;
    if (!Guest(minimum_, &image_.guest_base)) return false;
    image_.bytes.resize(static_cast<size_t>(end - minimum_), 0);
    const uint32_t entry = U32(file_ + 24);
    if (entry != 0 && (!Guest(entry, &image_.entry) || !Executable(image_.entry))) {
      return Fail("entry point is outside executable load segments or misaligned");
    }
    return true;
  }

  bool MapImage() {
    for (const auto& ph : loads_) {
      std::memcpy(image_.bytes.data() + (ph.vaddr - minimum_), file_ + ph.offset, ph.filesz);
      uint32_t address;
      if (!Guest(ph.vaddr, &address)) return false;
      image_.segments.push_back({address, ph.memsz, ph.flags});
    }
    return true;
  }

  bool Name(uint32_t offset, std::string* out) {
    const auto old = names_.find(offset);
    if (old != names_.end()) { *out = old->second; return true; }
    const uint32_t length = Tag(kDtStrSz);
    if (!Has(kDtStrtab) || offset >= length) return Fail("string-table offset out of bounds");
    const uint8_t* p = MetadataAt(uint64_t{Tag(kDtStrtab)} + offset, length - offset);
    if (p == nullptr) return Fail("string table is outside PT_LOAD");
    const size_t available = std::min<size_t>(length - offset, kMaxNameLength + 1);
    const auto* terminator = static_cast<const uint8_t*>(std::memchr(p, 0, available));
    if (terminator == nullptr) return Fail("unterminated or excessive dynamic string");
    const size_t count = static_cast<size_t>(terminator - p);
    if (count + 1 > options_.max_image_size - std::min(name_bytes_, options_.max_image_size)) {
      return Fail("dynamic strings exceed configured memory limit");
    }
    name_bytes_ += count + 1;
    *out = std::string(reinterpret_cast<const char*>(p), count);
    names_.emplace(offset, *out);
    return true;
  }

  bool Dynamic() {
    if (!has_dynamic_) return true;
    if (dynamic_.filesz == 0 || (dynamic_.filesz % 8) != 0 ||
        At(dynamic_.vaddr, dynamic_.filesz) == nullptr) return Fail("invalid PT_DYNAMIC range");
    // The dynamic segment must describe the bytes actually copied from the
    // file, not a conflicting file offset or zero-filled BSS.
    const auto* ph = Covering(dynamic_.vaddr, dynamic_.filesz);
    const uint64_t delta = uint64_t{dynamic_.vaddr} - ph->vaddr;
    if (!Fits(delta, dynamic_.filesz, ph->filesz) ||
        uint64_t{ph->offset} + delta != dynamic_.offset) {
      return Fail("PT_DYNAMIC does not match its file-backed PT_LOAD bytes");
    }
    const uint8_t* p = At(dynamic_.vaddr, dynamic_.filesz);
    bool terminated = false;
    for (size_t at = 0; at < dynamic_.filesz; at += 8) {
      const uint32_t tag = U32(p + at), value = U32(p + at + 4);
      if (tag == 0) { terminated = true; break; }
      if (tag == kDtNeeded) { needed_.push_back(value); continue; }
      switch (tag) {
        case kDtPltRelSz: case 3: case kDtHash: case kDtStrtab: case kDtSymtab:
        case kDtStrSz: case kDtSymEnt: case kDtInit: case kDtFini: case kDtSoname:
        case 15: case 16: case kDtRel: case kDtRelSz: case kDtRelEnt: case kDtPltRel:
        case 21: case kDtJmpRel: case 24: case kDtInitArray: case kDtFiniArray:
        case kDtInitArraySz: case kDtFiniArraySz: case 29: case kDtFlags:
        case kDtGnuHash: case kDtRelCount: case kDtFlags1:
        case kDtRelr: case kDtRelrSz: case kDtRelrEnt:
        case kDtAndroidRelr: case kDtAndroidRelrSz: case kDtAndroidRelrEnt:
        case kDtAndroidRelrCount: case kDtAndroidRel: case kDtAndroidRelSz:
          break;
        case 7: case 8: case 9: return Fail("DT_RELA relocations are unsupported");
        case 22: return Fail("DT_TEXTREL is unsupported");
        case 32: case 33: return Fail("DT_PREINIT_ARRAY is unsupported for shared libraries");
                case kDtAndroidRela: case kDtAndroidRelaSz:
          return Fail("ELF32 Android packed RELA relocations are unsupported");
        case 0x6ffffff0: case 0x6ffffffc: case 0x6ffffffd: case 0x6ffffffe: case 0x6fffffff:
          return Fail("versioned dynamic symbols are unsupported");
        default: return Fail("unsupported dynamic tag " + std::to_string(tag));
      }
      if (!tags_.emplace(tag, value).second) return Fail("duplicate dynamic tag " + std::to_string(tag));
    }
    if (!terminated) return Fail("PT_DYNAMIC has no bounded DT_NULL terminator");
    // DF_SYMBOLIC / DF_BIND_NOW and DF_1_NOW are harmless in this eager,
    // single-image loader. Other flags require behavior not implemented here.
    if ((Tag(kDtFlags) & ~uint32_t{2 | 8}) != 0 || (Tag(kDtFlags1) & ~uint32_t{1}) != 0) {
      return Fail("unsupported dynamic flags (including TLS or text relocations)");
    }
    if (Has(kDtStrtab) != Has(kDtStrSz) || (Has(kDtStrtab) &&
        (Tag(kDtStrSz) == 0 || MetadataAt(Tag(kDtStrtab), Tag(kDtStrSz)) == nullptr))) {
      return Fail("invalid dynamic string-table range");
    }
    if (Has(kDtStrtab)) {
      const uint8_t* strings = MetadataAt(Tag(kDtStrtab), Tag(kDtStrSz));
      if (strings[0] != 0 || strings[Tag(kDtStrSz) - 1] != 0) {
        return Fail("dynamic string table must begin and end with NUL");
      }
    }
    for (uint32_t needed : needed_) {
      std::string name;
      if (!Name(needed, &name)) return false;
      if (!options_.resolve_dependency)
        return Fail("DT_NEEDED '" + name + "' requires the unimplemented dependency loader");
      bool accepted = false;
      try {
        accepted = options_.resolve_dependency(name);
      } catch (...) {
        return Fail("dependency resolver threw an exception");
      }
      if (!accepted) return Fail("unresolved dependency '" + name + "'");
      image_.dependencies.push_back(std::move(name));
    }
    if (Has(kDtSoname) && !Name(Tag(kDtSoname), &image_.soname)) return false;
    for (uint32_t tag : {uint32_t{15}, uint32_t{29}}) {
      if (Has(tag)) { std::string ignored; if (!Name(Tag(tag), &ignored)) return false; }
    }
    return true;
  }

  bool SymbolCount(size_t* count) {
    *count = 0;
    if (Has(kDtHash)) {
      const uint32_t address = Tag(kDtHash);
      const uint8_t* header = MetadataAt(address, 8);
      if (header == nullptr) return Fail("truncated DT_HASH header");
      const uint32_t buckets = U32(header), chains = U32(header + 4);
      const uint64_t words = uint64_t{buckets} + chains;
      const uint8_t* table = MetadataAt(uint64_t{address} + 8, words * 4);
      if (buckets == 0 || chains == 0 || chains > options_.max_symbols || table == nullptr) {
        return Fail("invalid DT_HASH size");
      }
      for (uint64_t i = 0; i < words; ++i) {
        if (U32(table + i * 4) >= chains) return Fail("DT_HASH symbol index out of bounds");
      }
      *count = chains;
    }
    if (Has(kDtGnuHash)) {
      const uint32_t address = Tag(kDtGnuHash);
      const uint8_t* header = MetadataAt(address, 16);
      if (header == nullptr) return Fail("truncated DT_GNU_HASH header");
      const uint32_t buckets = U32(header), first = U32(header + 4),
                     bloom = U32(header + 8), shift = U32(header + 12);
      const uint64_t bucket_address = uint64_t{address} + 16 + uint64_t{bloom} * 4;
      const uint8_t* bucket_table = MetadataAt(bucket_address, uint64_t{buckets} * 4);
      if (buckets == 0 || !PowerOfTwo(bloom) || shift >= 32 || first == 0 ||
          first > options_.max_symbols || MetadataAt(address, 16 + uint64_t{bloom} * 4) == nullptr ||
          bucket_table == nullptr) return Fail("invalid DT_GNU_HASH layout");
      uint32_t highest = 0;
      for (uint32_t i = 0; i < buckets; ++i) {
        const uint32_t symbol = U32(bucket_table + uint64_t{i} * 4);
        if (symbol != 0 && (symbol < first || symbol >= options_.max_symbols)) {
          return Fail("DT_GNU_HASH bucket index out of bounds");
        }
        highest = std::max(highest, symbol);
      }
      size_t gnu_count = first;
      if (highest != 0) {
        const uint64_t chain_address = bucket_address + uint64_t{buckets} * 4;
        uint32_t symbol = highest;
        for (;;) {
          if (symbol >= options_.max_symbols) return Fail("DT_GNU_HASH chain exceeds symbol limit");
          const uint8_t* chain = MetadataAt(chain_address + uint64_t{symbol - first} * 4, 4);
          if (chain == nullptr) return Fail("unterminated DT_GNU_HASH chain");
          ++symbol;
          if ((U32(chain) & 1) != 0) break;
        }
        gnu_count = symbol;
        if (MetadataAt(chain_address, uint64_t{gnu_count - first} * 4) == nullptr) {
          return Fail("DT_GNU_HASH chain range crosses an unmapped gap");
        }
      }
      if (*count != 0 && *count != gnu_count) return Fail("inconsistent dynamic hash symbol counts");
      *count = gnu_count;
    }
    return *count != 0 || Fail("DT_SYMTAB requires a bounded SysV or GNU hash table");
  }

  bool Symbols() {
    if (!Has(kDtSymtab)) {
      if (Has(kDtHash) || Has(kDtGnuHash) || Has(kDtSymEnt)) return Fail("missing DT_SYMTAB");
      return true;
    }
    if (!Has(kDtStrtab) || !Has(kDtSymEnt) || Tag(kDtSymEnt) != 16) {
      return Fail("invalid DT_SYMTAB entry size or missing string table");
    }
    size_t count;
    if (!SymbolCount(&count)) return false;
    const uint8_t* table = MetadataAt(Tag(kDtSymtab), uint64_t{count} * 16);
    if (table == nullptr) return Fail("dynamic symbol table exceeds PT_LOAD");
    symbols_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* p = table + i * 16;
      Symbol s{U32(p), U32(p + 4), U32(p + 8), static_cast<uint8_t>(p[12] >> 4),
               static_cast<uint8_t>(p[12] & 15), static_cast<uint8_t>(p[13] & 3), U16(p + 14)};
      if (s.type == kSttTls) return Fail("STT_TLS symbols require the unimplemented TLS runtime");
      if (s.type == kSttIfunc) return Fail("STT_GNU_IFUNC is unsupported");
      if (s.binding > kStbWeak || s.type > 4 || (p[13] & ~uint8_t{3}) != 0) {
        return Fail("unsupported dynamic symbol type or binding");
      }
      if (s.section >= 0xff00 && s.section != kShnAbs) return Fail("unsupported special symbol section");
      std::string name;
      if (!Name(s.name, &name)) return false;
      if (i == 0 && (s.name != 0 || s.value != 0 || s.size != 0 || p[12] != 0 ||
                     p[13] != 0 || s.section != 0)) return Fail("invalid null symbol");
      if (s.section == 0 && s.value != 0) return Fail("undefined symbol has a nonzero value");
      symbols_.push_back(s);
      if (s.section == 0) continue;
      uint32_t address = s.value;
      if (s.section != kShnAbs) {
        const uint32_t value = s.type == kSttFunc ? s.value & ~uint32_t{1} : s.value;
        // A zero-size end marker may equal the segment end. Function entry
        // points, however, must contain at least one complete instruction.
        if (Covering(value, s.size) == nullptr || !Guest(s.value, &address)) {
          return Fail("defined dynamic symbol is outside PT_LOAD");
        }
        if (s.type == kSttFunc && !Executable(address)) return Fail("function symbol is not executable");
      }
      if ((s.binding == kStbGlobal || s.binding == kStbWeak) &&
          (s.visibility == 0 || s.visibility == 3) && !name.empty()) {
        image_.symbols.push_back({std::move(name), address, s.size, s.binding, s.type});
      }
    }
    return true;
  }

  bool AppendRelocation(uint32_t offset, uint32_t info, bool plt) {
    Relocation r{offset, info >> 8, info & 255};
    if (r.type == 0) return true;
    if (r.type != 2 && r.type != 3 && r.type != 21 && r.type != 22 && r.type != 23)
      return Fail(std::string("unsupported ") + RelocationName(r.type) +
                  " (" + std::to_string(r.type) + ")");
    if (plt && r.type != 22) return Fail("DT_JMPREL contains a non-JUMP_SLOT relocation");
    if ((r.offset & 3) != 0 || At(r.offset, 4) == nullptr)
      return Fail("relocation target is unaligned or outside PT_LOAD");
    const auto* target = Covering(r.offset, 4);
    if ((target->flags & 2) == 0)
      return Fail("relocation target is not in a writable PT_LOAD segment");
    if (r.type == 23 && r.symbol != 0)
      return Fail("R_ARM_RELATIVE must not reference a symbol");
    if (r.type != 23 && r.symbol >= symbols_.size())
      return Fail("relocation symbol index out of bounds");
    if (relocations_.size() >= options_.max_relocations)
      return Fail("relocation limit exceeded");
    relocations_.push_back(r);
    return true;
  }

  bool RelocationTable(uint32_t address, uint32_t bytes, bool plt) {
    if (bytes % 8 != 0 || bytes / 8 > options_.max_relocations -
        std::min(relocations_.size(), options_.max_relocations))
      return Fail("invalid REL table size");
    if (bytes == 0) return true;
    const uint8_t* table = MetadataAt(address, bytes);
    if (table == nullptr) return Fail("REL table exceeds PT_LOAD");
    for (size_t i = 0; i < bytes / 8; ++i) {
      if (!AppendRelocation(U32(table + i * 8), U32(table + i * 8 + 4), plt))
        return false;
    }
    return true;
  }

  // Android APS2 encodes groups of ELF32 REL records as signed LEB128 values.
  // Decode a bounded stream, never reading past its declared DT_ANDROID_RELSZ.
  // AAPCS32 uses REL, not RELA: group addend flags and RELA tags fail closed.
  bool PackedRelocations() {
    if (Has(kDtAndroidRel) != Has(kDtAndroidRelSz))
      return Fail("incomplete Android packed REL description");
    if (!Has(kDtAndroidRel)) return true;
    const uint32_t size = Tag(kDtAndroidRelSz);
    const uint8_t* stream = MetadataAt(Tag(kDtAndroidRel), size);
    if (size < 4 || stream == nullptr || std::memcmp(stream, "APS2", 4) != 0)
      return Fail("invalid Android APS2 relocation header");
    size_t pos = 4;
    // The packed stream uses signed LEB128 even for the unsigned ELF32
    // fields. Preserve negative deltas for offset arithmetic; negative
    // relocation info is an allowed 32-bit bit pattern, not a host pointer.
    auto number = [&](int64_t* out) {
      uint64_t bits = 0;
      unsigned shift = 0;
      for (unsigned i = 0; i < 5; ++i) {
        if (pos >= size) return false;
        const uint8_t byte = stream[pos++];
        bits |= uint64_t{byte & 127} << shift;
        shift += 7;
        if ((byte & 128) != 0) continue;
        if ((byte & 64) != 0) bits |= ~uint64_t{0} << shift;
        const int64_t signed_bits = static_cast<int64_t>(bits);
        // A five-byte encoding may include high bits not representing an
        // ELF32 value. Reject them before any narrowing conversion.
        if (signed_bits < INT32_MIN || signed_bits > UINT32_MAX) return false;
        *out = signed_bits;
        return true;
      }
      return false;
    };
    int64_t count = 0, offset = 0;
    if (!number(&count) || !number(&offset) || count < 0 ||
        static_cast<uint64_t>(count) > options_.max_relocations ||
        offset < 0 || offset > UINT32_MAX)
      return Fail("invalid Android APS2 relocation count or initial offset");
    uint64_t remaining = static_cast<uint64_t>(count);
    int64_t next = offset;
    while (remaining != 0) {
      int64_t group = 0, flags = 0, delta = 0, info = 0;
      if (!number(&group) || !number(&flags) || group <= 0 ||
          static_cast<uint64_t>(group) > remaining || flags < 0)
        return Fail("invalid Android APS2 group size or flags");
      // Android ELF32 REL ignores GROUPED_BY_ADDEND without HAS_ADDEND;
      // the latter is invalid for a format without explicit addends.
      if ((flags & ~int64_t{7}) != 0)
        return Fail("Android APS2 REL group has invalid flags or addends");
      if ((flags & 2) != 0 && !number(&delta))
        return Fail("truncated Android APS2 group offset");
      if ((flags & 1) != 0 && !number(&info))
        return Fail("truncated Android APS2 group info");
      for (int64_t i = 0; i < group; ++i) {
        int64_t step = delta;
        if ((flags & 2) == 0 && !number(&step))
          return Fail("truncated Android APS2 relocation offset");
        next += step;
        if (next < 0 || next > UINT32_MAX)
          return Fail("Android APS2 relocation address overflow");
        int64_t record_info = info;
        if ((flags & 1) == 0 && !number(&record_info))
          return Fail("truncated Android APS2 relocation info");
        if (!AppendRelocation(static_cast<uint32_t>(next),
                              static_cast<uint32_t>(record_info), false))
          return false;
      }
      remaining -= static_cast<uint64_t>(group);
    }
    if (pos != size) return Fail("trailing Android APS2 relocation data");
    return true;
  }

  // ELF32 RELR: even entries identify a direct relative relocation; odd
  // entries describe the following 31 four-byte words. Validate the entire
  // expansion before publishing any writes, including overlaps with DT_REL.
  bool RelrTable(uint32_t address, uint32_t bytes) {
    if ((bytes & 3) != 0) return Fail("invalid RELR table size");
    if (bytes == 0) return true;
    const uint8_t* table = MetadataAt(address, bytes);
    if (table == nullptr) return Fail("RELR table exceeds PT_LOAD");
    uint64_t cursor = 0;
    bool have_base = false;
    auto add = [&](uint64_t offset) {
      if (offset > UINT32_MAX || (offset & 3) != 0 ||
          At(static_cast<uint32_t>(offset), 4) == nullptr)
        return Fail("RELR target is unaligned or outside PT_LOAD");
      const auto* segment = Covering(static_cast<uint32_t>(offset), 4);
      if (segment == nullptr || (segment->flags & 2) == 0)
        return Fail("RELR target is not in a writable PT_LOAD segment");
      if (relocations_.size() >= options_.max_relocations)
        return Fail("RELR expansion exceeds relocation limit");
      relocations_.push_back({static_cast<uint32_t>(offset), 0, 23});
      return true;
    };
    for (size_t index = 0; index < bytes / 4; ++index) {
      const uint32_t entry = U32(table + index * 4);
      if ((entry & 1) == 0) {
        if (!add(entry)) return false;
        cursor = uint64_t{entry} + 4;
        have_base = true;
      } else {
        if (!have_base) return Fail("RELR bitmap has no preceding address");
        for (uint32_t bit = 1; bit < 32; ++bit) {
          if ((entry & (uint32_t{1} << bit)) != 0 &&
              !add(cursor + uint64_t{bit - 1} * 4)) return false;
        }
        cursor += 31 * 4;
        if (cursor > kAddressLimit) return Fail("RELR bitmap address overflow");
      }
    }
    return true;
  }

  bool Relocations() {
    if (Has(kDtRel) != Has(kDtRelSz)) return Fail("incomplete DT_REL description");
    if ((Has(kDtRel) || Has(kDtRelEnt)) && Tag(kDtRelEnt) != 8) return Fail("DT_RELENT must be 8");
    if (Has(kDtRel) && !RelocationTable(Tag(kDtRel), Tag(kDtRelSz), false)) return false;
    if (Has(kDtJmpRel) != Has(kDtPltRelSz) || Has(kDtJmpRel) != Has(kDtPltRel) ||
        (Has(kDtJmpRel) && (!Has(kDtPltRel) || Tag(kDtPltRel) != kDtRel))) {
      return Fail("incomplete or non-REL PLT relocations");
    }
    if (Has(kDtJmpRel) && !RelocationTable(Tag(kDtJmpRel), Tag(kDtPltRelSz), true)) return false;
    // Both standardized and early Android tags use exactly the same 32-bit
    // encoding. Fail closed on duplicates, partial descriptions and bad sizes.
    const bool standard = Has(kDtRelr) || Has(kDtRelrSz) || Has(kDtRelrEnt);
    const bool android = Has(kDtAndroidRelr) || Has(kDtAndroidRelrSz) ||
                         Has(kDtAndroidRelrEnt);
    if (standard && android) return Fail("mixed RELR tag families");
    if (standard || android) {
      const uint32_t a = standard ? kDtRelr : kDtAndroidRelr;
      const uint32_t s = standard ? kDtRelrSz : kDtAndroidRelrSz;
      const uint32_t e = standard ? kDtRelrEnt : kDtAndroidRelrEnt;
      if (!Has(a) || !Has(s) || !Has(e)) return Fail("incomplete RELR description");
      if (Tag(e) != 4) return Fail("RELR entry size must be 4");
      if (!RelrTable(Tag(a), Tag(s))) return false;
    }
    if (Has(kDtAndroidRelrCount) && !android)
      return Fail("Android RELR count without RELR table");
    if (!PackedRelocations()) return false;
    if (Has(kDtRelCount)) {
      const uint32_t count = Tag(kDtRelCount);
      if (!Has(kDtRel) || count > Tag(kDtRelSz) / 8) return Fail("invalid DT_RELCOUNT");
      const uint8_t* table = MetadataAt(Tag(kDtRel), Tag(kDtRelSz));
      for (size_t i = 0; i < count; ++i) {
        if (U32(table + i * 8 + 4) != 23) return Fail("DT_RELCOUNT does not describe relative relocations");
      }
    }
    std::set<uint32_t> targets;
    for (const auto& r : relocations_) {
      if (!targets.insert(r.offset).second) return Fail("duplicate relocation target");
    }
    // Validate init/fini table extents before any relocation writes.
    for (auto pair : {std::pair{kDtInitArray, kDtInitArraySz}, std::pair{kDtFiniArray, kDtFiniArraySz}}) {
      if (Has(pair.first) != Has(pair.second) || Tag(pair.second) % 4 != 0 ||
          (Tag(pair.second) != 0 && At(Tag(pair.first), Tag(pair.second)) == nullptr)) {
        return Fail("invalid init/fini array range");
      }
    }
    return true;
  }

  bool Relocate() {
    struct Resolved { uint32_t address; bool weak_missing; };
    std::map<uint32_t, Resolved> resolved;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    writes.reserve(relocations_.size());
    for (const auto& r : relocations_) {
      const uint32_t addend = U32(At(r.offset, 4));
      uint32_t value;
      if (r.type == 23) {
        // Arithmetic for these word relocations is modulo 2^32. Image and
        // metadata address calculations, unlike relocation data, never wrap.
        value = options_.load_bias + addend;
      } else {
        auto found = resolved.find(r.symbol);
        if (found == resolved.end()) {
          const Symbol& s = symbols_[r.symbol];
          Resolved result{0, false};
          if (s.section != 0) {
            result.address = s.value;
            if (s.section != kShnAbs && !Guest(s.value, &result.address)) return false;
          } else if (r.symbol != 0) {
            std::string name;
            if (!Name(s.name, &name)) return false;
            bool imported = false;
            try {
              imported = options_.resolve_import && options_.resolve_import(name, &result.address);
            } catch (...) {
              return Fail("import resolver threw an exception");
            }
            if (!imported) {
              if (s.binding != kStbWeak) return Fail("unresolved import '" + name + "'");
              result.address = 0;
              result.weak_missing = true;
            }
          }
          found = resolved.emplace(r.symbol, result).first;
        }
        uint32_t symbol = found->second.address;
        const uint32_t place = options_.load_bias + r.offset;
        if (r.type == 3 && found->second.weak_missing) symbol = place;
        if (r.type == 21 || r.type == 22) {
          // Match Bionic: REL GLOB_DAT/JUMP_SLOT ignore the stored addend.
          value = symbol;
        } else {
          const bool thumb = symbols_[r.symbol].type == kSttFunc && (symbol & 1) != 0;
          value = ((thumb ? symbol & ~uint32_t{1} : symbol) + addend) | uint32_t{thumb};
          if (r.type == 3) value -= place;
        }
      }
      writes.emplace_back(r.offset, value);
    }
    // No malformed/unsupported relocation can partially modify the image.
    for (const auto& [offset, value] : writes) Put32(image_.bytes.data() + (offset - minimum_), value);
    return true;
  }

  bool AddInit(uint32_t address, std::vector<uint32_t>* out) {
    if (address == 0 || address == UINT32_MAX) return true;
    if (!Executable(address)) return Fail("constructor/finalizer is not executable guest code");
    out->push_back(address);
    return true;
  }
  bool InitFini() {
    if (Tag(kDtInit) != 0) {
      uint32_t address;
      if (!Guest(Tag(kDtInit), &address) || !AddInit(address, &image_.constructors)) return false;
    }
    const uint32_t init_size = Tag(kDtInitArraySz);
    const uint8_t* init = init_size != 0 ? At(Tag(kDtInitArray), init_size) : nullptr;
    for (size_t i = 0; i < init_size / 4; ++i) {
      if (!AddInit(U32(init + i * 4), &image_.constructors)) return false;
    }
    const uint32_t fini_size = Tag(kDtFiniArraySz);
    const uint8_t* fini = fini_size != 0 ? At(Tag(kDtFiniArray), fini_size) : nullptr;
    for (size_t i = fini_size / 4; i > 0; --i) {
      if (!AddInit(U32(fini + (i - 1) * 4), &image_.finalizers)) return false;
    }
    if (Tag(kDtFini) != 0) {
      uint32_t address;
      if (!Guest(Tag(kDtFini), &address) || !AddInit(address, &image_.finalizers)) return false;
    }
    return true;
  }
};

}  // namespace

bool ElfImage::Lookup(std::string_view name, uint32_t* address) const {
  const ElfSymbol* weak = nullptr;
  for (const auto& symbol : symbols) {
    if (symbol.name != name) continue;
    if (symbol.binding == kStbGlobal) {
      if (address != nullptr) *address = symbol.address;
      return true;
    }
    weak = &symbol;
  }
  if (weak == nullptr) return false;
  if (address != nullptr) *address = weak->address;
  return true;
}

bool LoadElf32(const void* data, size_t size, const ElfLoadOptions& options,
               ElfImage* image, std::string* error) {
  try {
    if (image == nullptr) {
      if (error != nullptr) *error = "ARM32 ELF: null output image";
      return false;
    }
    return Loader(data, size, options, error).Run(image);
  } catch (const std::bad_alloc&) {
    if (error != nullptr) {
      try { *error = "ARM32 ELF: cannot allocate image"; }
      catch (const std::bad_alloc&) { error->clear(); }
    }
  } catch (const std::length_error&) {
    if (error != nullptr) {
      try { *error = "ARM32 ELF: allocation size exceeds container limits"; }
      catch (const std::bad_alloc&) { error->clear(); }
    }
  }
  return false;
}

}  // namespace gesher::arm32
