// gesher: standalone file-backed JNI_OnLoad/OnUnload guest library session.
// Does not register a native into real ART or assert APK compatibility.
#include <gesher/arm32_native_session.h>

#include <jni.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace gesher::arm32;

namespace {
void Check(bool good, const char* message) {
  if (!good) {
    std::cerr << "ARM32 native session: " << message << '\n';
    std::exit(1);
  }
}
jint JNICALL Version(JNIEnv*) { return JNI_VERSION_1_6; }

struct TempDirectory {
  std::filesystem::path path;
  explicit TempDirectory(std::filesystem::path fixture) {
    path = std::filesystem::temp_directory_path() /
           ("gesher-arm32-native-" + fixture.stem().string());
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
void Run(const char* filename) {
  const std::filesystem::path source(filename);
  Check(std::filesystem::is_regular_file(source), "guest .so exists");
  TempDirectory directory(source);
  const std::string soname = "gesher_arm32_jni_fixture.so";
  const auto copied = directory.path / soname;
  Check(std::filesystem::copy_file(source, copied), "copy extracted guest ELF32");

  JNINativeInterface_ functions{};
  functions.GetVersion = Version;
  JNIEnv real{};
  real.functions = &functions;
  std::string error;

  // Failing a caller-supplied root path cannot escape its directory, including
  // backslash paths on Windows, non-basename paths and traversal attempts.
  for (std::string_view invalid : {"../escape.so", "/absolute.so",
                                   "C:\\escaped.so", "..hidden.so", ""}) {
    NativeLibrarySession denied(directory.path, &real);
    Check(!denied.Open(invalid, true, &error) && !denied.IsOpen(),
          "reject unsafe extracted soname");
  }
  {
    NativeLibrarySession missing(directory.path, &real);
    Check(!missing.Open("absent.so", true, &error) && !missing.IsOpen(),
          "missing library cannot initialize guest runtime");
  }

  NativeLibrarySession native(directory.path, &real);
  if (!native.Open(soname, true, &error))
    std::cerr << "ARM32 native session open: " << error << '\n';
  Check(native.IsOpen(), "load real ELF32 from directory and invoke guest JNI_OnLoad");
  Check(native.GuestProgram().LibraryCount() == 1,
        "one native library loaded with byte ownership");
  Value initial;
  Check(native.GuestProgram().CallExport(soname, "jni32_onload_state",
        {}, ValueKind::I32, &initial, &error) && initial.bits == 1,
        "guest JNI_OnLoad executed via mapped JavaVM32");

  jvalue args[2]{};
  args[0].j = 0x100000002LL;
  args[1].i = 7;
  jvalue result{};
  Check(native.CallNative("Java_gesher_Test_critical", "(JI)J",
                          JniCallMode::CriticalNative, nullptr,
                          args, 2, &result, &error) &&
        result.j == 0x100000009LL,
        "invoke compiled ARM/Thumb @CriticalNative through file-backed session");
  Check(native.Close(&error), "unload JNI guest, then run ELF32 finalizers");
  Check(!native.IsOpen() && native.Close(&error), "native session closes idempotently");
  Check(!native.CallNative("Java_gesher_Test_critical", "(JI)J",
                           JniCallMode::CriticalNative, nullptr,
                           args, 2, &result, &error),
        "closed session refuses JNI entry");
  Check(!native.Open(soname, true, &error), "closed native session cannot reopen");

  // A DSO without requested JNI_OnLoad negotiation is allowed to run guest
  // code when its caller explicitly opts out of JNI lifecycle negotiation.
  NativeLibrarySession raw(directory.path, &real);
  Check(raw.Open(soname, false, &error), "file-backed loader without JNI_OnLoad");
  Check(raw.GuestProgram().CallExport(soname, "jni32_onload_state",
        {}, ValueKind::I32, &initial, &error) && initial.bits == 0,
        "fresh guest memory has no JNI_OnLoad side effects");
  Check(raw.Close(&error), "close independent raw guest session");
}
}  // namespace

int main(int argc, char** argv) {
  Check(argc == 2, "single guest ARM/Thumb JNI ELF32 fixture");
  Run(argv[1]);
  std::cout << "ARM32 file-backed JNI native session passed\n";
  return 0;
}
