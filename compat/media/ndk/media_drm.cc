// The NDK's MediaDrm and MediaCrypto (libmediandk.so) for loaded ELF code.
//
// Gesher has no DRM plugin: no crypto scheme (Widevine, PlayReady, ClearKey
// or any other) is available, as the Java MediaDrm/MediaCrypto stand-ins
// (compat/media/jni/media_jni_others.cc) already report. These are AOSP's
// NdkMediaDrm.cpp/NdkMediaCrypto.cpp answers on a device whose drm and crypto
// HALs offer no plugin, not a substitute for one:
//  - AMediaDrm_isCryptoSchemeSupported and AMediaCrypto_isCryptoSchemeSupported
//    are false for every UUID and container type;
//  - AMediaDrm_createByUUID and AMediaCrypto_new return null (createPlugin
//    fails), so no AMediaDrm or AMediaCrypto object ever exists;
//  - every AMediaDrm_* operation on an object is AMEDIA_ERROR_INVALID_OBJECT,
//    which AOSP returns for a null or plugin-less object; AMediaDrm_release
//    and AMediaCrypto_delete of null do nothing;
//  - AMediaCrypto_requiresSecureDecoderComponent is false (no plugin
//    requires one).
// An app checks the scheme first (Firefox's parent process does at startup)
// and goes without protected playback; before this, the call reached an
// unresolved-import stub, which aborts the process.
#include <cstddef>
#include <cstdint>

#include <media/NdkMediaCrypto.h>
#include <media/NdkMediaDrm.h>
#include <media/NdkMediaError.h>

#include <gesher/bionic_shim.h>

namespace gesher::media {
namespace {

#if defined(__x86_64__)
#define SYSV __attribute__((sysv_abi))
#elif defined(__aarch64__)
#define SYSV
#endif

bool SYSV DrmIsCryptoSchemeSupported(const uint8_t*, const char*) { return false; }
AMediaDrm* SYSV DrmCreateByUuid(const uint8_t*) { return nullptr; }
// Only null can be released: no object is ever created.
void SYSV DrmRelease(AMediaDrm*) {}

bool SYSV CryptoIsCryptoSchemeSupported(const uint8_t*) { return false; }
bool SYSV CryptoRequiresSecureDecoderComponent(const char*) { return false; }
AMediaCrypto* SYSV CryptoNew(const uint8_t*, const void*, size_t) { return nullptr; }
void SYSV CryptoDelete(AMediaCrypto*) {}

// An AMediaDrm_* operation: the object is not one createByUUID returned.
template <class Signature>
struct NoObject;
template <class... A>
struct NoObject<media_status_t(AMediaDrm*, A...)> {
  static media_status_t SYSV Call(AMediaDrm*, A...) { return AMEDIA_ERROR_INVALID_OBJECT; }
};

void Register() {
  using gesher::bionic::AddBionicSymbol;
  AddBionicSymbol("libmediandk.so", "AMediaDrm_isCryptoSchemeSupported",
                  reinterpret_cast<void*>(&DrmIsCryptoSchemeSupported));
  AddBionicSymbol("libmediandk.so", "AMediaDrm_createByUUID", reinterpret_cast<void*>(&DrmCreateByUuid));
  AddBionicSymbol("libmediandk.so", "AMediaDrm_release", reinterpret_cast<void*>(&DrmRelease));
  AddBionicSymbol("libmediandk.so", "AMediaCrypto_isCryptoSchemeSupported",
                  reinterpret_cast<void*>(&CryptoIsCryptoSchemeSupported));
  AddBionicSymbol("libmediandk.so", "AMediaCrypto_requiresSecureDecoderComponent",
                  reinterpret_cast<void*>(&CryptoRequiresSecureDecoderComponent));
  AddBionicSymbol("libmediandk.so", "AMediaCrypto_new", reinterpret_cast<void*>(&CryptoNew));
  AddBionicSymbol("libmediandk.so", "AMediaCrypto_delete", reinterpret_cast<void*>(&CryptoDelete));
  // decltype: the prototypes of <media/NdkMediaDrm.h>, never defined here.
#define NO_OBJECT(name) \
  AddBionicSymbol("libmediandk.so", #name, reinterpret_cast<void*>(&NoObject<decltype(name)>::Call))
  NO_OBJECT(AMediaDrm_setOnEventListener);
  NO_OBJECT(AMediaDrm_setOnExpirationUpdateListener);
  NO_OBJECT(AMediaDrm_setOnKeysChangeListener);
  NO_OBJECT(AMediaDrm_openSession);
  NO_OBJECT(AMediaDrm_closeSession);
  NO_OBJECT(AMediaDrm_getKeyRequest);
  NO_OBJECT(AMediaDrm_getKeyRequestWithDefaultUrlAndType);
  NO_OBJECT(AMediaDrm_provideKeyResponse);
  NO_OBJECT(AMediaDrm_restoreKeys);
  NO_OBJECT(AMediaDrm_removeKeys);
  NO_OBJECT(AMediaDrm_queryKeyStatus);
  NO_OBJECT(AMediaDrm_getProvisionRequest);
  NO_OBJECT(AMediaDrm_provideProvisionResponse);
  NO_OBJECT(AMediaDrm_getSecureStops);
  NO_OBJECT(AMediaDrm_releaseSecureStops);
  NO_OBJECT(AMediaDrm_getPropertyString);
  NO_OBJECT(AMediaDrm_getPropertyByteArray);
  NO_OBJECT(AMediaDrm_setPropertyString);
  NO_OBJECT(AMediaDrm_setPropertyByteArray);
  NO_OBJECT(AMediaDrm_encrypt);
  NO_OBJECT(AMediaDrm_decrypt);
  NO_OBJECT(AMediaDrm_sign);
  NO_OBJECT(AMediaDrm_verify);
#undef NO_OBJECT
}

[[maybe_unused]] const bool g_registered = (Register(), true);

}  // namespace
}  // namespace gesher::media
