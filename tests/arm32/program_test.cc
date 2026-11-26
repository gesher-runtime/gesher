// gesher: end-to-end isolated ARM32 dynamic library execution contracts.
// This deliberately does not represent Android ART or an installed APK.
#include <gesher/arm32_program.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace gesher::arm32;
namespace {
void Check(bool okay, const char* message) {
  if (!okay) { std::cerr << "ARM32 program test: " << message << '\n'; std::exit(1); }
}
std::vector<uint8_t> ReadFile(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  Check(static_cast<bool>(stream), "fixture must exist");
  return {std::istreambuf_iterator<char>(stream), {}};
}
void Run(const char* fixture) {
  const auto guest = ReadFile(fixture);
  const std::string soname = "gesher_arm32_fixture.so";
  unsigned opens = 0;
  Program program([&](std::string_view name, std::vector<uint8_t>* bytes,
                      std::string* error) {
    ++opens;
    if (name != soname) {
      if (error) *error = "unexpected guest library name";
      return false;
    }
    *bytes = guest;
    return true;
  });
  std::string error;
  bool reentry_rejected = false;
  Check(program.BindHostSymbol("host_adjust", {ValueKind::I32}, ValueKind::I32,
        [&](const std::vector<Value>& arguments, Value* result, std::string*) {
          if (arguments.size() != 1) return false;
          // A guest-to-host callback may not change the program's module
          // vector while the outer call holds its export entrypoint.
          std::string nested_error;
          const bool reentered = program.LoadLibrary("missing.so", &nested_error);
          reentry_rejected = !reentered &&
                             nested_error == "ARM32 program is already active";
          if (!reentry_rejected) return false;
          *result = Value::Signed32(static_cast<int32_t>(arguments[0].bits) * 3);
          return true;
        }, &error), "typed import binding");
  Check(!program.BindHostSymbol("host_adjust", {}, ValueKind::Void,
        [](const auto&, Value*, std::string*) { return true; }, &error),
        "duplicate import rejected");
  if (!program.LoadLibrary(soname, &error)) std::cerr << error << '\n';
  Check(program.LibraryCount() == 1, "one loaded library");
  Check(opens == 1, "provider called exactly once");
  Check(program.LoadLibrary(soname, &error) && opens == 1,
        "idempotent already loaded dependency");
  Value result;
  Check(!program.CallExport(soname, "fixture_add",
        {Value::Signed32(1), Value::Signed32(2)}, ValueKind::I32,
        &result, &error), "cannot call library before its constructors");
  if (!program.Initialize(&error)) std::cerr << error << '\n';

  Check(program.CallExport(soname, "fixture_add",
        {Value::Signed32(11), Value::Signed32(13)}, ValueKind::I32, &result, &error),
        "call exported code");
  Check(static_cast<int32_t>(result.bits) == 66,
        "constructor and global pointer affect result");

  Check(program.CallExport(soname, "fixture_import",
        {Value::Signed32(9)}, ValueKind::I32, &result, &error) &&
        static_cast<int32_t>(result.bits) == 32,
        "guest PLT calls a typed host thunk");
  Check(reentry_rejected, "reentrant provider load was rejected during guest execution");
  Check(program.CallExport(soname, "fixture_wide",
        {Value::Signed32(5), Value::Signed64(0x100000002LL),
         Value::Signed64(0x200000003LL), Value::Signed32(7)},
        ValueKind::I64, &result, &error) &&
        result.bits == 0x300000011ULL, "AAPCS32 aligned 64-bit registers and spill");
  Check(program.CallExport(soname, "fixture_double",
        {Value::Signed32(2), Value::Float64(1.25), Value::Signed32(3),
         Value::Float64(2.5)}, ValueKind::F64, &result, &error) &&
        result.bits == Value::Float64(8.75).bits, "softfp double return");
  Check(program.CallExport(soname, "fixture_float",
        {Value::Float32(1.25f), Value::Float32(2.5f)}, ValueKind::F32,
        &result, &error) && result.bits == Value::Float32(3.75f).bits,
        "softfp float return");

  constexpr uint32_t kData = 0x30000;
  constexpr auto kRW = MemoryPermission::kRead | MemoryPermission::kWrite;
  Check(program.Memory().Map(kData, 4096, kRW), "map guest pointer scratch");
  Check(program.Memory().Write32(kData, 4) &&
        program.Memory().Write32(kData + 4, 8) &&
        program.Memory().Write32(kData + 8, 15), "populate guest pointer");
  Check(program.CallExport(soname, "fixture_memory",
        {Value::GuestPointer(kData), Value::Unsigned32(3)}, ValueKind::U32,
        &result, &error) && result.bits == 27, "32-bit memory pointer call");
  uint32_t stored = 0;
  Check(program.Memory().Read32(kData, &stored) && stored == 27,
        "guest pointer write visible to caller");
  Check(!program.CallExport(soname, "does_not_exist", {}, ValueKind::I32,
                           &result, &error), "missing export fails explicitly");

  // Failed new load rolls back its own address reservation, preserving the
  // previously loaded module and its executable trampolines.
  Check(!program.LoadLibrary("missing.so", &error) &&
        program.LibraryCount() == 1, "provider failure preserves prior program");
  Check(program.CallExport(soname, "fixture_import",
        {Value::Signed32(10)}, ValueKind::I32, &result, &error) &&
        static_cast<int32_t>(result.bits) == 35,
        "success after failed library transaction");
  Check(program.Initialize(&error), "successful constructors never rerun");
  Check(program.Shutdown(&error), "run destructors and close program");
  Check(program.Shutdown(&error), "repeat shutdown is idempotent");
  Check(!program.CallExport(soname, "fixture_import",
        {Value::Signed32(10)}, ValueKind::I32, &result, &error),
        "closed program cannot run native calls");
  Check(!program.LoadLibrary(soname, &error),
        "closed program cannot reload a native library");
  Check(!program.Initialize(&error),
        "closed program cannot reinitialize");

  Program malformed([](std::string_view, std::vector<uint8_t>* output,
                       std::string*) {
    *output = {0x7f, 'E', 'L', 'F', 1};
    return true;
  });
  Check(!malformed.LoadLibrary("bad.so", &error) &&
        malformed.LibraryCount() == 0,
        "malformed ELF never leaves a mapped module");
  Check(!malformed.LoadLibrary("bad.so", &error) &&
        malformed.LibraryCount() == 0,
        "malformed ELF retry preserves image allocation cursor");

  Program throwing([](std::string_view, std::vector<uint8_t>*, std::string*) -> bool {
    throw 42;
  });
  Check(!throwing.LoadLibrary("bad.so", &error) &&
        throwing.LibraryCount() == 0,
        "provider exception is contained and rolled back");

  Program no_stack({}, 4096);
  Check(!no_stack.LoadLibrary("any.so", &error) &&
        no_stack.LibraryCount() == 0,
        "insufficient guest address budget fails before library provider");
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "one compiled ARM/Thumb ELF fixture path required");
  Run(argv[1]);
  std::cout << "ARM32 linked program and guest ABI tests passed\n";
  return 0;
}
