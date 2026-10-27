// AMediaFormat's real Android ELF contract, shared by a JNI library and an
// executable. Both are built by the NDK for x86_64 and arm64-v8a (ADR 0064).
#include <dlfcn.h>
#include <media/NdkMediaFormat.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#ifndef MEDIA_FORMAT_EXECUTABLE
#include <jni.h>
#endif

namespace {

struct Report {
  std::string text;
  int failed = 0;
};

struct Part {
  Report& report;
  const char* name;
  int failed = 0;
  void Check(bool ok, const std::string& what) {
    if (ok) return;
    ++failed;
    ++report.failed;
    report.text += "media4 format failure: " + std::string(name) + ": " + what + "\n";
  }
  void End(const char* detail) {
    report.text += "media4 format " + std::string(name) + (failed ? ": FAILED; " : ": ok; ")
        + detail + "\n";
  }
};

struct Format {
  AMediaFormat* value = AMediaFormat_new();
  ~Format() { AMediaFormat_delete(value); }
  operator AMediaFormat*() const { return value; }
  void Delete() { AMediaFormat_delete(value); value = nullptr; }
};

const char* const kFunctions[] = {
    "AMediaFormat_new", "AMediaFormat_delete", "AMediaFormat_toString",
    "AMediaFormat_clear", "AMediaFormat_copy", "AMediaFormat_getInt32",
    "AMediaFormat_getInt64", "AMediaFormat_getFloat", "AMediaFormat_getDouble",
    "AMediaFormat_getSize", "AMediaFormat_getRect", "AMediaFormat_getString",
    "AMediaFormat_getBuffer", "AMediaFormat_setInt32", "AMediaFormat_setInt64",
    "AMediaFormat_setFloat", "AMediaFormat_setDouble", "AMediaFormat_setSize",
    "AMediaFormat_setRect", "AMediaFormat_setString", "AMediaFormat_setBuffer"};

struct Key { const char* name; const char* value; };
const Key kKeys[] = {
#define FORMAT_KEY(name, value) {name, value},
#include "pinned_format_keys.inc"
#undef FORMAT_KEY
};
static_assert(std::size(kFunctions) == 21);
static_assert(std::size(kKeys) == 152);

std::string Run() {
  Report report;
#if defined(__aarch64__)
  report.text = "media4 format ABI: arm64-v8a\n";
#elif defined(__x86_64__)
  report.text = "media4 format ABI: x86_64\n";
#else
#error Unsupported media format test ABI
#endif
#ifdef MEDIA_FORMAT_EXECUTABLE
  report.text += "media4 format caller: executable\n";
#else
  report.text += "media4 format caller: JNI\n";
#endif
  void* library = dlopen("libmediandk.so", RTLD_NOW);
  void* system = dlopen("/system/lib64/libmediandk.so", RTLD_NOW);
  Part exports{report, "exports"};
  exports.Check(library != nullptr && system != nullptr, "bare-name and system-path dlopen");
  for (const char* name : kFunctions) {
    void* entry = library != nullptr ? dlsym(library, name) : nullptr;
    exports.Check(entry != nullptr && entry == dlsym(system, name) &&
                      entry == dlsym(RTLD_DEFAULT, name), name);
  }
  // Library availability must describe its real subset, not add a dummy codec.
  exports.Check(dlsym(RTLD_DEFAULT, "AMediaCodec_createDecoderByType") == nullptr &&
                    (library == nullptr || dlsym(library, "AMediaCodec_createDecoderByType") == nullptr),
                "unimplemented codec stays absent");
  exports.End("21 functions through bare/system/default handles; no dummy codec");

  Format format;
  if (format.value == nullptr) {
    report.text += "media4 format failure: allocation returned null\n";
    return report.text;
  }
  Part data{report, "data"};
  for (size_t i = 0; i < std::size(kKeys); ++i) {
    const Key& key = kKeys[i];
    void* address = library != nullptr ? dlsym(library, key.name) : nullptr;
    if (address == nullptr) {
      data.Check(false, key.name);
      continue;
    }
    const char* const* slot = static_cast<const char* const*>(address);
    bool valid = *slot != nullptr && std::strcmp(*slot, key.value) == 0;
    data.Check(valid && address == dlsym(system, key.name) &&
                   address == dlsym(RTLD_DEFAULT, key.name), key.name);
    if (!valid) continue;
    AMediaFormat_setInt32(format, *slot, static_cast<int32_t>(i));
    int32_t value = -1;
    data.Check(AMediaFormat_getInt32(format, key.value, &value) &&
                   value == static_cast<int32_t>(i), std::string(key.name) + " as a format key");
  }
  data.Check(library != nullptr && dlsym(library, "AMEDIAFORMAT_KEY_MIME") ==
                 static_cast<const void*>(&AMEDIAFORMAT_KEY_MIME) &&
                 std::strcmp(AMEDIAFORMAT_KEY_MIME, "mime") == 0,
             "relocated data import is the pointer object, not its string");
  data.End("152 actual pointer objects and pinned AOSP values; ELF data indirection");
  AMediaFormat_clear(format);

  Part typed{report, "typed"};
  AMediaFormat_setInt32(format, "i32", INT32_MIN + 7);
  AMediaFormat_setInt64(format, "i64", INT64_C(0x1122334455667788));
  AMediaFormat_setFloat(format, "float", 1.25f);
  AMediaFormat_setDouble(format, "double", -4096.125);
  const size_t large_size = (size_t{1} << 40) + 23;
  AMediaFormat_setSize(format, "size", large_size);
  AMediaFormat_setRect(format, "rect", -4, 7, 1920, 1080);
  int32_t i32 = 0, left = 0, top = 0, right = 0, bottom = 0;
  int64_t i64 = 0;
  float f32 = 0;
  double f64 = 0;
  size_t size = 0;
  typed.Check(AMediaFormat_getInt32(format, "i32", &i32) && i32 == INT32_MIN + 7, "int32");
  typed.Check(AMediaFormat_getInt64(format, "i64", &i64) &&
                  i64 == INT64_C(0x1122334455667788), "full-width int64");
  typed.Check(AMediaFormat_getFloat(format, "float", &f32) && f32 == 1.25f, "float");
  typed.Check(AMediaFormat_getDouble(format, "double", &f64) && f64 == -4096.125, "double");
  typed.Check(AMediaFormat_getSize(format, "size", &size) && size == large_size, "LP64 size_t");
  typed.Check(AMediaFormat_getRect(format, "rect", &left, &top, &right, &bottom) &&
                  left == -4 && top == 7 && right == 1920 && bottom == 1080, "rectangle");
  // Call typed dlsym entries too, exercising their real ABI rather than presence.
  auto set_double = reinterpret_cast<decltype(&AMediaFormat_setDouble)>(
      library != nullptr ? dlsym(library, "AMediaFormat_setDouble") : nullptr);
  auto get_double = reinterpret_cast<decltype(&AMediaFormat_getDouble)>(
      system != nullptr ? dlsym(system, "AMediaFormat_getDouble") : nullptr);
  if (set_double != nullptr && get_double != nullptr) {
    set_double(format, "dynamic", 8192.0625);
    typed.Check(get_double(format, "dynamic", &f64) && f64 == 8192.0625,
                "floating argument through dlsym");
  } else {
    typed.Check(false, "typed dlsym entries missing");
  }
  typed.End("int32/int64/float/double/LP64 size/rectangle; direct and dlsym calls");

  Part ownership{report, "ownership"};
  char text[] = "\xe2\x82\xac-\xf0\x9f\x8e\xb5";
  const char* key = "title-\xc3\xa9";
  AMediaFormat_setString(format, key, text);
  text[0] = 'x';
  const char* string = nullptr;
  ownership.Check(AMediaFormat_getString(format, key, &string) &&
                      std::strcmp(string, "\xe2\x82\xac-\xf0\x9f\x8e\xb5") == 0,
                  "UTF-8 string owns a copy of caller bytes");
  uint8_t bytes[] = {0, 1, 0x80, 0xff};
  AMediaFormat_setBuffer(format, "buffer", bytes, sizeof(bytes));
  bytes[0] = 9;
  void* buffer = nullptr;
  size_t length = 0;
  const uint8_t expected[] = {0, 1, 0x80, 0xff};
  ownership.Check(AMediaFormat_getBuffer(format, "buffer", &buffer, &length) &&
                      length == sizeof(expected) && std::memcmp(buffer, expected, length) == 0,
                  "binary buffer owns a copy of caller bytes");
  Format copy;
  AMediaFormat_setInt32(copy, "old", 19);
  ownership.Check(AMediaFormat_copy(copy, format) == AMEDIA_OK, "copy returns AMEDIA_OK");
  format.Delete();
  ownership.Check(AMediaFormat_getBuffer(copy, "buffer", &buffer, &length) &&
                      length == sizeof(expected) && std::memcmp(buffer, expected, length) == 0,
                  "copied buffer survives deleting its source");
  ownership.Check(AMediaFormat_getString(copy, key, &string) &&
                      std::strcmp(string, "\xe2\x82\xac-\xf0\x9f\x8e\xb5") == 0,
                  "copied UTF-8 string survives deleting its source");
  i32 = 123;
  ownership.Check(!AMediaFormat_getInt32(copy, "old", &i32) && i32 == 123,
                  "copy replaces the target map");
  ownership.End("UTF-8 and binary input copies; copied values survive source deletion");

  Part errors{report, "errors"};
  i32 = 123;
  i64 = 456;
  f32 = 7.5f;
  errors.Check(!AMediaFormat_getInt32(copy, "missing", &i32) && i32 == 123,
               "missing value preserves output");
  errors.Check(!AMediaFormat_getInt64(copy, "i32", &i64) && i64 == 456,
               "type mismatch preserves int64 output");
  errors.Check(!AMediaFormat_getFloat(copy, "double", &f32) && f32 == 7.5f,
               "type mismatch preserves float output");
  errors.Check(!AMediaFormat_getInt32(nullptr, "i32", &i32) && i32 == 123 &&
                   !AMediaFormat_getInt32(copy, nullptr, &i32) && i32 == 123,
               "pinned null-format/name getter behavior");
  AMediaFormat_setInt32(nullptr, "i32", 99);
  AMediaFormat_setInt32(copy, nullptr, 99);
  errors.Check(AMediaFormat_getInt32(copy, "i32", &i32) && i32 == INT32_MIN + 7,
               "null-format/name setters leave valid data unchanged");
  errors.Check(AMediaFormat_copy(nullptr, copy) == AMEDIA_ERROR_INVALID_PARAMETER &&
                   AMediaFormat_copy(copy, nullptr) == AMEDIA_ERROR_INVALID_PARAMETER,
               "copy rejects null source and destination");
  errors.Check(AMediaFormat_delete(nullptr) == AMEDIA_OK, "delete null follows pinned delete");
  errors.End("missing/type mismatch preserve outputs; pinned null/copy/delete results");

  Part clear{report, "clear"};
  AMediaFormat_clear(copy);
  buffer = reinterpret_cast<void*>(uintptr_t{0x1234});
  length = 567;
  clear.Check(!AMediaFormat_getBuffer(copy, "buffer", &buffer, &length) &&
                  buffer == reinterpret_cast<void*>(uintptr_t{0x1234}) && length == 567,
              "clear removes buffer without modifying failed outputs");
  AMediaFormat_setString(copy, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(copy, AMEDIAFORMAT_KEY_WIDTH, 1920);
  clear.Check(AMediaFormat_getString(copy, "mime", &string) &&
                  std::strcmp(string, "video/avc") == 0 &&
                  AMediaFormat_getInt32(copy, "width", &i32) && i32 == 1920,
              "clear permits reuse with imported format keys");
  clear.End("clear removes old entries and permits typed reuse");

  Part dump{report, "dump"};
  AMediaFormat_setInt32(copy, "number", 17);
  AMediaFormat_setFloat(copy, "fraction", 1.25f);
  const char* borrowed = AMediaFormat_toString(copy);
  std::string snapshot = borrowed != nullptr ? borrowed : "";
  dump.Check(snapshot.find("mime: string(video/avc)") != std::string::npos &&
                 snapshot.find("number: int32(17)") != std::string::npos &&
                 snapshot.find("fraction: float(") != std::string::npos,
             "toString reflects actual typed values");
  AMediaFormat_setInt32(copy, "number", 23);
  dump.Check(borrowed != nullptr && snapshot == borrowed,
             "borrowed debug string remains valid until the next toString call");
  const char* updated = AMediaFormat_toString(copy);
  dump.Check(updated != nullptr && std::strstr(updated, "number: int32(23)") != nullptr,
             "next toString reflects the updated map");
  dump.End("typed debug output and the pinned borrowed-string lifetime");

  Part lifecycle{report, "lifecycle"};
  for (int i = 0; i < 128; ++i) {
    Format source, target;
    AMediaFormat_setInt64(source, "cycle", INT64_C(0x1234567800000000) + i);
    lifecycle.Check(AMediaFormat_copy(target, source) == AMEDIA_OK &&
                        AMediaFormat_getInt64(target, "cycle", &i64) &&
                        i64 == INT64_C(0x1234567800000000) + i,
                    "new/copy/get cycle " + std::to_string(i));
    AMediaFormat_clear(source);
    lifecycle.Check(AMediaFormat_getInt64(target, "cycle", &i64) &&
                        i64 == INT64_C(0x1234567800000000) + i,
                    "target survives source clear " + std::to_string(i));
  }
  lifecycle.End("128 independent new/copy/clear/delete cycles");
  if (library != nullptr) dlclose(library);
  if (system != nullptr) dlclose(system);
  report.text += report.failed == 0 ? "media4 format: all passed\n" : "media4 format: FAILED\n";
  return report.text;
}

}  // namespace

#ifdef MEDIA_FORMAT_EXECUTABLE
int main() {
  std::string report = Run();
  std::fputs(report.c_str(), stdout);
  std::fflush(stdout);
  return report.find("media4 format: all passed\n") != std::string::npos ? 0 : 1;
}
#else
extern "C" JNIEXPORT jstring JNICALL Java_gesher_MediaFormatTest_run(JNIEnv* env, jclass) {
  return env->NewStringUTF(Run().c_str());
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_gesher_mediaformat_MediaFormatActivity_run(JNIEnv* env, jclass) {
  return env->NewStringUTF(Run().c_str());
}
#endif
