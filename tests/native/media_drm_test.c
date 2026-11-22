// The NDK's MediaDrm and MediaCrypto from an x86-64 Android library through
// the bionic shim. Gesher has no DRM plugin, so it answers as Android does
// for a scheme no plugin supports: isCryptoSchemeSupported is false, no
// MediaDrm or MediaCrypto object is created and an operation on a null object
// is AMEDIA_ERROR_INVALID_OBJECT. The entries resolve both as imports and
// through dlsym, as an app probing for them does.
// Built by native-libs.yml as media_drm_test.so; elf_load_test calls
// gesher_selftest().
#include <dlfcn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <media/NdkMediaCrypto.h>
#include <media/NdkMediaDrm.h>
#include <media/NdkMediaError.h>

static int failures;

static void check(const char* what, int ok) {
  printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) ++failures;
}

// Widevine, PlayReady, ClearKey and the W3C common PSSH system id.
static const uint8_t kSchemes[][16] = {
    {0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce, 0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed},
    {0x9a, 0x04, 0xf0, 0x79, 0x98, 0x40, 0x42, 0x86, 0xab, 0x92, 0xe6, 0x5b, 0xe0, 0x88, 0x5f, 0x95},
    {0xe2, 0x71, 0x9d, 0x58, 0xa9, 0x85, 0xb3, 0xc9, 0x78, 0x1a, 0xb0, 0x30, 0xaf, 0x78, 0xd3, 0x0e},
    {0x10, 0x77, 0xef, 0xec, 0xc0, 0xb2, 0x4d, 0x02, 0xac, 0xe3, 0x3c, 0x1e, 0x52, 0xe2, 0xfb, 0x4b},
};

int gesher_selftest(void) {
  printf("media_drm: no crypto scheme without a DRM plugin\n");
  bool any_drm = false, any_crypto = false, any_drm_object = false, any_crypto_object = false;
  for (size_t i = 0; i < sizeof(kSchemes) / sizeof(kSchemes[0]); ++i) {
    any_drm |= AMediaDrm_isCryptoSchemeSupported(kSchemes[i], NULL);
    any_drm |= AMediaDrm_isCryptoSchemeSupported(kSchemes[i], "video/mp4");
    any_drm |= AMediaDrm_isCryptoSchemeSupported(kSchemes[i], "audio/webm");
    any_crypto |= AMediaCrypto_isCryptoSchemeSupported(kSchemes[i]);
    AMediaDrm* drm = AMediaDrm_createByUUID(kSchemes[i]);
    any_drm_object |= drm != NULL;
    if (drm != NULL) AMediaDrm_release(drm);
    static const uint8_t kInit[4] = {1, 2, 3, 4};
    AMediaCrypto* crypto = AMediaCrypto_new(kSchemes[i], kInit, sizeof(kInit));
    any_crypto_object |= crypto != NULL;
    AMediaCrypto_delete(crypto);
  }
  check("AMediaDrm_isCryptoSchemeSupported is false for every scheme", !any_drm);
  check("AMediaCrypto_isCryptoSchemeSupported is false for every scheme", !any_crypto);
  check("AMediaDrm_createByUUID creates no object", !any_drm_object);
  check("AMediaCrypto_new creates no object", !any_crypto_object);
  check("AMediaCrypto_requiresSecureDecoderComponent is false",
        !AMediaCrypto_requiresSecureDecoderComponent("video/avc"));
  AMediaCrypto_delete(NULL);
  check("AMediaCrypto_delete(NULL) returns", 1);

  AMediaDrmSessionId session;
  memset(&session, 0, sizeof(session));
  check("AMediaDrm_openSession(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_openSession(NULL, &session) == AMEDIA_ERROR_INVALID_OBJECT);
  check("AMediaDrm_closeSession(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_closeSession(NULL, &session) == AMEDIA_ERROR_INVALID_OBJECT);
  const char* value = NULL;
  check("AMediaDrm_getPropertyString(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_getPropertyString(NULL, "vendor", &value) == AMEDIA_ERROR_INVALID_OBJECT);
  check("AMediaDrm_setPropertyString(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_setPropertyString(NULL, "sessionSharing", "enable") ==
            AMEDIA_ERROR_INVALID_OBJECT);
  const uint8_t* request = NULL;
  size_t request_size = 0;
  const char* url = NULL;
  check("AMediaDrm_getProvisionRequest(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_getProvisionRequest(NULL, &request, &request_size, &url) ==
            AMEDIA_ERROR_INVALID_OBJECT);
  check("AMediaDrm_setOnEventListener(NULL) is AMEDIA_ERROR_INVALID_OBJECT",
        AMediaDrm_setOnEventListener(NULL, NULL) == AMEDIA_ERROR_INVALID_OBJECT);

  // Looked up at run time, as an app probing for the entries does.
  typedef bool (*Supported)(const uint8_t*, const char*);
  Supported supported = (Supported)dlsym(RTLD_DEFAULT, "AMediaDrm_isCryptoSchemeSupported");
  check("dlsym(AMediaDrm_isCryptoSchemeSupported)", supported != NULL);
  if (supported != NULL) check("  and it is false for Widevine", !supported(kSchemes[0], "video/mp4"));
  check("dlsym(AMediaCrypto_new)", dlsym(RTLD_DEFAULT, "AMediaCrypto_new") != NULL);
  check("dlsym(AMediaDrm_verify)", dlsym(RTLD_DEFAULT, "AMediaDrm_verify") != NULL);

  if (failures == 0) printf("media_drm: all passed\n");
  return failures;
}
