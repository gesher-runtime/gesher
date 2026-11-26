// gesher: checked, file-backed ARM32 native library session.
// Deliberately isolated from production dalvikvm until its ART trampoline,
// thread model and NDK dependencies can be implemented and verified.
#include <gesher/arm32_native_session.h>

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace gesher::arm32 {
namespace {
bool Fail(std::string* error, const char* reason) {
  if (error) *error = reason;
  return false;
}
}  // namespace

NativeLibrarySession::NativeLibrarySession(
    std::filesystem::path directory, JNIEnv* host_env, size_t memory_budget)
    : directory_(std::move(directory)),
      program_([this](std::string_view name, std::vector<uint8_t>* bytes,
                      std::string* error) { return Supply(name, bytes, error); },
               memory_budget),
      bionic_(program_),
      jni_(program_, host_env) {}

NativeLibrarySession::~NativeLibrarySession() {
  try { (void)Close(nullptr); } catch (...) {}
}

bool NativeLibrarySession::SafeSoname(std::string_view name) {
  if (name.size() < 4 || name.size() > 255 || name[0] == '.' ||
      name.find("..") != std::string_view::npos ||
      name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos ||
      name.find(':') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos)
    return false;
  for (unsigned char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' ||
          c == '.' || c == '+'))
      return false;
  }
  return true;
}

bool NativeLibrarySession::Supply(std::string_view name,
                                  std::vector<uint8_t>* output,
                                  std::string* error) {
  if (!output || !SafeSoname(name))
    return Fail(error, "ARM32 native library soname is invalid or unsafe");
  try {
    std::error_code ec;
    const auto root = std::filesystem::canonical(directory_, ec);
    if (ec || !std::filesystem::is_directory(root))
      return Fail(error, "ARM32 native library directory does not exist");
    const auto requested = root / std::string(name);
    const auto file = std::filesystem::canonical(requested, ec);
    if (ec || file.parent_path() != root ||
        !std::filesystem::is_regular_file(file))
      return Fail(error, "ARM32 native library not found inside its directory");
    const uintmax_t filesize = std::filesystem::file_size(file, ec);
    if (ec || filesize == 0 || filesize > kMaxLibraryFile ||
        filesize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
      return Fail(error, "ARM32 native library is missing, empty or too large");
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return Fail(error, "cannot open ARM32 native library file");
    std::vector<uint8_t> staged(static_cast<size_t>(filesize));
    if (!stream.read(reinterpret_cast<char*>(staged.data()),
                     static_cast<std::streamsize>(filesize)))
      return Fail(error, "short read of ARM32 native library file");
    // Reject changed/growing files between stat and read. This does not
    // replace a full installer-owned immutable APK extraction snapshot.
    if (stream.peek() != std::char_traits<char>::eof())
      return Fail(error, "ARM32 native library changed during read");
    *output = std::move(staged);
    if (error) error->clear();
    return true;
  } catch (const std::exception&) {
    return Fail(error, "ARM32 native library file provider failed");
  }
}

bool NativeLibrarySession::Open(
    std::string_view soname, bool require_jni_onload, std::string* error,
    NativeRegistrationHandler registration_handler) {
  if (state_ != State::Fresh)
    return Fail(error, "ARM32 native library session can only be opened once");
  // Even failed sessions are deliberately single-use. A partially bound host
  // import thunk and its guest mapping cannot safely be reused as a new JVM.
  state_ = State::Closed;
  if (!SafeSoname(soname))
    return Fail(error, "ARM32 native library root name is invalid");
  try {
    root_ = std::string(soname);
    if (!bionic_.Install(error) || !jni_.Install(error))
      return false;
    // JNI_OnLoad is allowed to call RegisterNatives immediately. The host
    // callback must be installed before we execute any guest constructors
    // or the first JNI_OnLoad instruction.
    if (registration_handler &&
        !jni_.SetNativeRegistrationHandler(
            std::move(registration_handler), error))
      return false;
    if (!program_.LoadLibrary(root_, error) || !program_.Initialize(error)) {
      // Partial constructor success may leave initialized dependencies whose
      // finalizers call through JNI. Shut them down *while jni_ is alive*,
      // rather than deferring Program's destructor until after Jni32 dies.
      (void)program_.Shutdown(nullptr);
      return false;
    }
    if (require_jni_onload) {
      jint version = 0;
      if (!jni_.CallOnLoad(root_, &version, error)) {
        (void)program_.Shutdown(nullptr);
        return false;
      }
      onload_invoked_ = true;
    }
    state_ = State::Open;
    if (error) error->clear();
    return true;
  } catch (const std::exception&) {
    (void)program_.Shutdown(nullptr);
    return Fail(error, "ARM32 native library session initialization failed");
  }
}

bool NativeLibrarySession::CallNative(
    std::string_view symbol, std::string_view descriptor, JniCallMode mode,
    jobject receiver, const jvalue* args, size_t argument_count,
    jvalue* result, std::string* error) {
  if (state_ != State::Open)
    return Fail(error, "ARM32 native library session is not open");
  return jni_.CallNative(root_, symbol, descriptor, mode, receiver, args,
                         argument_count, result, error);
}

bool NativeLibrarySession::Close(std::string* error) {
  if (state_ == State::Closed) {
    if (error) error->clear();
    return true;
  }
  if (state_ == State::Fresh) {
    state_ = State::Closed;
    if (error) error->clear();
    return true;
  }
  state_ = State::Closed;
  std::string unload_error;
  const bool unloaded = !onload_invoked_ ||
                        !program_.HasExport(root_, "JNI_OnUnload") ||
                        jni_.CallOnUnload(root_, &unload_error);
  onload_invoked_ = false;
  std::string shutdown_error;
  const bool stopped = program_.Shutdown(&shutdown_error);
  if (!unloaded || !stopped) {
    if (error) *error = !unload_error.empty() ? unload_error : shutdown_error;
    return false;
  }
  if (error) error->clear();
  return true;
}
}  // namespace gesher::arm32
