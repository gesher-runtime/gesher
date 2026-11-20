// Complete production loader cleanup, inspected without a production test API.
// These are explicit ownership units, not ELF parser or Windows VEH fixtures.
#include "elf_loader.cc"

namespace elf = gesher::elf;
namespace {
int checks;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL loader ownership: %s\n", message); std::abort(); }
}
elf::Library* Owner() {
  auto* lib = new elf::Library();
  lib->map_size = 4096;
  lib->base = static_cast<uint8_t*>(mmap(nullptr, lib->map_size,
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(lib->base != MAP_FAILED, "real owner mapping allocated");
  lib->strtab = reinterpret_cast<const char*>(lib->base);
  std::memcpy(lib->base, "missing_import", sizeof("missing_import"));
  return lib;
}
void Allocate(elf::Library* lib, bool multiple_pages) {
  const size_t count = multiple_pages ? elf::kStubPageSize / elf::kStubSize + 1 : 1;
  for (size_t i = 0; i < count; ++i) {
    auto* stub = static_cast<uint8_t*>(elf::MakeUnresolvedStub(lib, lib->strtab));
    Check(stub != nullptr, "production unresolved function stub allocated");
    uint64_t name = 0;
    std::memcpy(&name, stub + 2, sizeof(name));
    Check(name == reinterpret_cast<uint64_t>(lib->strtab), "stub owns a reference into its owner's real mapping");
  }
  auto* first = static_cast<uint8_t*>(elf::MakeUnresolvedData(lib, lib->strtab));
  auto* second = static_cast<uint8_t*>(elf::MakeUnresolvedData(lib, lib->strtab));
  Check(first != nullptr && second == first + elf::kDataSlot, "production data placeholders use owned slots");
  Check(lib->unresolved_pages.size() == (multiple_pages ? 2u : 1u), "every stub arena is retained by its owner");
}
}  // namespace

int main() {
  const size_t empty_allocations = elf_host_platform::LiveReservations();
  const size_t empty_bytes = elf_host_platform::ReservedBytes();
  const size_t empty_owners = elf::g_data_owners.size();
  auto* retained = Owner();
  Allocate(retained, false);
  const size_t retained_allocations = elf_host_platform::LiveReservations();
  const size_t retained_bytes = elf_host_platform::ReservedBytes();
  for (int retry = 0; retry < 32; ++retry) {
    auto* failed = Owner();
    Allocate(failed, true);
    Check(elf::g_data_owners.size() == empty_owners + 2, "pending and retained data owners are distinct");
    elf::DiscardLibrary(failed);
    Check(elf::g_data_owners.size() == empty_owners + 1 && elf::g_data_owners.back() == retained,
          "discard removes all raw references to only the retired data owner");
    Check(elf_host_platform::LiveReservations() == retained_allocations &&
          elf_host_platform::ReservedBytes() == retained_bytes,
          "all discarded stub pages and data reservations return to the baseline");
    Check(std::strcmp(retained->unresolved_data_names.front(), "missing_import") == 0,
          "retained diagnostic names remain mapped and intact");
  }
  elf::DiscardLibrary(retained);
  Check(elf::g_data_owners.size() == empty_owners &&
        elf_host_platform::LiveReservations() == empty_allocations &&
        elf_host_platform::ReservedBytes() == empty_bytes,
        "the final owner releases every diagnostic resource");
  std::printf("ELF loader resource ownership: %d checks passed\n", checks);
}
