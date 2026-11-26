// gesher: integration of compiler-produced ARM32 ELF with guest-safe libc.
// All calls execute ARM/Thumb through Dynarmic, not host-native code.
#include <gesher/arm32_bionic.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace gesher::arm32;

namespace {
void Check(bool passed, const char* label) {
  if (!passed) { std::cerr << "ARM32 Bionic32: " << label << "\n"; std::exit(1); }
}
std::vector<uint8_t> Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "open ELF32 libc fixture");
  return {std::istreambuf_iterator<char>(in), {}};
}
void Run(const char* path) {
  const auto bytes = Read(path);
  const std::string soname = "gesher_arm32_bionic_fixture.so";
  Program program([&](std::string_view name, std::vector<uint8_t>* output,
                      std::string*) {
    if (name != soname) return false;
    *output = bytes;
    return true;
  });
  Bionic32 libc(program);
  std::string error;
  Check(libc.Install(&error), "install typed libc imports");
  if (!program.LoadLibrary(soname, &error)) std::cerr << "load: " << error << '\n';
  Check(program.LibraryCount() == 1, "load real compiler ELF32 libc fixture");
  Check(program.Initialize(&error), "initialize guest library");

  const auto call = [&](const char* symbol, int32_t expected) {
    Value result;
    if (!program.CallExport(soname, symbol, {}, ValueKind::I32, &result, &error))
      std::cerr << symbol << ": " << error << '\n';
    Check(static_cast<int32_t>(result.bits) == expected && error.empty(),
          "execute expected guest libc result");
  };
  call("bionic_heap", 42);
  call("bionic_copy", 77);
  call("bionic_strings", 21);
  call("bionic_errno", 55);

  Value invalid;
  Check(!program.CallExport(soname, "bionic_invalid", {}, ValueKind::I32,
                            &invalid, &error) && !error.empty(),
        "invalid guest pointer fails closed");
  call("bionic_heap", 42);  // Guest program survives a rejected host adapter.
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "pass one compiled ARM/Thumb libc fixture");
  Run(argv[1]);
  std::cout << "ARM32 guest libc integration passed\n";
  return 0;
}
