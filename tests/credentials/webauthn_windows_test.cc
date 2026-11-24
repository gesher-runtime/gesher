// Copyright (C) 2026 The Gesher Authors
// SPDX-License-Identifier: Apache-2.0
// Windows host contracts for compat/devices/webauthn_win.cc (docs/passkeys.md, ADR 0092),
// against the real webauthn.dll and without showing any UI:
//  - the API version, and whether a user-verifying platform authenticator (Windows
//    Hello) is available: a runner usually has none, both answers are valid;
//  - cancellation ids are issued and distinct, and canceling an id that has no
//    ceremony does no harm;
//  - WebAuthNGetErrorName names the HRESULTs as CredentialManagerService's error
//    mapping expects;
//  - the platform credential list of a random relying party is empty, and deleting a
//    random credential id fails;
//  - a ceremony without an owner window never reaches Windows: no dialog, no result.
// On a Windows without webauthn.dll every call must answer kUnavailable.
// tests/credentials/webauthn_probe.cc covers the real ceremonies by hand.

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "webauthn_win.h"

using namespace gesher::devices;

static int failures, checks;

static void Check(bool ok, const char* what) {
  checks++;
  if (!ok) failures++;
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  fflush(stdout);
}

static unsigned long Hex(HRESULT hr) { return static_cast<unsigned long>(hr); }

static webauthn::MakeCredentialRequest SampleRequest() {
  webauthn::MakeCredentialRequest request;
  request.rp_id = L"gesher-test.example";
  request.rp_name = L"Gesher test";
  request.user_id = {1, 2, 3, 4};
  request.user_name = L"user";
  request.user_display_name = L"User";
  request.algorithms = {-7, -257};
  const char json[] = "{\"type\":\"webauthn.create\",\"challenge\":\"AAAA\",\"origin\":\"test\","
                      "\"crossOrigin\":false}";
  request.client_data_json.assign(json, json + sizeof(json) - 1);
  request.timeout_ms = 10000;
  return request;
}

