// gesher: compiler-linked ELF32 DT_NEEDED and guest constructor ordering.
#include <gesher/arm32_program.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace gesher::arm32;
namespace {
void Check(bool good, const char* label) {
  if (!good) { std::cerr << "ARM32 DT_NEEDED: " << label << '\n'; std::exit(1); }
}
std::vector<uint8_t> Read(const char* path) {
  std::ifstream in(path, std::ios::binary);
  Check(bool(in), "open compiled ELF32 object");
  return {std::istreambuf_iterator<char>(in), {}};
}
void Run(const char* consumer_path, const char* dependency_path,
         const char* consumer_name, const char* dependency_name) {
  const auto consumer = Read(consumer_path);
  const auto dependency = Read(dependency_path);
  const auto provider = [&](std::string_view name, std::vector<uint8_t>* output,
                            std::string*) {
    if (name == consumer_name) { *output = consumer; return true; }
    if (name == dependency_name) { *output = dependency; return true; }
    return false;
  };
  Program program(provider);
  std::string error;
  if (!program.LoadLibrary(consumer_name, &error))
    std::cerr << "ARM32 dependent ELF: " << error << '\n';
  Check(program.LibraryCount() == 2, "real DT_NEEDED library load");
  Check(program.LoadLibrary(consumer_name, &error) && program.LibraryCount() == 2,
        "dependency graph loaded only once");

  Value returned;
  Check(!program.CallExport(consumer_name, "consumer_calculate",
                            {Value::Signed32(5)}, ValueKind::I32,
                            &returned, &error), "constructor gate");

  Check(program.Initialize(&error), "run dependency-first constructors");
  if (!program.CallExport(consumer_name, "consumer_calculate",
                          {Value::Signed32(5)}, ValueKind::I32,
                          &returned, &error))
    std::cerr << "ARM32 inter-DSO PLT: " << error << '\n';
  Check(static_cast<int32_t>(returned.bits) == 21 && error.empty(),
        "inter-DSO import and constructors produce real result");
  Check(program.CallExport(dependency_name, "dependency_increment",
                           {Value::Signed32(5)}, ValueKind::I32,
                           &returned, &error) &&
        static_cast<int32_t>(returned.bits) == 12,
        "dependency exports available by own library name");

  Program missing([&](std::string_view name, std::vector<uint8_t>* out,
                      std::string*) {
    if (name != consumer_name) return false;
    *out = consumer;
    return true;
  });
  Check(!missing.LoadLibrary(consumer_name, &error) &&
        missing.LibraryCount() == 0,
        "missing DT_NEEDED dependency rolls back entire transaction");
  Check(!missing.Initialize(&error) || missing.LibraryCount() == 0,
        "missing dependency can never publish an executable consumer");
}
}  // namespace
int main(int argc, char** argv) {
  Check(argc == 5, "consumer, dependency, consumer SONAME, dependency SONAME");
  Run(argv[1], argv[2], argv[3], argv[4]);
  std::cout << "ARM32 two-DSO dependency test passed\n";
  return 0;
}
