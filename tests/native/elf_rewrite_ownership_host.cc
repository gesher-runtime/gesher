// Complete production rewriter; internal state inspected without shipping a
// test interface. Win32 primitives are the explicitly isolated Linux boundary.
#include "fs_rewrite.cc"
#include <array>
namespace elf = gesher::elf;
namespace {
int checks;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL rewrite ownership: %s\n", message); std::abort(); }
}
constexpr size_t kPageBytes = 4096;
struct File {
  alignas(Elf64_Ehdr) std::array<uint8_t, sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr)> bytes{};
  File(size_t pages, bool deferred = false) {
    auto* h = reinterpret_cast<Elf64_Ehdr*>(bytes.data());
    h->e_phoff = sizeof(Elf64_Ehdr); h->e_phnum = deferred ? 2 : 1;
    auto* p = reinterpret_cast<Elf64_Phdr*>(bytes.data() + h->e_phoff);
    p[0].p_type = PT_LOAD; p[0].p_flags = PF_R | PF_X;
    p[0].p_filesz = p[0].p_memsz = pages * kPageBytes;
    if (deferred) {
      p[0].p_memsz += kPageBytes; p[1].p_type = PT_GNU_EH_FRAME;
      p[1].p_vaddr = pages * kPageBytes + 64; p[1].p_filesz = p[1].p_memsz = 16;
    }
  }
};
struct Counts {
  size_t objects = elf::Objects().size(), traps = elf::Traps().size();
  size_t ranges = elf::Ranges().size(), syscalls = elf::SyscallSites().size();
  size_t fingerprints = elf::Fingerprints().size(), pending = elf::Pending().size();
  size_t allocations = elf_host_platform::LiveReservations();
  bool operator==(const Counts&) const = default;
};
uint8_t* Image(size_t pages) {
  auto* p = static_cast<uint8_t*>(mmap(nullptr, pages * kPageBytes,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(p != MAP_FAILED, "real image mapping");
  std::memset(p, 0x90, pages * kPageBytes); return p;
}
void StubCode(uint8_t* image) {
  // FS read needs an out-of-line stub. Rewritten, never executed on Linux GS.
  const uint8_t code[] = {0x64, 0x48, 0x8B, 0x07, 0x0F, 0x05, 0xC3};
  std::memcpy(image + 128, code, sizeof(code));
}
std::unique_ptr<elf::FsRewriteRegistration> Register(uint8_t* image, File& file, size_t pages) {
  std::unique_ptr<elf::FsRewriteRegistration> owner;
  elf::FsRewriteStats stats; std::string error;
  Check(elf::RewriteFsAccesses("ownership-fixture", file.bytes.data(), file.bytes.size(),
      image, image, pages * kPageBytes, &stats, &error, 0, &owner), "production rewrite succeeds");
  Check(owner != nullptr, "successful rewrite returns ownership"); return owner;
}
}

int main() {
  Counts empty; File file(2); uint8_t* stable_image = Image(2); StubCode(stable_image);
  auto stable = Register(stable_image, file, 2); Counts retained;
  Check(retained.objects == empty.objects + 1, "one owned object registered");
  Check(retained.traps > empty.traps && retained.ranges > empty.ranges, "trap and code metadata recorded");
  Check(retained.syscalls > empty.syscalls && retained.fingerprints > empty.fingerprints,
        "syscall and fingerprint metadata recorded");
  Check(retained.allocations == empty.allocations + 1, "load-time stub arena owned");
  elf::LazyObject* object = elf::Objects().back();
  StubCode(stable_image + kPageBytes);
  Check(elf::ProtectLoadedMemory(stable_image + kPageBytes, kPageBytes,
      PROT_READ | PROT_EXEC) == 0, "new code enters the production pending-page path");
  // Invoke the production handler explicitly: this Linux fixture does not
  // install or emulate Windows vectored exception dispatch.
  Check(elf::HandleExecuteFault(reinterpret_cast<uintptr_t>(stable_image + kPageBytes + 128)),
      "deferred rewrite after load-time stubs succeeds");
  Check(object->arenas.size() == 2 && !object->arenas[0].writable_executable &&
        object->arenas[1].writable_executable, "deferred stubs use a writable arena");
  retained = Counts{};
  for (int i = 0; i < 32; ++i) {
    uint8_t* abandoned_image = Image(2); StubCode(abandoned_image);
    auto abandoned = Register(abandoned_image, file, 2); abandoned.reset();
    Check(Counts{} == retained, "rollback withdraws only owned records and frees every stub");
    Check(munmap(abandoned_image, 2 * kPageBytes) == 0, "abandoned mapping released");
  }
  uint8_t* failure_image = Image(2); StubCode(failure_image);
  std::unique_ptr<elf::FsRewriteRegistration> failed;
  elf::FsRewriteStats stats; std::string error;
  elf_host_platform::FailNextVirtualProtect();
  Check(!elf::RewriteFsAccesses("failed-prepare", file.bytes.data(), file.bytes.size(),
      failure_image, failure_image, 2 * kPageBytes, &stats, &error, 0, &failed), "preparation failure propagates");
  Check(!failed && Counts{} == retained, "incomplete rewrite frees allocations and metadata");
  munmap(failure_image, 2 * kPageBytes);
  File packed(1, true); uint8_t* packed_image = Image(2);
  auto packed_owner = Register(packed_image, packed, 2);
  Check(elf::DeferUnpackedCode("packed", packed.bytes.data(), packed_image, &error), "pending pages registered");
  Check(elf::Pending().size() == retained.pending + 2, "both packed code pages remain pending");
  packed_owner.reset(); Check(Counts{} == retained, "pending pages removed before object destruction");
  munmap(packed_image, 2 * kPageBytes);
  stable.reset(); Check(Counts{} == empty, "all rewrite ownership returns to baseline");
  munmap(stable_image, 2 * kPageBytes);
  std::printf("ELF rewrite ownership: %d checks passed\n", checks);
}