int main() {
  DWORD version = webauthn::ApiVersion();
  printf("WebAuthNGetApiVersionNumber: %lu\n", static_cast<unsigned long>(version));

  bool available = true;
  HRESULT hr = webauthn::IsUserVerifyingPlatformAuthenticatorAvailable(&available);
  printf("user-verifying platform authenticator: hr 0x%08lx, available %d\n", Hex(hr),
         available ? 1 : 0);

  GUID first{}, second{};
  const GUID zero{};
  HRESULT first_hr = webauthn::GetCancellationId(&first);
  HRESULT second_hr = webauthn::GetCancellationId(&second);
  std::vector<webauthn::PlatformCredential> credentials;

  if (version == 0) {
    Check(hr == webauthn::kUnavailable && !available,
          "without webauthn.dll: no platform authenticator, kUnavailable");
    Check(first_hr == webauthn::kUnavailable && memcmp(&first, &zero, sizeof(GUID)) == 0,
          "without webauthn.dll: no cancellation id");
    Check(webauthn::CancelCurrentOperation(first) == webauthn::kUnavailable,
          "without webauthn.dll: cancel is kUnavailable");
    Check(webauthn::ErrorName(E_FAIL).empty(), "without webauthn.dll: no error names");
    Check(webauthn::PlatformCredentials(nullptr, &credentials) == webauthn::kUnavailable &&
              credentials.empty(),
          "without webauthn.dll: no platform credential list");
    webauthn::Attestation attestation;
    Check(webauthn::MakeCredential(GetDesktopWindow(), SampleRequest(), nullptr, &attestation) ==
                  webauthn::kUnavailable &&
              attestation.credential_id.empty(),
          "without webauthn.dll: MakeCredential is kUnavailable and returns nothing");
  } else {
    Check(hr == S_OK, "the platform authenticator query succeeds (either answer is valid)");
    Check(first_hr == S_OK && second_hr == S_OK, "cancellation ids are issued");
    Check(memcmp(&first, &zero, sizeof(GUID)) != 0 && memcmp(&first, &second, sizeof(GUID)) != 0,
          "cancellation ids are non-zero and distinct");
    HRESULT cancel = webauthn::CancelCurrentOperation(first);
    printf("WebAuthNCancelCurrentOperation without a ceremony: 0x%08lx\n", Hex(cancel));
    Check(cancel != webauthn::kUnavailable, "canceling an id without a ceremony reaches Windows");
    HRESULT again = webauthn::GetCancellationId(&second);
    Check(again == S_OK, "an id is still issued after that cancel");

    // webauthn.h's table for WebAuthNGetErrorName; CredentialManagerService relies on it.
    Check(webauthn::ErrorName(S_OK) == L"Success", "S_OK is Success");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x8009000FL)) == L"InvalidStateError",
          "NTE_EXISTS is InvalidStateError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x80090029L)) == L"ConstraintError",
          "NTE_NOT_SUPPORTED is ConstraintError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x80090027L)) == L"NotSupportedError",
          "NTE_INVALID_PARAMETER is NotSupportedError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x80090036L)) == L"NotAllowedError",
          "NTE_USER_CANCELLED is NotAllowedError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x80090011L)) == L"NotAllowedError",
          "NTE_NOT_FOUND is NotAllowedError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x80090035L)) == L"NotAllowedError",
          "NTE_DEVICE_NOT_FOUND is NotAllowedError");
    Check(webauthn::ErrorName(static_cast<HRESULT>(0x800705B4L)) == L"NotAllowedError",
          "ERROR_TIMEOUT is NotAllowedError");
    Check(webauthn::ErrorName(E_FAIL) == L"UnknownError", "E_FAIL is UnknownError");

    // A relying party nobody registered: this run's cancellation id makes it unique.
    wchar_t rp[80];
    swprintf(rp, 80, L"gesher-%08lx%04x%04x.invalid", static_cast<unsigned long>(second.Data1),
             static_cast<unsigned>(second.Data2), static_cast<unsigned>(second.Data3));
    HRESULT list = webauthn::PlatformCredentials(rp, &credentials);
    printf("WebAuthNGetPlatformCredentialList(%ls): 0x%08lx, %u credentials\n", rp, Hex(list),
           static_cast<unsigned>(credentials.size()));
    std::vector<uint8_t> random_id(reinterpret_cast<const uint8_t*>(&first),
                                   reinterpret_cast<const uint8_t*>(&first) + sizeof(GUID));
    random_id.insert(random_id.end(), reinterpret_cast<const uint8_t*>(&second),
                     reinterpret_cast<const uint8_t*>(&second) + sizeof(GUID));
    HRESULT deleted = webauthn::DeletePlatformCredential(random_id);
    printf("WebAuthNDeletePlatformCredential(random id): 0x%08lx\n", Hex(deleted));
    if (version >= 4) {
      Check(list == S_OK && credentials.empty(),
            "the platform credential list of a random relying party is empty");
      Check(FAILED(deleted), "deleting a random credential id fails");
    } else {
      Check(list == webauthn::kUnavailable && deleted == webauthn::kUnavailable,
            "before API version 4 there is no platform credential list");
    }
  }

  // No owner window: no dialog, no call into Windows, no result.
  Check(webauthn::ForegroundWindowOf(4) == nullptr,
        "the System process owns no foreground window");
  Check(webauthn::ForegroundWindowOf(0) == nullptr, "pid 0 owns no foreground window");
  webauthn::Attestation attestation;
  attestation.credential_id = {9};
  Check(webauthn::MakeCredential(nullptr, SampleRequest(), nullptr, &attestation) ==
                webauthn::kNoOwner &&
            attestation.credential_id.empty() && attestation.authenticator_data.empty() &&
            attestation.attestation_object.empty(),
        "MakeCredential without an owner window is kNoOwner and returns nothing");
  webauthn::GetAssertionRequest get;
  get.rp_id = L"gesher-test.example";
  get.client_data_json = SampleRequest().client_data_json;
  webauthn::Assertion assertion;
  assertion.signature = {9};
  Check(webauthn::GetAssertion(nullptr, get, nullptr, &assertion) == webauthn::kNoOwner &&
            assertion.signature.empty() && assertion.credential_id.empty(),
        "GetAssertion without an owner window is kNoOwner and returns nothing");

  printf("webauthn windows contracts: %d/%d passed\n", checks - failures, checks);
  return failures == 0 ? 0 : 1;
}
