// The NDK's AMediaFormat for loaded ELF code (ADR 0064): the actual pinned
// AOSP implementation, with System V entries and its pointer data objects.
#include <cstdint>

#include <media/NdkMediaFormat.h>

#include <gesher/bionic_shim.h>

extern "C" {
#define MEDIA_FORMAT_KEY(name) extern const char* name;
#include "media_format_keys.inc"
#undef MEDIA_FORMAT_KEY
}

namespace gesher::media {
namespace {

// These signatures use fixed-width integers, LP64 size_t and opaque pointers;
// only the host calling convention differs from an Android ELF caller's.
template <auto F>
struct Sysv;
template <class R, class... A, R (*F)(A...)>
struct Sysv<F> {
  static R __attribute__((sysv_abi)) Call(A... args) { return F(args...); }
};

void Register() {
  using gesher::bionic::AddBionicSymbol;
#define FORMAT_ENTRY(name) AddBionicSymbol(#name, reinterpret_cast<void*>(&Sysv<&name>::Call))
  FORMAT_ENTRY(AMediaFormat_new);
  FORMAT_ENTRY(AMediaFormat_delete);
  FORMAT_ENTRY(AMediaFormat_toString);
  FORMAT_ENTRY(AMediaFormat_clear);
  FORMAT_ENTRY(AMediaFormat_copy);
  FORMAT_ENTRY(AMediaFormat_getInt32);
  FORMAT_ENTRY(AMediaFormat_getInt64);
  FORMAT_ENTRY(AMediaFormat_getFloat);
  FORMAT_ENTRY(AMediaFormat_getDouble);
  FORMAT_ENTRY(AMediaFormat_getSize);
  FORMAT_ENTRY(AMediaFormat_getRect);
  FORMAT_ENTRY(AMediaFormat_getString);
  FORMAT_ENTRY(AMediaFormat_getBuffer);
  FORMAT_ENTRY(AMediaFormat_setInt32);
  FORMAT_ENTRY(AMediaFormat_setInt64);
  FORMAT_ENTRY(AMediaFormat_setFloat);
  FORMAT_ENTRY(AMediaFormat_setDouble);
  FORMAT_ENTRY(AMediaFormat_setSize);
  FORMAT_ENTRY(AMediaFormat_setRect);
  FORMAT_ENTRY(AMediaFormat_setString);
  FORMAT_ENTRY(AMediaFormat_setBuffer);
#undef FORMAT_ENTRY
  // An ELF data import dereferences this slot. Its string pointee is not
  // the symbol's address; publish the actual AOSP const-char-pointer object.
#define MEDIA_FORMAT_KEY(name) AddBionicSymbol(#name, &name);
#include "media_format_keys.inc"
#undef MEDIA_FORMAT_KEY
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
